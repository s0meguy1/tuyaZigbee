# The light-show engine, builds 36-37

The on-device engine behind `light_show*` in zigbee2mqtt. This is the reference
for anyone writing a show against it; the consumer-side summary lives on the
Home Assistant host (`MOES_CUSTOM_FIRMWARE_CAPABILITIES_READ_FIRST.md`), the
wire format in `light/moes_fxwire.h`, the renderers in `light/light_effects.c`.

Build 36 introduced everything below; build 37 is a one-constant fix to the
way reports are framed, and is the build to run. It was written against the
first real show
(`script.self_destruct_sequence` v3, 19 fixtures, 2026-09-02) and its wish
list. That show needed 36 group frames and 19 unicasts on a transport measured
at ~1.55 group frames per second, and half its design effort went into spacing
frames. The point of this build is that the same show is one upload and one
trigger.

## What changed from build 35, in one table

| wish-list item | build 36-37 |
|---|---|
| 1. fixture index + effects that use it | `light_show_index` (persisted) + `light_show_spread`; phase now really offsets every periodic effect |
| 2. deferred execution | `delay` in a `light_show_cue` frame: every member applies the frame `delay` ms after receipt |
| 3. on-chip cue list | `light_show_cue_list` (32 entries) + `light_show_cue_run` |
| 4. effect brightness independent of ZCL level | `light_show_level` (255 = follow) |
| 5. effects surviving a Level command | `light_show_takeover: hold` |
| 6. run-for-N-then-stop | `light_show_duration` |
| 7. decreasing transitions | **not changed**: the level logic ramps correctly on the host in both directions (see below); needs a real PWM measurement |
| 8. the two stop paths agree | yes: show colour lives in the engine, every stop returns to the fixture's own ZCL state |
| 9. wave/chase/color_step/snow | `wave` does follow hue (its hue swings ±85°, which is why it drew more than solid); the other three own their colour and are now documented as such |
| 10. burst density vs speed | `light_show_density` (percent of slots), speed then sets the in-burst flash rate |
| 11. readable state | the chip reports every value after a unicast write and on `/get` |
| 12. stage colour while off | show hue/saturation/level can be written to a dark fixture; nothing shows until an effect runs |
| timeline drift (F14) | the timeline now runs on the system clock, not a callback count |
| "please don't regress" list | `onWithTimedOff`, per-fixture randomness, live datapoint writes, restart-on-resend, own-colour effects at full: all kept |

## Model

The engine owns a small **show state**, all of it settable over the Tuya
cluster and all of it reported back:

| key | wire | default | meaning |
|---|---|---|---|
| `light_show` | effect enum 0..14 | `stop` | which renderer owns the LEDs; `stop` releases them |
| `light_show_speed` | 1..100 | 50 | rate; applies live without restarting |
| `light_show_phase` | 0..359° | 0 | this fixture's own offset into the effect's period |
| `light_show_index` | 0..254, 255 none | none | **persisted in NV**; set once per fixture, unicast only |
| `light_show_spread` | 0..359° | 0 | extra phase per index step |
| `light_show_hue` | 0..359°, 360 follow | follow | show colour; follow = the fixture's own colour (white when it is in colour-temperature mode) |
| `light_show_saturation` | 0..100% | 100 | show saturation |
| `light_show_level` | 0..254, 255 follow | follow | show brightness; follow = the fixture's own level, 0 when it is off |
| `light_show_takeover` | release / hold | release | what a ZCL command does to a running effect |
| `light_show_duration` | ms, 0 forever | 0 | an effect stops itself after this |
| `light_show_density` | 0 auto, 1..100% | 0 | burst: share of one-second slots that spark |
| `light_show_cue_run` | stop / run / loop | stop | the sequencer |

**Nothing is reset by a stop.** The state is always exactly what was last
written (or what a cue entry set). That is deliberate, and it means a show must
clean up after itself explicitly, in one frame:

