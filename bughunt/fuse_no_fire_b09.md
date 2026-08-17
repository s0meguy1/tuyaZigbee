# fuse_no_fire_b09 — build-09 liveness fuse did not fire on its first real-hardware test

> ## Superseded certainty notice (2026-08-17)
> The liveness fuse's non-fire cause is unresolved. The build-10 one-shot
> re-arm change and its hardware-timer behavior are not silicon-verified;
> passing host tests cannot model the timer, IRQ, or stack conditions. Do not
> treat the causal verdict below as established.

**Scope:** host-side analysis only. No device action, no flashing, no radio/MQTT/docker,
no commit, no git mutation. One new file (this one).
**Branch:** `moes-ts0505b`; build 09 = HEAD `2f3636a` + public-moes, image
`build/light/light_TS0505B.lst` (built 2026-08-16 18:56).
**Read with:** `light/moes_liveness.{c,h}`, `light/moes_rescue.c`, `light/zb_appCb.c`,
`bughunt/liveness_redesign.md`, `bughunt/liveness_no_fire.md`, `bughunt/mac_scan_wedge.md`,
`bughunt/boothang_stack.md`, the vendored SDK (`proj/drivers/drv_timer.{c,h}`,
`platform/services/b85m/irq_handler.c`, `platform/chip_8258/{timer,bsp,register}.h`,
`proj/os/ev_timer.c`, `proj/os/ev.c`, `apps/common/main.c`, `zigbee/bdb/bdb.c`) and
`tools/rescue_hosttest/*`.

---

## Verdict up front

| Suspect | Verdict |
|---|---|
| 1. TIMER_IDX_0 availability + `drv_hwTmr` period semantics | **Arithmetic CLEARED (period is exactly 1 s); timer index CLEARED (free). The periodic re-arm of a B85 Timer0 in `TIMER_MODE_SCLK` is the one unproven silicon link and is the prime suspect.** |
| 2. Arming path on the OTA-install boot | **CLEARED** — both the joined and factory-new branches provably arm the monitor; there is no `bdb` resume branch that bypasses the init callback. |
| 3. Plain implementation bugs | **NONE FOUND** — `volatile` split, fuse comparison, reset threshold, ticker re-arm and hw re-arm are all correct in the linked ELF. |
| 4. Host-test fidelity | **CONFIRMED GAP** — `tools/rescue_hosttest` models the hw timer as "fire N times on command" and ignores the period, so the 27/27 suite proves the *logic* but cannot see a real-hardware cadence/IRQ failure. |

**Most probable root cause:** the fuse's decision logic is correct, the monitor is armed, and the
ticker starves in both known wedge classes — but the hardware-timer sampler is **not producing a
real 1 s periodic interrupt on the TLSR8258 in this application**. Build 09 is the first production
user of `drv_hwTmr` `TIMER_IDX_0` in periodic `TIMER_MODE_SCLK`; the SDK documents that mode as
"free run from 0 to 0xffffffff", and the periodic re-arm it relies on
(`timer_set_init_tick(0)` + `timer_set_cap_tick(tick)` on a free-running 32-bit counter) has never
been bench-verified for Timer0/1/2 on this part. If that re-arm does not actually reset the
free-running counter, the sample cadence is not 1 s — it becomes the 32-bit wrap time (~89 s at
48 MHz), so the 60-sample fuse would take ~89 minutes, which is invisible in a 10-minute
observation. This is exactly the class of failure the 27/27 host suite cannot represent.

---

## 1. Suspect 1 — TIMER_IDX_0 and the `drv_hwTmr` period

### 1a. The period is exactly 1 s, not ~48 µs and not 1000 s

`moes_livenessHwSampleCb` returns `MOES_LIVENESS_SAMPLE_MS * 1000` = `1 000 000`
(`light/moes_liveness.c:71-97`, both the armed and disarmed paths). The SDK interprets that
return as **microseconds**, not ticks:

- `drv_hwTmr_set(tmrIdx, t_us, ...)` computes `t.low = t_us * TIMER_TICK_1US_GET(tmrIdx)`
  (`proj/drivers/drv_timer.c:219-230`). For `TIMER_IDX_0` (< `TIMER_IDX_3`),
  `TIMER_TICK_1US_GET` = `H_TIMER_CLOCK_1US` = `CLOCK_SYS_CLOCK_HZ / 1000000` = **48**
  (`drv_timer.h:29-41`). So the initial capture is `1 000 000 * 48 = 48 000 000` ticks = **1 s**
  at the 48 MHz timer clock.
