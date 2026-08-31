# Fallback design — making this firmware unable to lock itself out

**Date:** 2026-08-15
**Code:** `light/moes_rescue.{c,h}`, hooks in `light/tuyaLight.c`,
`light/zb_appCb.c`, `light/light_effects.c`, `light/zcl_tuyaMfg.c`,
`common/factory_reset.c`
**Test:** `tools/rescue_hosttest/` — runs on a PC, no hardware
**Off switch:** `MOES_RESCUE_ENABLE 0` in `light/moes_rescue.h` restores the
previous behaviour exactly (verified to build)

---

## Build 19 current contract (supersedes the historical rescue-mode sections below)

The detailed design below records the pre-Build-18 rescue-mode experiment. It
is retained for incident history only; its tables describing skipped init,
output, effects, identify, persistence, and manufacturer commands are **not**
the current firmware contract.

Build 19 has these rules:

- The boot-storm flag is advisory only. It may alter only OTA query cadence and
  the join-blink pattern. It never gates application init, factory-reset
  handling, persistence, reporting, normal light output, identify/blink,
  effects, or manufacturer commands.
- The liveness fuse arms after every successful BDB init callback, including a
  factory-new/unjoined boot. A healthy commissioning scheduler advances its
  cooperative ticker; a pre-join scheduler/timer wedge reaches the independent
  hardware-timer reset bound.
- Plain ZCL `On` cancels any pending `On With Timed Off` timer and clears its
  transient `OnTime`/`OffWaitTime` state. Those fields are deliberately absent
  from the on/off NV record and therefore never survive a boot.
- The extended-colour endpoint exposes read/reportable `CurrentX` and
  `CurrentY`, backed by the exact coordinates from `MoveToColor`. No unproven
  XY-to-PWM conversion is claimed; physical colour/output is not established
  by Zigbee attributes alone. Continuous and step XY commands remain
  explicitly unsupported rather than reporting a successful no-op.
- The vendored event-timer list walks have a bounded fail-closed guard. A
  detected overlong/cyclic list posts the SDK timer exception; the registered
  production handler resets synchronously, while an early pre-registration
  detection takes a direct reset fallback. It is defensive hardening, not
  proof of the physical root cause of any observed timer stall.

**Verification rule:** radio connectivity, command acknowledgements, and ZCL
reads do not verify LED output. Call a firmware output change verified only
after visual confirmation on a fixture with LEDs or direct PWM-register
evidence. Build 19 source/host tests are not silicon proof, mains proof, or a
root-cause finding.

---

## HISTORICAL PRE-BUILD-18 RESCUE-MODE DESIGN — NOT THE CURRENT FIRMWARE CONTRACT

Sections 0–9 below are retained as incident/forensic history. They describe
the superseded minimal-output rescue mode and must not be read as Build 19
behaviour; the current contract is the one above.

### 0. The answer up front

> *If the next image is bad anyway, do we get it back without a ladder?*

**Usually, but not always, and the exceptions are specific.**

**Yes**, for a bug in application code that runs after `stack_init()` returns.
That covers every bug this project has actually hit or that this audit found:
the `factoryRst_init()` ordering bug, the phantom-button factory reset
(`AUDIT_FINDINGS.md` A-1), the effect engine, the colour path, the Tuya
cluster handler, the reporting table. A light with any of those comes up in
rescue mode within about a minute and stays reachable indefinitely.

**No**, for four things, and the fourth is now field-proven:

1. **A fault before the probation counter is written** — i.e. inside
   `led_init()`, `hwLight_init()`, or `zb_init()`. The counter never advances,
   so rescue mode never latches. See §4.1.
2. **Anything that stops the radio working.** Rescue mode does not repair a
   radio; it stops competing with one.
3. **An image that does not execute at all.** The bootloader's CRC check
   catches a corrupt image, but a valid image that faults in `cstartup` before
   `main()` is beyond reach.