```json
{"light_show_cue": {"effect":"stop", "hue":360, "saturation":100, "level":255,
                    "duration":0, "density":0, "speed":50, "phase":0,
                    "spread":0, "takeover":"release", "cue_run":"stop"}}
```

Reset **every** parameter you touched, not just the visible ones. A leftover
`duration` is the trap: it is invisible until someone starts an unrelated
effect on that fixture weeks later and it stops itself a few seconds in. A
leftover `takeover: hold` is the other one, because ordinary on/off and
brightness commands then stop rendering that fixture. `index` is the exception
worth keeping, since it describes where the fixture is and is persisted for
that reason. A loaded cue list is harmless once `cue_run` is `stop`, and is
cleared by uploading a new list or by any power cycle.

Effective phase = `(phase + index × spread) mod 360`. With 19 fixtures indexed
0..18 in spatial order and spread 19, one group broadcast of `pulse` is a
brightness wave from fixture 0 to 18; `chase`/`rainbow` a colour wheel across
the house; `strobe` a running strobe. The random effects (`burst`, `twinkle`,
`lightning`, `candle`, `fire`, `snow`) ignore phase: each fixture draws its own
seed per run, so a room scatters from one broadcast with nothing to set.

### Colour and level are show state, not fixture state

Builds 27-35 wrote `light_show_hue` into the ZCL colour attributes. That is
what turned an occupied kitchen blue during a pre-show, and why a `stop`
left a red fixture behind while a Level command restored white (F4/F5/F10).
Now:

* a hue/saturation/level write to an idle fixture changes nothing visible; the
  values are staged and the next effect uses them;
* a write to a running effect applies on its next frame, no restart;
* every stop (datapoint `stop`, a ZCL command under `release`, a duration
  running out, a cue entry with `effect: stop`) renders the fixture's own
  on/off, level and colour again.

An **explicit level** renders an effect at that brightness regardless of the
fixture's on/off state - sparks at full on a dark room, no flare first. It is
also a master fader over the own-colour effects (`fire`, `explode`, `candle`,
`snow`, `lightning`, `rainbow`, `chase`, `color_step`), which otherwise render
at full from any state, as the blasts rely on. Level 0 is a blackout the effect
survives. `fade` (in the same cue frame or cue-list entry) ramps the level
linearly.

### Takeover policy

`release` (default): any on/off, level, colour or scene command stops a running
effect and takes the output - the right thing when a person grabs the colour
picker. `hold`: the effect keeps the LEDs; level/colour commands update the
fixture's own attributes underneath it (visible immediately wherever the show
follows the fixture, and what the fixture lands on at stop); an OFF blacks the
effect out and an ON brings it back, with no re-arm. `hold` persists until you
change it, so end a show with `takeover: release`.

## Frames

One Tuya frame can carry several datapoints and is applied atomically:
parameters first, then the effect (a restart, even of the same effect), then
`cue_run`. Any invalid value rejects the whole frame. With `delay`, the frame
is applied `delay` ms after receipt on every member; a newer frame, delayed or
not, replaces a pending one. So:

```json
{"light_show_cue": {"effect":"burst","hue":220,"saturation":100,"level":254}}
```
lights blue sparks on a dark room in one frame (was FLARE + BLUE_MIX + BURST),
and
```json
{"light_show_cue": {"effect":"explode","speed":100,"delay":400}}
```
arms the blast 400 ms ahead so that group jitter does not smear it.

The composite carries `effect speed phase spread hue saturation level fade
delay duration takeover density cue_run`. A complete frame is under 76 bytes,
the converter refuses anything larger.

## Cue list