- On expiry, `drv_hwTmr_irq_process` takes the callback return `t` and does
  `pTimer->expireInfo.low = t * TIMER_TICK_1US_GET(tmrIdx)` (`drv_timer.c:155-186`, the multiply
  at `:171`) — i.e. the return is µs, and the re-arm capture is again `48 000 000` ticks = 1 s.
- The linked ELF confirms the constants: `light_TS0505B.lst` literal `0x000f4240` (= 1 000 000)
  at the return sites `a03c`/`a08c`, and `drv_hwTmr_set` hardcodes `0x30` (= 48) for the
  `idx < 3` µs scale (`1ecc6: tmovs r1, #48`), multiplying it into the capture at `1ed10`.
- The fuse threshold is also correct: `MOES_LIVENESS_PROGRESS_RESET_TICKS` =
  `60 * 1000U / 1000` = 60 (`light/moes_liveness.c:19-20`), and the compiled comparison is
  `tcmp r2, #59; tjls skip` — reset when `s_noProgress` reaches 60 (`a00e`-`a010`).

The redesign paper's pseudo returned `SAMPLE_MS * TIMER_TICK_1US_GET(...) / 1000`, which would
have been ~48 µs; the shipped code correctly fixed that. **There is no 1000× arithmetic error.**

### 1b. TIMER_IDX_0 is free in the linked application

- Timer index table: `TIMER_IDX_2` = watchdog, `TIMER_IDX_3` = MAC CSMA system timer
  (`proj/drivers/drv_timer.h:46-50`).
- The only `drv_hwTmr_init` / `drv_hwTmr_set` call sites in the linked ELF are:
  - our liveness (`drv_hwTmr_init` at `a05a`, `drv_hwTmr_set` at `a066`), and
  - the radio/MAC system timer (`drv_hwTmr_init` at `1d0d4` via `ZB_TIMER_INIT`,
    `drv_hwTmr_set` at `22362` — both `TIMER_IDX_3`).
- `drv_hwTmr_init(TIMER_IDX_0, ...)` memset's the per-index info and then does
  `timer_set_mode(0, mode)` + `reg_irq_mask |= (1 << 0)` (`drv_timer.c:193-201`;
  `platform/chip_8258/timer.h:90-113`; disasm `1ec82`-`1ec96`). Nothing else in the binary writes
  `reg_irq_mask` bit 0 — the only other `reg_irq_mask` writers clear bits 13 and 20
  (`0xd70`/`0xd74` and `0x1eb3c`/`0x1eb40` literals), never bit 0.

So TIMER_IDX_0 is not stolen by another linked object.

### 1c. The callback really runs in IRQ context, and `SYSTEM_RESET()` from there is safe

- `irq_handler` (`platform/services/b85m/irq_handler.c:80-85`) dispatches
  `drv_timer_irq0_handler()` on `FLD_IRQ_TMR0_EN` after clearing the source and
  `reg_tmr_sta = FLD_TMR_STA_TMR0`.
- `drv_timer_irq0_handler` → `drv_hwTmr_irq_process(TIMER_IDX_0)` → `pTimer->cb(arg)`
  (`drv_timer.c:234-237`, `:155-176`), so `moes_livenessHwSampleCb` executes in the timer IRQ,
  independent of `ev_main`.
- `SYSTEM_RESET()` = `mcu_reset()` = `write_reg8(0x06f, 0x20)` — a single byte write to
  `reg_pwdn_ctrl` setting `FLD_PWDN_CTRL_REBOOT` (`drv_hw.h:31`,
  `platform/chip_8258/bsp.h:110-113`, `register.h:247-252`). The ELF reset path is exactly this:
  `a012-a016` stores `0x20` to `0x80006f`. A register write is IRQ-safe; no NV write is involved
  (the unmarked-reset design is correct).

### 1d. The one thing not proven: Timer0 periodic re-arm in `TIMER_MODE_SCLK`

