# Diagnosing a fault this firmware cannot report

Written 2026-08-30, immediately after the watchdog-tighten reset loop (build 24)
took roughly a day and produced **four** confident wrong diagnoses before it fell
in under ten minutes of the right measurement. The wrong turns were not bad luck;
they were the same mistake four times, and it is a mistake worth naming.

Read this before debugging any "impossible" behaviour on this device.

---

## 1. The one mistake that caused all of them

> **An instrument that cannot observe the failure was used as evidence the
> failure was absent.**

Every wrong diagnosis in that episode was an instance of it:

| Claim | Instrument | Why it could not support the claim |
|---|---|---|
| "It is not rebooting" | zero `device_announce` in z2m | a reboot on this firmware emits **no announce at all** |
| "It is healthy on bench 3V3" | PC sampling shows a healthy spread | a device that reboots every 15 s **runs perfectly in between**, so the spread looks fine |
| "Running an effect reboots it" | marker went missing after starting an effect | the marker write was **never confirmed to have landed** |
| "The mains PSU is failing" | fault seen on mains, not on bench 3V3 | the bench window was also the **unjoined** window, and the trigger was gated on being joined |

Before trusting a negative result, ask: *if the fault I am ruling out were
happening right now, would this instrument show it?* On this device the answer
is usually no. z2m in particular is blind: during a 15 s reset loop it logged
`leave_count: 0`, `network_address_changes: 0` and **zero** errors.

## 2. Detect a reboot over the air: the `.data` marker

No programmer needed, works on a device in the ceiling.

`g_zcl_tuyaFxAttrs` (cluster `0xEF00`) is a `.data` object initialised to
`{effect 0, speed 50, phase 0}`, and the Tuya datapoint handler is its only
writer. Nothing in the firmware ever writes `speed` back to 50, so C startup
re-running `.data` initialisation is the only way it returns to 50.

    write speed = 99   (raw ZCL, status-checked)   ->  read attr 2  ==  99   alive
    ... wait ...                                   ->  read attr 2  ==  50   REBOOTED

**Always confirm the write landed before timing anything from it.** An
unconfirmed write produced wrong diagnosis #3 above, within an hour of the
technique being invented.

Use raw ZCL with the default response re-enabled, because `sendDataPoints()`
sets `disableDefaultResponse: true` and hides the device's real ZCL status:

    {"zclcommand":{"cluster":"manuSpecificTuya","command":"dataRequest",
      "payload":{"seq":1,"dpValues":[{"dp":111,"datatype":2,"data":[0,0,0,99]}]},
      "options":{"disableDefaultResponse":false},"check_status":true}}

and **run a control that the firmware must reject** (effect id >= MOES_EF_MAX,
or an unknown datapoint). A silent pass proves nothing unless a known-bad frame
visibly fails.

## 3. Since build 24: just read the reset reason

    {"read":{"cluster":"manuSpecificTuya","attributes":[4]}}

* `0` - the last reset was **not** one of our exceptions: a real power cycle, or
  a watchdog/hardware reset.
* non-zero - subtract 1 for the `SYS_EXCEPTTION_*` code (`proj/os/ev.h`).

That asymmetry is the useful part. "Rebooting repeatedly **with 0 here**" points
at the watchdog or hardware; a code points at a specific `ZB_EXCEPTION_POST`
site. See `light/moes_bootmark.h` for why it is an analog register and not NV.

## 4. Measure the PERIOD, not the presence

A fault that merely "stops" after a change is weak evidence - plenty of
unrelated changes stop things. A fault whose **period tracks a constant you
moved** is close to conclusive.

That is how the watchdog bug was settled. Rather than deleting the suspect call,
its trigger constant was moved 15 s -> 60 s and the reset period was re-measured:

    settle 15 s -> 15.11 s (n=14)
    settle 60 s -> 59.50 s (n=5)
    ratio 3.938 vs a constant ratio of 4.000

Design the experiment so **both** outcomes are informative before running it. If
the period had stayed at 15 s, the hypothesis was dead and that would have been
just as valuable.

## 5. Verify at the artifact, not at the build log

A successful build does not prove your change is in the image. Find the compiled
constant in the `.bin`:

    15 s gate = 15000*1000*16 = 240,000,000 = 0x0E4E1C00 little-endian

The b22 image contained exactly one instance and zero of the 60 s value; b23 was
the exact inverse. That check also caught an error in a source comment - the
predicted literal was absent because `clock_time_exceed()` scales by **16**
(16 MHz system tick), not by the 48 MHz CPU clock. Note the *single* occurrence
also proved the constant had exactly one runtime consumer.

## 6. Bench rules that each cost an hour

1. **Only true power removal boots this board.** Programmer soft reset and RST
   pulses each fail *differently and convincingly* (`PC=0x0`, a loop in
   `BSS_CLEAR`, a park in `start_suspend`). This manufactured a false "build 19
   does not boot".
2. **SWire activity deafens the radio.** Never instrument over SWire and test
   over the air in the same window.