4. **A hang — code that stops making forward progress without resetting.**
   Build 04 did exactly this on 2026-08-15: it answered a full configure pass,
   then a flash/NV busy-wait wedged with interrupts off, and the light stayed
   radio-dead and PWM-latched forever (`HANG_FINDINGS.md`). Rescue mode never
   engaged — **probation counts reboots, and a hang produces none.** On a device
   whose only recovery path is OTA, a hang is strictly worse than a reset loop,
   because a reset loop at least presents a rescue-mode target.

§4 quantifies the first three. The fourth — the hang — is not covered by rescue
mode at all; it is closed by the hardware watchdog (§5), which is **applied as
of build 06**. The residual application-level risk otherwise remains
concentrated in `libzb_router.a` and ~20 lines of pre-stack init, which is the
right place for it — those are the parts we do not change between releases.

---

### 1. How it works

One byte, `MOES_NV_ITEM_BOOT_PROBATION 0x71`, in `NV_MODULE_APP`. It counts
**consecutive boots that never reached a stable state**.

```
                        ┌──────────────────────────────────────────┐
   power on             │  user_init()                             │
       │                │    led_init()                            │  ← not covered (§4.1)
       │                │    hwLight_init()                        │  ← not covered
       ▼                │    lightFx_init()                        │  ← not covered
   stack_init()  ───────│    stack_init()   [nv_init() lives here] │  ← not covered
       │                │                                          │
       ▼                │    moes_rescueBootCheck()   ◄── read+1   │
   count = read()       │                                          │
       │                │    user_app_init()   ZCL + OTA + GP      │
       ├─ ≥ 6 ? ────────│    factoryRst_init()      skipped in rescue
       │      │         │    light_adjust()         skipped in rescue
       │      └─► RESCUE│    bdb_init()                            │
       │         no write└─────────────────────────────────────────┘
       ▼
   write(count+1)                joined?  ──► 20 min ──► write(0)
                                                          │
                                             3-cycle gesture ──► write(0)
```

**Incremented** once per boot, in `moes_rescueBootCheck()`, called from
`user_init()` immediately after `stack_init()` returns. That is the earliest
point at which NV is real — `nv_init()` is not in the SDK sources, it lives
inside `libzb_router.a` and runs from `zb_init()`. Getting this wrong is what
cost a fixture on 2026-08-14, so the call sits directly under the comment that
records it.

**Cleared** when either:

* the light has been **joined** continuously for `MOES_RESCUE_STABLE_MINUTES`
  (20). A one-minute repeating `TL_ZB_TIMER` counts down in RAM and re-checks
  `zb_isDeviceJoinedNwk()` on every tick; falling off the network *restarts*
  the countdown rather than pausing it. Started from
  `zbdemo_bdbInitCb(joinedNetwork)` and `zbdemo_bdbCommissioningCb(SUCCESS)`,
  and idempotent.
* the user completes the **3-power-cycle factory-reset gesture** —
  `factoryRst_handler()` calls `moes_rescueClear()` immediately before
  `zb_factoryReset()`.

**Latched** at `MOES_RESCUE_FAIL_THRESHOLD` (6). The first rescue boot is
therefore boot **7** of a loop; at ~10 s per cycle that is a little over a
minute.

### What rescue mode does *not* do

| skipped | why |
|---|---|
| `factoryRst_init()` | three NV operations and a `TL_ZB_TIMER` on the boot path; also the source of two historical reset loops |
| `factoryRst_handler()` in `app_task()` | can call `zb_factoryReset()` |
| `light_adjust()` | pulls in `tuyaLight_colorInit()` → `light_applyUpdate()` → `light_fresh()` → the whole colour/level/effect machinery |
| `tuyaLightAttrsChk()` and the attribute-store timer | three NV writes on a timer |
| the effect engine | `lightFx_start()` refuses anything but `MOES_EF_STEADY`; the 25 fps timer never runs |
| cluster `0xEF00` commands | the only network-reachable way to start an effect; the cluster stays *registered* so z2m keeps matching the device and therefore keeps offering OTA |
| **all ZCL-driven output updates** | `light_fresh()` returns immediately. This is the single choke point for on/off, level, hue, saturation, colour temperature and scene recall, so blocking it there is what makes "the output stage is written exactly once, at boot" true. The attributes still update, so reads and reports stay truthful — the light simply does not act on them |
| the identify blink | `light_blink_start()` returns immediately; it would otherwise drive `hwLight_onOffUpdate()` on a timer behind `light_fresh()`'s back, and the steady dim-white output *is* the diagnostic |
| the key scanner | already off on this board (`HAVE_NET_BUTTON 0`) |