The SDK comment is explicit: `TIMER_MODE_SCLK` = "free run from 0 to 0xffffffff"
(`drv_timer.h:56`). The periodic re-arm is `hwTimerSet` → `timer_set_init_tick(tmrIdx, 0)` +
`timer_set_cap_tick(tmrIdx, tick)` (`drv_timer.c:80-89`), executed from the IRQ each expiry. That
only produces a clean 1 s cadence if writing `reg_tmr0_tick` actually resets a free-running
counter. This is the exact path the 27/27 host suite does not exercise (see §4), and it has no
production precedent in this firmware: the MAC CSMA backoff uses `TIMER_IDX_3`, whose re-arm is a
*different*, absolute mechanism (`stimer_set_irq_capture(tick + clock_time())`,
`drv_timer.c:87`), so it does not validate the Timer0/1/2 re-arm at all.

---

## 2. Suspect 2 — arming path on the OTA-install boot: CLEARED

The boot path after the stock bootloader installs the image is the ordinary `bdb_init` path, not a
separate "resume" that skips the app callback:

- `tuyaLight.c:463` calls `bdb_init(..., repower=1)`. In `bdb_init`
  (`zigbee/bdb/bdb.c:1591-1660`) the device reads NV via `zb_isDeviceFactoryNew()`; with valid NV
  it sets `nodeIsOnANetwork = factoryNew ? 0 : 1` (`:1636`) and takes the
  `bdb_routerStart()` branch (`:1638-1648`).
- `bdb_routerStart` (`0x35d0c` in the ELF) is a two-instruction tail call to `zb_routerStart`
  (`0x35ee4`), which drives the start-device confirm. `bdb_zdoStartDevCnf` in `BDB_STATE_INIT`
  sets `initResult` and schedules `BDB_EVT_INIT_DONE` (`bdb.c:1272-1278`), and `bdb_task` then
  calls the app init callback (`bdb.c:1082-1084`):
  `zbdemo_bdbInitCb(initResult, nodeIsOnANetwork)`.
- `light/zb_appCb.c:151-168`: when `status == BDB_INIT_STATUS_SUCCESS && joinedNetwork`,
  `moes_livenessBootedOnNetwork()` is called **before** any rejoin completes — this is the arm
  for the class-1 rejoin-scan wedge.
- The factory-new branch (`joinedNetwork == 0`) schedules network steer and arms later at
  `BDB_COMMISSION_STA_SUCCESS` via `moes_livenessJoined()` (`light/zb_appCb.c:212-227`).

The observed announce at 23:27:55 proves a successful (re)join, which fires
`zbdemo_bdbCommissioningCb(BDB_COMMISSION_STA_SUCCESS)` → `moes_livenessJoined()`
(`zb_appCb.c:227`). So at least one arm point ran on this boot regardless of whether the NV read
as joined or factory-new. The monitor was armed and the hw timer was started at arm time
(`moes_livenessEnsureTimer`, `light/moes_liveness.c:103-123`, sets `s_hwArmed` only after
`drv_hwTmr_init`+`drv_hwTmr_set`).

---

## 3. Suspect 3 — plain bugs: NONE FOUND

- **volatile split is correct.** `s_armed`/`s_progress` are `volatile` (task-written, IRQ-read);
  `s_lastProgress`/`s_noProgress` are IRQ-only (single writer = the IRQ);
  `s_tickerRunning`/`s_hwArmed` are task-only (`light/moes_liveness.c:25-36`). The ELF accesses
  each as a single byte `tloadrb`/`tstorerb`, so there is no torn access or caching across the IRQ.
- **Fuse comparison is correct.** `++s_noProgress >= 60` compiles to unsigned compare against 59
  with `tjls` skip (`a00e-a010`); reset fires at 60 no-progress samples.
- **Ticker re-arm is correct.** `moes_livenessProgressTickerCb` returns 0
  (`light/moes_liveness.c:47-56`), and `ev_timer_executeCB` treats `t == 0` as "re-arm with the
  same period" (`proj/os/ev_timer.c:256-286`, the `:267-268` branch). The ticker is scheduled via
  `TL_ZB_TIMER_SCHEDULE` = `ev_timer_taskPost` (`ev_timer.h:134`), i.e. the cooperative
  `ev_timer.timer_head` list, which is exactly the list both wedge classes starve
  (`liveness_no_fire.md` §4, `ev.c:69-75`, `main.c:85`). Period is 1000 ms
  (`ev_timer_update` uses `S_TIMER_CLOCK_1US`, `ev_timer.c:239`).