3. **Take ONE SWire read of timer-driven state.** Repeated `-c` stalls freeze the
   application's own `ev_timer` callbacks, which looks exactly like a firmware
   bug that stopped a timer.
4. **The bench 3V3 relay is not a power cycle** while SWire is clipped on. The
   MCU stays alive on parasitic power through SWS/RST and only the external
   flash loses power - an MCU running on dead flash, worse than either extreme.
   Verified by the chip answering `ChipID: 0x5562` while the flash returned
   `Error get Flash JEDEC ID! (102)`.
5. **`Activate ... Error!` usually means the TARGET needs a power cycle, not a
   bad clip.** SWire activation failing on every timing (20/50/100/200 ms) looks
   exactly like a loose connector, and on 2026-08-30 it was diagnosed as one
   twice - the second time wrongly, after asking the user to re-seat three clips
   that were fine. A single power cycle of the fixture restored it immediately.
   This is the same family as rule 1: a TLSR8258 in certain states will not
   respond to anything until power is actually removed. **Try a power cycle
   before touching the wiring.**
6. **A `TlsrPgm` read leaves the CPU halted.** `-s` (stop) and `-c` (stall) are
   *pre-processing* flags with no implicit resume, so a verification readback
   ends with the chip stopped. A device that "does not come up" after a flash is
   usually still halted from your own last command - release it with
   `-t 50 -a 20 -r -m` and re-check **before** concluding the image is bad. This
   produced a false "build 24 did not boot" within minutes of the build that
   actually fixed the reset loop; the board was alive on the mesh one command
   later.
7. z2m's published state after a `set` is **optimistic**, not evidence.

## 7. Do not skip the on-device patterns

The light-show engine (`light/light_effects.c`, datapoints `0x6E` effect,
`0x6F` speed, `0x70` phase) is the most sensitive end-to-end test this device
has, and it is the thing users actually notice. It exercises datapoint parsing,
the ZCL handler, `ev_timer` scheduling, the renderer and the PWM output stage in
one command, continuously, with a human-visible result.

**Run it after any build that touches timing, the scheduler or the output
stage.** Two specific traps:

* An effect started while a level/colour transition is still stepping is
  cancelled a few seconds later, because `light_applyUpdate*()` calls
  `light_fresh()` on every transition step. Send the effect **alone** or the
  test is measuring your own command sequence.
* "It ran for a few seconds then went back to white" is the reset-loop
  signature, not an effect bug. Check the marker (section 2) before touching
  the engine - that exact symptom was blamed on effects for hours.

Rates worth knowing: the engine ticks at `MOES_EFFECT_TICK_MS` (20 ms since
build 22), strobe is mapped by **frequency** (1.0-12.0 Hz over speed 1-100), and
the smooth effects scale a cycle length. `tools/fxrate_hosttest` executes the
real `moes_fxrate.c` on the host, so rate regressions are catchable without
hardware.

## 7a. Trust the human's eyes over your own instruments

Three findings on 2026-08-30 came from the user reporting what the light did,
after tooling had said the opposite:

* "It strobed blue the entire time, no changes" - four colour commands had been
  REJECTED by z2m (`ERR_OUT_OF_RANGE`) because the test sent saturation 254 when
  z2m's units are 0-100. Its own hue/sat->xy conversion emitted negative
  coordinates and it never transmitted. The device had not received a colour
  command in a day.
* "magenta (or light orangeish)" - a hand-written datapoint payload of
  `[0, 0, 0, 300]` silently truncated to 44, so "magenta 300 deg" was
  transmitted as 44 deg, orange. **Never hand-write a multi-byte datapoint
  value**; encode with `value.to_bytes(4, "big")` and range-check first. 359 deg
  would have arrived as 103 deg, green. The z2m converter is unaffected -
  `tuya.sendDataPointValue()` encodes correctly - so this only bites raw test
  scripts.
* "a really really low light orange strobe" - `fxCurHsv()` takes brightness from
  the level attribute ONLY while the light is on, else 0, floored at 0x20. A
  test that turned the light off between phases rendered the next effect at
  minimum brightness. Consequence for scenes: a "mostly dark" room must come
  from a mostly-dark EFFECT, not from turning the fixtures off.

The pattern: every one of these was a fault in the test rig or the wire format
that no device-side instrument could see, and the only detector that worked was
a person looking at the light. When a human observation contradicts your
tooling, suspect the tooling first.

## 8. Order of attack for an undiagnosable fault

1. Reproduce it and **measure a number** - period, count, rate. Not "it flickers".
2. Read attribute `0x0004`: exception, or watchdog/power.
3. Change one *supply-side* variable at a time (mains vs bench 3V3, LED load on
   vs off). These are cheap and exclude whole classes.
4. Grep the config for a constant matching the number from step 1. In this
   codebase the guilty constant was the **only** one of its value in the tree.
5. Move that constant and re-measure. Period tracks it -> causal.
6. Only then read code looking for a mechanism, and if two mechanisms both fit,
   write a fix that closes both rather than guessing between them.