### What it still does

`stack_init()`, `user_app_init()` (ZCL, the endpoint, OTA, green power),
`bdb_init()`, and `report_handler()`. Plus two positive actions:

* **A visible marker.** Instead of `light_adjust()`, rescue mode writes the
  output stage directly: `moes_outSet(0, 0, 0, 0x40, 0)` — dim cool white,
  fixed. A light that comes up dim white and ignores every command is in
  rescue mode, not dead. That distinction is worth a lot when someone is
  deciding whether to fetch a ladder.
* **A shorter OTA poll.** `MOES_RESCUE_OTA_QUERY_SECONDS` (10 min) instead of
  `MY_OTA_PERIODIC_QUERY_INTERVAL` (6 h). A light that has told us it cannot
  run its own firmware should be asking for a replacement often.

---

### 2. What it depends on

Deliberately almost nothing:

1. `nv_flashReadNew()` / `nv_flashWriteNew()` on one byte in our own module.
2. `zb_isDeviceJoinedNwk()`.
3. One `TL_ZB_TIMER`.

No effect engine, no colour code, no `moes_flashcfg` config, no PWM (the
marker write is optional and failure-tolerant — it is five register writes),
no floating point, no allocation. `moes_rescue.c` is 160 lines including
comments.

The requirement in `AI_AUDIT_BRIEF.md` §4 was "if it needs NV, the radio stack
and the OTA cluster and nothing else, good". That is what it needs.

---

### 3. Bounding flash wear (requirement 5)

An NV write on every boot is fine at normal rates and not in a reset loop, so
the counter is written **at most `MOES_RESCUE_FAIL_THRESHOLD` times, ever, per
episode**:

* boots 1–6 of a loop write the counter once each;
* boot 7 **latches and deliberately does not write** — the counter is already
  at the threshold, and a light that keeps resetting must not keep erasing
  sectors while it does it;
* every boot from then on writes nothing.

And because rescue mode also skips `factoryRst_init()` (which writes twice per
boot) and the attribute-store timer, **a latched light in a permanent reset
loop performs zero NV writes**. It is quieter on flash than a healthy light.

This is asserted in the host test: 200 consecutive unstable boots produce
exactly 6 writes.

In normal operation the cost is 2 writes per boot (one to increment, one to
clear 20 minutes later), against the 2 per boot `factoryRst_init()` already
performs. `NV_MODULE_APP` is a 4 KB sector, double-buffered, with ~63 index
slots — so a sector erase roughly every 15 boots. At any plausible power-cycle
rate that is decades.

---

### 4. Failure modes

### 4.1 A fault before `moes_rescueBootCheck()` — NOT COVERED

The uncovered window is:

```c
led_init();          /* two drv_gpio_write() calls to pin NULL - no-ops here */
hwLight_init();      /* moes_flashCfgLoad() (memset + constants), drv_pwm_init(),
                        5 × gpio_set_func + pwmInit + drv_pwm_start */
lightFx_init();      /* memset */
stack_init();        /* zb_init() - prebuilt, opaque */
```

The first three were read line by line during this audit and contain no NV
access, no timers, no radio and no loops. The fourth is `libzb_router.a` and
cannot be audited.

**A crash in there is unrecoverable and no application-level mechanism can
change that.** The counter has not been touched, so it never reaches the
threshold. Mitigation is procedural, not technical:

* keep that window minimal — nothing new goes above `stack_init()` without a
  specific reason;
* the bench soak (`OTA_TEST_PLAN.md`) exercises exactly this window, because
  it is the first thing that runs.

### 4.2 The radio never joins

Rescue mode latches (the stable clock never starts, so the counter climbs),
the light comes up minimal, and it retries `bdb_networkSteerStart()` on the
normal schedule. If the radio itself is broken, nothing here helps.

### 4.3 The counter cannot be read

`moes_rescueBootCheck()` treats any non-`NV_SUCC` as zero, so the light boots
normally. Degrades to the pre-rescue behaviour. Asserted in the host test.

### 4.4 The counter cannot be written

The count never advances, rescue mode never latches, and the light behaves
exactly as it did before this mechanism existed. This is the one *silent*
degradation: a light whose APP NV module is unwritable has no fallback and
nothing says so. Accepted — the alternative (refusing to boot normally when NV
is unwritable) would turn a working light into a broken one. Asserted in the
host test.

### 4.5 The counter reads back as garbage

Clamped to the threshold, which means **rescue mode**. The clamp direction is
the opposite of `factory_reset.c`'s on purpose: there, a garbage-high value
must not trigger a reset, so it clamps to 0; here, a garbage-high value causing
an unnecessary rescue boot is harmless and self-heals after one 20-minute
stable run, whereas a garbage value *suppressing* rescue mode would not be.
**Every failure of this counter ends in either rescue mode or a normal boot —
never in a state where OTA is unavailable.** Asserted in the host test
(`0xFF` → rescue, no write).

### 4.6 A healthy light is driven into rescue mode by repeated power cycles

Someone working on a circuit could flick a breaker seven times without ever
letting a light run 20 minutes. It would come up in rescue mode: dim white, no
effects, no colour control. Then the next boot after a 20-minute stable run
clears it and everything returns.

That is a mild, self-healing, visible degradation. It is the price of a
threshold low enough to catch a real loop quickly. If it proves annoying,
raise `MOES_RESCUE_FAIL_THRESHOLD` — but not below 4, see next.

### 4.7 Interaction with the 3-power-cycle gesture (requirement 3)

Two independent defences, both cheap:

1. `MOES_RESCUE_FAIL_THRESHOLD` is **6**, and the gesture is 3 cycles. A user
   performing the gesture correctly reaches a count of 3.
2. `factoryRst_handler()` calls `moes_rescueClear()` when the gesture
   *completes*, immediately before `zb_factoryReset()`. So a successful
   gesture zeroes the counter as a side effect.

The gesture is **not available while in rescue mode**, because rescue skips
`factoryRst_init()`. That is deliberate: rescue mode exists to stay joined and
take an OTA, and a factory reset would drop the light off the network and make
recovery *harder*. A light in rescue mode is one OTA away from normal, and a
normal boot restores the gesture.

### 4.8 Rescue mode masks a real bug

A light that quietly latches rescue mode looks "fine" — it is on the network,
it responds to z2m — while doing none of its job. The signals that distinguish
it: it emits dim cool white and nothing else, it ignores on/off, level and
colour, and it polls OTA every 10 minutes instead of every 6 hours. Worth
adding an explicit indicator in a later version (see §7).

---

### 5. The watchdog (requirement 4)

**Closed in code as of build 06** (branch `moes-ts0505b`, HEAD `07e157a`):
`MODULE_WATCHDOG_ENABLE` is now `1` (`light/app_cfg.h:76`), the
`ota_newImageValid()` CRC loop is serviced (`drv_wd_clear()` per 256-byte
iteration), and `mspi_wait()` is bounded (`MSPI_WAIT_MAX_ITER 20000u`). The
sequencing rule was honoured — the enable and the CRC fix shipped in the same
change. The history below explains why those three belong together; it is left
intact as the pre-build-06 record.

### 5.1 The sequencing argument, corrected by the field