- **Hw re-arm arithmetic is correct** (§1a). The callback return is µs, matching the SDK's
  `t * TIMER_TICK_1US_GET` at `drv_timer.c:171`.
- The reset write is unreachable-guarded and does not do an IRQ-unsafe NV write
  (`light/moes_liveness.c:88-89`), matching the design's deliberate unmarked-reset choice.

---

## 4. Suspect 4 — why the 27/27 host suite cannot see this: CONFIRMED GAP

`tools/rescue_hosttest` compiles the real `light/moes_liveness.c` (Makefile), but the shim
models the hardware timer as a **period-agnostic fire-N-times** substrate:

- `drv_hwTmr_set` ignores `t_us` entirely (`test_rescue.c:143-153`) and just stores the callback.
- `tick_hw_all(n)` calls every live hw callback exactly `n` times (`test_rescue.c:195-207`); it
  uses the callback return only for its **sign** (`< 0` cancels), never its magnitude.
- `SYSTEM_RESET()` is `host_systemReset()` — a counter (`test_rescue.c:227-231`,
  `shim/tl_common.h:57`).
- There is no model of: the µs→tick multiply, `drv_hwTmr_irq_process`'s re-arm, the single
  `irq_handler` dispatch order (RF RX/TX first, Timer0 after), IRQ masking, or TIMER_IDX_0
  conflicts.