Up to 32 entries of `{t, effect, speed, hue, saturation, level, fade}`; `t` in
ms from the start, entries in time order, omitted fields keep their value,
`effect: stop` releases the output while the sequence continues. Uploaded six
per frame (`light_show_cue_list`, so 32 entries is six frames, sent before the
show), started with `light_show_cue_run: run` (or `loop`, where the last
entry's `t` is the loop length and its settings apply at the wrap). `stop`
aborts the sequence and the effect; so does `light_show: stop`. The list is
RAM only: re-upload after a power cut. A list with a lost frame refuses to run
(`/get` shows `light_show_cue_count` and `light_show_cue_run` stuck at `stop`).

The ceiling half of the self-destruct show, as a cue list (P1 = 23.19 s,
det_lead folded into the trigger's `delay`):

```json
{"light_show_cue_list": [
  {"t": 0,     "effect": "burst",   "hue": 220, "saturation": 100, "level": 254},
  {"t": 1450,  "effect": "twinkle"},
  {"t": 12500, "hue": 0},
  {"t": 13250, "speed": 1, "effect": "strobe"},
  {"t": 15700, "speed": 3},  {"t": 17300, "speed": 6},  {"t": 18900, "speed": 12},
  {"t": 20000, "speed": 20}, {"t": 21000, "speed": 32}, {"t": 21800, "speed": 55},
  {"t": 22500, "speed": 100},
  {"t": 23190, "effect": "explode", "speed": 100},
  {"t": 23950, "effect": "fire"},
  {"t": 25190, "speed": 55}, {"t": 27190, "speed": 35},
  {"t": 28500, "speed": 100},
  {"t": 29800, "effect": "explode"},
  {"t": 30600, "effect": "fire"},
  {"t": 31600, "speed": 40}, {"t": 32500, "speed": 12},
  {"t": 33600, "effect": "stop", "level": 0, "fade": 1000}
]}
```
then, on the beat: `{"light_show_cue": {"cue_run": "run"}}` - and the audio is
the only thing left to synchronise. The per-fixture strobe rates of the old
pre-show (19 unicasts) become `light_show_spread` on a strobe, or simply
different `index` values, and the white pops stay what they were: one
`onWithTimedOff` frame each.

## Reports

The chip reports every value (a Tuya dataReport, decoded by the converter into
the same keys) after a **unicast** write, after a `/get`, and when it changes
state on its own after a unicast started it (a duration ran out, a cue list
ended). Group frames never trigger reports: nineteen fixtures answering every
cue would take the airtime the next cue needs. After a group write the
published state is what was asked for, exactly as before.

```
zigbee2mqtt/<device>/get  {"light_show": ""}
```
answers with all of it, including `light_show_cue_count`.

## What is not in this build

* **Item 7, decreasing `moveToLevel` transitions.** `tools/build19_hosttest`
  plus a host harness running the real `light_applyUpdate` arithmetic ramps
  254→1 over 30 ticks as 245, 237, 228 … 9, 1 in both directions, so the level
  logic is not where the hold-then-snap comes from. The field measurement was
  a mains wattmeter with ~1-1.5 s lag; the next step is a PWM-pin capture on
  the bench (a Pi GPIO on one LED pin), not a code change on a guess.
* Cue lists do not persist across a power cut, and an entry cannot carry
  phase/spread/takeover/density (set those in a frame before `cue_run`).
* Reports are unicast-only by design (see above).

## Wire format

Command 0x00/0x01 (dataRequest/dataResponse), payload `seq u16 BE` then
repeated `dp u8, type u8, len u16 BE, data`. Values are folded big-endian from
1 to 4 bytes. 0x03 (dataQuery, no payload) is answered with 0x02 (dataReport,
same layout, compact 1/2-byte values).

**The report must carry the server-to-client direction bit.** zigbee-herdsman
keeps dataRequest and dataQuery in the cluster's `commands` and dataResponse,
dataReport and the status reports in its `commandsResponse`, and picks the
table from that bit. With the wrong bit the report is delivered as an undecoded
`raw` frame and zigbee2mqtt publishes nothing, with no error logged anywhere.
Build 36 had it backwards and build 37 fixed it. Datapoint ids: `light/moes_fxwire.h`.
The exact parser and report encoder run on the host in
`tools/fxwire_hosttest`; the timing maths in `tools/fxrate_hosttest`.