A watchdog turns a hang into a reset loop. The pre-2026-08-15 reading was "a
reset loop is worse than a hang"; the field result refuted that in one direction.
On this device the only recovery path is OTA, so **a hang is strictly worse than
a reset loop**: a hang produces no reboots, so probation never advances and
rescue mode can never latch; a reset loop at least presents a rescue-mode target.
Build 04 is the counterexample.

With rescue mode in place, the watchdog becomes net positive: a hang becomes a
reset, the resulting unstable boots park the probation counter at six, and the
seventh boot latches rescue and becomes recoverable. Without a watchdog, a hang
is permanent and nothing counts it.

So the ordering still holds, but for a sharper reason: **rescue mode first
(proven on a bench unit), watchdog second — because the watchdog only helps once
rescue mode is there to convert its resets into recovery.**

### 5.2 The concrete reason not to do it yet

`main()` enables it as `drv_wd_setInterval(600)` — 600 ms — and clears it twice
per loop iteration. Any single operation that blocks the main loop for longer
than 600 ms resets the device *mid-operation*. The candidates:

* **`ota_newImageValid()`** (`zigbee/ota/ota.c:138`) CRCs the entire staged
  image in one call, 256 bytes at a time, with **no `drv_wd_clear()` in the
  loop**. For a ~200 KB image that is ~800 iterations of `flash_read` +
  `xcrc32`. Estimated 100–150 ms, so it probably survives — but it is called
  from `ota_mcuReboot()`, at the single most dangerous moment in the device's
  life, and "probably" is not the standard here. The image will also grow.
  `bughunt/watchdog_design.md` §1.2 refines this estimate to ~160–240 ms for
  today's image and ~330–500 ms at the legal maximum image size — brushing the
  real ~595 ms interval.
* **NV sector rotation** erases up to 4 sectors for the keypair module.
* **`move_flash_data()`** in `moes_otaScheme.c` would certainly exceed it —
  currently not compiled (`AUDIT_FINDINGS.md` A-7).

**If the watchdog is ever enabled, add an explicit `drv_wd_clear()` inside
`ota_newImageValid()`'s CRC loop first**, and treat that as part of the same
change. That is a vendored-SDK patch and belongs in the table in
`MOES_EDITING_GUIDE.md` §2. The full 600 ms justification (worst legitimate
IRQ-off ≤ ~300 ms, actual interval ≈595.3 ms) is in `bughunt/watchdog_design.md`
§2.

---

### 6. The no-bootloader dual-bank scheme — recommendation: **do not do it**

`light/moes_otaScheme.c`, behind `MOES_NOBOOT_MIGRATION`. It is genuinely
attractive on paper — true A/B, the old image stays intact until the new one
boots. Four reasons not to, in order of weight.

### 6.1 The failure mode is not "a bad update", it is a dead board

`ensure_correct_ota_scheme()` and `moes_otaBankInstall()` both end with:

```c
ram_flash_write_page(0x0    + FLASH_TLNK_FLAG_OFFSET, 4, &unused_flag);  /* stock bootloader */
ram_flash_write_page(0x8000 + FLASH_TLNK_FLAG_OFFSET, 4, &unused_flag);  /* running slot */
SYSTEM_RESET();
```

That zeroes the `KNLT` flag inside the **stock Tuya bootloader** — the thing
`MOES_EDITING_GUIDE.md` §1.3 says must never be written, and the thing that
makes OTA installs work at all. If the image at `0x40000` does not come up,
there is nothing left that does: bootloader invalidated, `0x8000` invalidated,
SWire writes broken on this silicon, the bootloader's UART path unanswered, no
TB-03F in hand. **Every light that took that update, at once.**

Rescue mode's worst case is "one light needs a ladder". This one's worst case
is the whole deployed fleet.

### 6.2 The prerequisite is bigger than "put `main()` in `.ram_code`"