Consequently the 27 passing scenarios prove "armed + no progress → reset" as a state machine, but
they **cannot** prove that the physical Timer0 fires every 1 s on the TLSR8258, or that its IRQ is
serviced during a real wedge. The suite is green (verified: `make check`, 27/27 "all scenarios
passed") precisely because the failure is in the substrate the shim omits, not in the logic.

---

## 5. Most probable root cause

The only link left standing after clearing suspects 1a/1b/2/3 is the **real cadence of the
hardware-timer sampler**. The observed no-fire is itself strong evidence: in both known wedge
classes the ticker starves (class-1 parks `ev_timer_executeCB`; class-2 parks `ev_poll_process`,
`boothang_stack.md` §2a), `s_progress` freezes, and the monitor is armed — so a genuine 1 s Timer0
would have tripped the fuse ~60 s after the wedge, well inside the 10+ minute observation. It did
not, so the sampler was not sampling at 1 s (or was not being serviced).

Ranked concrete mechanisms, all invisible to the host suite:

1. **(Primary) Timer0 `TIMER_MODE_SCLK` periodic re-arm does not produce a 1 s cadence on
   silicon.** The mode is documented "free run from 0 to 0xffffffff", and the SDK re-arms by
   writing `reg_tmr0_tick = 0` + `reg_tmr0_capt = 48M`. If the free-running counter is not
   actually reset by the tick write, the compare is already in the past after the first expiry and
   the next interrupt arrives only after the 32-bit wrap (~89 s at 48 MHz). 60 such samples ≈
   89 minutes, so the fuse never fires inside the observation window.
2. **Timer0 IRQ starved by the shared `irq_handler` dispatch.** `irq_handler` handles RF RX/TX
   first and reaches `drv_timer_irq0_handler` only after `rf_rx_irq_handler` returns
   (`irq_handler.c:57-85`). A wedge that parks the CPU inside `rf_rx_irq_handler`
   (`mac_scan_wedge.md` §2) or holds IRQs off in the MAC CSMA CCA section
   (`mac_scan_wedge.md` §1) blocks Timer0 even when its flag is pending.
3. **Residual — the device is in a third wedge class where `ev_main` keeps returning.** Then the
   ticker keeps firing and the fuse correctly does not trip, but this contradicts the
   announce→silent-during-interview signature, which matches the class-2 `boothang_stack` wedge
   (a hard `ev_poll` hang).

Both 1 and 2 were explicitly listed as unverified in `liveness_redesign.md` §6 F1/F2 and
`liveness_no_fire.md` §9, and both live entirely in the hardware/IRQ substrate that
`tools/rescue_hosttest` does not model.

---

## 6. Minimal build-10 patch sketch

All changes in `light/moes_liveness.c`; no SDK edits, no other files.

### 6a. Do immediately (low risk, fixes a latent bug)

In `moes_livenessEnsureTimer` (`light/moes_liveness.c:117-122`), stop ignoring the
`drv_hwTmr_set` return value and only latch `s_hwArmed` on success:

```c
if(!s_hwArmed){
    drv_hwTmr_init(TIMER_IDX_0, TIMER_MODE_SCLK);
    hw_timer_sts_t st = drv_hwTmr_set(TIMER_IDX_0, MOES_LIVENESS_SAMPLE_MS * 1000,
                                      moes_livenessHwSampleCb, NULL);
    s_hwArmed = (st == HW_TIMER_SUCC);   /* do not pretend an arm we never got */
}
```

This turns any `HW_TIMER_IS_RUNNING`/`HW_TIMER_INVALID` arm failure into a retry on the next arm
call instead of a silent "armed but no callback".

### 6b. Fix the actual cadence (needs one bench measurement to choose)

The root-cause fix is to stop depending on the unproven free-run re-arm. Two options, both inside
this file:

- **Option A (preferred if bench confirms tick mode is periodic):** change the init mode from
  `TIMER_MODE_SCLK` to `TIMER_MODE_TICK` (mode 3 — the periodic-tick mode, `chip_8258/timer.h:48`)
  at `light/moes_liveness.c:118`. Re-scale the period only if the bench shows tick mode runs off
  the 16 MHz system tick rather than the 48 MHz `H_TIMER_CLOCK_1US`.
- **Option B (if the free-run counter is the problem and tick mode is unsuitable):** make the
  sampler a **one-shot** and re-arm it explicitly with a fresh `drv_hwTmr_set` from the callback's
  own tail (keeping `s_hwArmed` in IRQ-only ownership), instead of relying on the SDK's
  return-value re-arm. This still uses Timer0, so it only helps if the defect is the re-arm
  arithmetic/order, not the timer hardware itself.

**Before shipping either option, bench-verify the actual Timer0 cadence** (toggling a GPIO in
`moes_livenessHwSampleCb` and measuring the period is enough). The 27/27 host suite cannot make
this decision; it is the precise gap §4 documents.

---

## 7. Refuted theories

| Theory | Why it fails |
|---|---|
| Return-value math is 1000× off (48 µs or 1000 s) | The shipped code returns `1 000 000` µs and the SDK multiplies by 48 ticks/µs → 1 s; confirmed in source and in the ELF literals (`0x0f4240`, `0x30`). |
| TIMER_IDX_0 is stolen by CSMA/RF/system | ELF has no other `drv_hwTmr_init/set(TIMER_IDX_0)` user; CSMA is TIMER_IDX_3, watchdog is Timer2. |
| `SYSTEM_RESET()` from IRQ is unsafe / NV write in IRQ | Reset is a single `write_reg8(0x6f,0x20)`; the code deliberately does **not** call `moes_resetSkipNextBoot()`. |
| OTA-install boot takes a `bdb` resume path that skips `zbdemo_bdbInitCb` | `bdb_init` → `bdb_routerStart` → `zb_routerStart` → `bdb_zdoStartDevCnf(BDB_STATE_INIT)` → `bdb_task` → `zbdemo_bdbInitCb`; no bypass exists. The announce also proves the SUCCESS arm fired. |
| Fuse comparison off-by-one / signed bug | `++s_noProgress >= 60` compiles to unsigned `> 59` with correct skip; resets at 60. |
| Ticker returns wrong and stops | It returns 0, which `ev_timer_executeCB` treats as "re-arm same period" (`ev_timer.c:267-268`). |
| `s_progress` u8 wrap defeats the fuse | Sampler compares equality; at 1 Hz wrap is 256 s, above the 60 s fuse (`liveness_redesign.md` F6). |
| Host suite proves the whole thing | It proves the state machine only; its hw-timer shim is period-agnostic and fires N times on command (`test_rescue.c:143-207`). |

---

## 8. Inferred / unverified

- That the Timer0 free-run re-arm does **not** reset the counter is **INFERRED** from the SDK's
  "free run from 0 to 0xffffffff" wording and the absence of any production precedent for
  Timer0/1/2 periodic use; it has **not** been measured on a bench part. This is the single most
  important follow-up.
- That the observed wedge is class-2 (`boothang_stack`, task-context `ev_poll` hang with IRQs
  live) is **INFERRED** from the announce→silent-during-interview signature and the ~94 s z2m
  interview timeout; the exact PC was not captured for this unit.
- The claim that both known wedge classes starve the progress ticker is a code reading of
  `ev_main`/`ev_timer`/`ev_poll` order plus the two prior SRAM captures; it was not re-captured on
  build 09.