The image is linked for `0x8000` (`BOOT_LOADER_MODE 1` → `APP_IMAGE_ADDR
0x8000`). The TLSR8258 hardware bank boot remaps so that an image *linked for
0x0* executes from `0x40000`. Copying a `0x8000`-linked image there means
every absolute address is off by `0x8000`.

Putting `main()` in `.ram_code` does not fix that — it only lets the migration
routine keep running while the flash window backing executing code is
rewritten. Doing this properly needs a **second, differently linked build**
(`BOOT_LOADER_MODE 0`, `APP_IMAGE_ADDR 0x0`) and a hand-off: OTA the
`0x8000`-linked image, have it stage the `0x0`-linked image, migrate, reboot.
That is two images, two link maps, and a one-way transition between them, on
hardware we cannot recover.

Note also that `MOES_NV_BASE_ADDRESS` and the OTA bank are coupled
(§1.2 of the guide). Changing `BOOT_LOADER_MODE` moves `FLASH_ADDR_OF_APP_FW`
from `0x8000` to `0x0`, which changes `FLASH_OTA_IMAGE_MAX_SIZE` from
`0x68000` to `0x6C000` and `FLASH_ADDR_OF_OTA_IMAGE` from `0x70000` to
`0x6C000`. Every address in `FIRMWARE_STATUS.md` §3 moves.

### 6.3 Most of the benefit is already banked

With `NV_BASE 0xD8000` the staging bank is `0x70000`, which is exactly where
the stock bootloader looks. The install is already close to atomic:
`ota_mcuReboot()` runs `ota_newImageValid()` — a full CRC-32 over the staged
image — and only then writes the one flag byte that arms it. The bootloader
validates again before copying. A stalled or corrupt download is a no-op;
that has been observed three times.

The residual window is *not* "a half-written image". It is **"a valid image
that boots and then reboots"** — the exact failure of 2026-08-14.

### 6.4 Dual-bank does not fix that failure either

A dual-bank device that boots the new image and then reset-loops is just as
stuck as a single-bank one, unless you add a boot-success flag and a fallback
to the other bank — which is *probation again*, with more moving parts and a
worse failure mode.

**So: probation/rescue is strictly the higher-value change, and it is
reversible. Dual-bank is the lower-value change and it is not.**

Keep `MOES_NOBOOT_MIGRATION` undefined. The file is now guarded by that symbol
in its entirety (`AUDIT_FINDINGS.md` A-7) so it cannot be reached by accident
and does not consume RAM-code budget.

**If it is ever revisited**, the prerequisites are: a TB-03F programmer in
hand (so a failed migration is recoverable), a two-image build with the second
image linked for `0x0`, and a bench unit that has survived the full cycle
twice. Not before.

---

### 7. Testing

### 7.1 Without hardware — done, and repeatable

`tools/rescue_hosttest/` compiles **the real `light/moes_rescue.c`** (not a
copy, not a model) against a shim providing the four SDK symbols it uses. NV
becomes a byte in the test file; the `TL_ZB_TIMER` becomes a manual tick; each
"boot" runs in a `fork()`ed child so the module's file statics are genuinely
re-initialised the way a reboot would do.

```
cd tools/rescue_hosttest && make check
```

15 scenarios, currently all passing:

| scenario | asserts |
|---|---|
| fresh device | first boot normal, count 1, exactly one write |
| below / at / above threshold | latch boundary is `>=`, and a latching boot writes nothing |
| latch after N boots | first rescue boot is #7 for a threshold of 6 |
| stable clears | no clear at 19 minutes, clear at 20, exactly one write, timer self-cancels |
| dropping off restarts the clock | a disconnect reloads the countdown rather than pausing it |
| gesture clears | `moes_rescueClear()` zeroes and persists; clearing an already-zero counter writes nothing |
| gesture cannot latch | threshold > 3 |
| garbage NV | `0xFF` clamps *into* rescue, no write |
| unreadable NV | treated as zero, boots normally |
| unwritable NV | still boots, tries once, no crash |
| timer pool exhausted | `TL_ZB_TIMER_SCHEDULE` returning NULL just means this boot does not clear |
| timer only when needed | idempotent, and not scheduled when the count is already 0 |
| flash wear bound | **200 consecutive unstable boots → exactly 6 NV writes** |

The harness was verified to actually catch regressions: changing the latch test
in `moes_rescue.c` from `>=` to `>` makes 5 scenarios fail, including the
flash-wear bound (200 writes instead of 6) and "never latches".

### 7.2 Without hardware — build checks

```bash
# rescue mode compiles and links
cmake . -B build -DDEVICE_VARIANT=TS0505B
cmake --build build --target light_TS0505B.zigbee -j8

# and the off switch really is an off switch
# (set MOES_RESCUE_ENABLE 0 in light/moes_rescue.h, rebuild, restore)
```

### 7.3 With hardware — bench unit, before any ceiling light

1. **Normal boot.** Flash the ordinary image. Confirm the light joins, is
   controllable, is *not* dim-white-and-unresponsive, and that
   `device_announce` appears once and only once in 15 minutes.
2. **Forced rescue.** Rebuild with `-DMOES_RESCUE_FORCE`, flash, confirm:
   dim cool white; on/off, level and colour commands accepted by ZCL (the
   attributes change and read back) but the *output does not move*; identify
   does not blink; `0xEF00` commands rejected; the device still joins, still
   interviews, still reports `supports_ota: true`.
3. **OTA from rescue mode — the whole point.** With the unit in forced rescue,
   push an ordinary image at it over MQTT. It must complete and reboot into
   normal operation. *If this step fails, the mechanism is worthless and
   nothing else in this document matters.*
4. **Natural latch.** Rebuild with a deliberate `SYSTEM_RESET()` about 15 s
   into `app_task()`, flash it, and watch: six announce-and-reset cycles, then
   the light stops resetting and comes up dim white. Confirm the announce
   cadence goes quiet. Then OTA it back to good.
5. **Self-clearing.** From a count of 1–5 (interrupt step 4 early), leave the
   light joined for 25 minutes, power-cycle it, and confirm it boots normally
   rather than into rescue.
6. **Gesture co-existence.** On a healthy unit, perform the 3-power-cycle
   gesture. Confirm it factory-resets and re-pairs as usual, and that it does
   *not* come up in rescue mode afterwards.

---

### 8. Honest scorecard

**What this buys:** the class of bug that has actually hurt this project —
application code that stops the light staying up — is now recoverable over the
air. That is the entire risk model in `AI_AUDIT_BRIEF.md` §1, minus the parts
below.

**What it does not buy:** anything before `stack_init()` returns, anything
inside `libzb_router.a`, and anything that stops the radio. Those remain
ladder territory, and the only mitigations are the bench-first policy and
keeping the pre-stack window small.

**What would close the remaining gap:** a **TB-03F programmer** (pvvx /
TLSRPGM). It is the only thing that makes an arbitrary bad image recoverable,
it is cheap, and it is still not in hand. Everything in this document is a
substitute for it. Buy one.

---

### 9. Possible later work

Not implemented, in rough value order:

1. **Report the state.** Expose the probation count and rescue flag as a
   read-only attribute (there is one free `0xEF00` datapoint) or fold it into
   `swBuildId`, so `zigbee2mqtt/bridge/devices` shows which lights have been
   unstable. Today rescue mode is only visible by behaviour.
2. **Second-stage escalation.** If a light is *still* looping after N boots in
   rescue mode, the fault is below the application and there is nothing left
   to switch off — but recording that fact would tell a human to stop waiting.
3. **Widen the stable-clock signal.** "Joined for 20 minutes" is a proxy for
   "could have taken an OTA". A stronger signal would be "completed an OTA
   query round-trip", which proves the OTA cluster is actually being serviced.
4. **Rescue-mode brightness as a code.** Blink count or brightness level could
   encode the probation count, giving a diagnosis from the floor.
