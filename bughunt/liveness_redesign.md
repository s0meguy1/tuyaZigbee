# liveness_redesign — wedge-proof liveness for build 09+ (paper)

**Scope:** design only. No code changed, nothing flashed, no git mutation, no
radio/MQTT/docker action. Exactly one new file (this one).
**Branch:** `moes-ts0505b`; builds under discussion: build 07 (HEAD `87a4e2d`)
and the uncommitted build-08 fix in the tree.
**Read with:** `bughunt/liveness_no_fire.md`, `bughunt/boothang_stack.md`,
`bughunt/mac_scan_wedge.md`, `light/moes_liveness.{c,h}`, `light/moes_rescue.{c,h}`,
`light/zb_appCb.c`, `tools/rescue_hosttest/test_rescue.c`, and the vendored SDK
sources under `build/tl_zigbee_sdk/` (`proj/os/ev_timer.c`, `proj/os/ev.c`,
`proj/drivers/drv_timer.{c,h}`, `proj/drivers/drv_hw.h`, `apps/common/main.c`,
`apps/common/factory_reset.c`, `platform/services/b85m/irq_handler.c`).
Claims that cannot be fully proven from source/prior captures are marked INFERRED.

---

## Verdict up front

The build-07 fuse fails in three independent ways, and only one of them is
"move the sampler off the starved list":

1. **Starvation** (`liveness_no_fire.md` candidate #1): the sampler is a
   cooperative `ev_timer` callback on the same list the wedged scan occupies, so
   it never runs during a wedge. **Fix:** keep a *task-context progress ticker*
   on `ev_timer` (it is *supposed* to starve), but move the *decision* to a
   `drv_hwTmr` IRQ that is serviced independently of `ev_main`.
2. **Joined-blindness** (class 2): the fuse condition is "unjoined AND zero BDB
   events", so a joined-but-wedged device never trips it. **Fix:** drop `joined`
   from the fuse condition entirely; the fuse becomes "no scheduler progress for
   N s".
3. **Probation erasure**: `moes_rescueStableTimerCb` clears the counter on
   `zb_isDeviceJoinedNwk()` alone, which stays 1 through a class-2 wedge.
   **Fix:** the stable clock may only clear on joined **and** continuous
   progress, with a one-minute confirmation window after the countdown reaches
   zero.

The chosen heartbeat is **signal C — "the cooperative `ev_timer` list is actually
servicing callbacks"**, observed as a task-context counter that the IRQ sampler
watches. It is the only candidate that keeps firing when the coordinator is down
and stops in *both* wedge classes.

---

## 1. Requirements matrix — signal × state

Three candidate "progress" signals are evaluated:

- **A — completed MAC TX** (CCA + TX-done IRQ, regardless of ACK). Observable in
  the app via the SDK-source counter `T_DBG_irqTest[0]` incremented in
  `platform/services/b85m/irq_handler.c:58-62` before `rf_tx_irq_handler()`.
- **B — any MAC RX**. Observable via `T_DBG_irqTest[1]`
  (`irq_handler.c:64-68`), or an app data-indication callback.
- **C — `ev_timer` list actually advancing** (the cooperative scheduler serviced
  a task-context callback). Observable only by a counter that an `ev_timer`
  callback increments.

| State | A: completed MAC TX | B: any MAC RX | C: ev_timer advancing |
|---|---|---|---|
| **S1 healthy idle joined** (NWK link-status TX every 15 s, `nwk_nib.h:57`) | FIRES — each link-status completes TX; TX-done fires with or without ACK (INFERRED) | FIRES — ACKs, neighbor link-status, coordinator traffic | FIRES — main loop idle, `ev_main` returns, ticker fires ~1 s |
| **S2 OTA transfer** | FIRES — image-block responses and app TXs complete | FIRES — image blocks inbound | FIRES — `ev_main` returns between chunks |
| **S3 pairing / factory-new** (no credentials) | N-A — monitor not armed | N-A | N-A — not armed |
| **S4 rescue mode** (joined, OTA query every `MOES_RESCUE_OTA_QUERY_SECONDS`=600 s) | FIRES — OTA query + link-status | FIRES | FIRES |
| **S5 coordinator offline 10 min** (MUST NOT reset-loop) | FIRES — rejoin beacon-requests / link-status attempts still complete TX without ACK (INFERRED, strong; `mac_scan_wedge.md` §1 shows TX-done is armed regardless of ACK outcome) | **NOT GUARANTEED** — quiet RF yields no frames → false reset | FIRES — scheduler alive; rejoin/OTA/link timers advance |
| **S6 class-1 wedge** (unjoined scan freeze, `mac_scan_wedge.md` §2) | STOPS — no TX in flight (radio idle-RX, MAC pending `0x84748c+8`=0) | STOPS at stack level; raw RX IRQ may still fire on ambient frames (UNCERTAIN) | **STOPS** — `ev_timer_executeCB` stuck in the scan timer cb, ticker behind it never fires |
| **S7 class-2 wedge** (joined APS/NWK link-status + ZCL-report hang, `boothang_stack.md` §1/§2) | STOPS — no TX in flight (radio RX, TX slot post-timeout, armed flag 0); a MAC-level retry of a queued frame is bounded by `macMaxCSMABackoffs`=4, so it would stop too (INFERRED, medium) | **UNCERTAIN** — radio is RX and the coordinator keeps sending; raw `rf_rx_irq_handler` may still fire and buffer frames while `ev_poll` is stuck | **STOPS** — `ev_poll_process` stuck, `ev_main` never returns, `ev_timer_process` never re-entered |

**Pick: C.** C is the only signal that FIRES in S5 and STOPS in both S6 and S7.

- B is disqualified twice over: it false-positives in S5 (coordinator-offline →
  RX silence → reset-loop), and it may false-negative in S7 (raw RF RX IRQ can
  keep firing while the NWK/APS task is wedged).
- A is a defensible second choice but its S7 guarantee depends on the MAC queue
  being empty, which is harder to prove than C's guarantee; and it requires the
  sampler to read an SDK-source debug counter rather than state the app owns. C
  also directly measures the layer both wedges starve.

The reason C works for class 2 is subtle and must be stated precisely: in
`ev_main()` (`ev.c:69-75`) `ev_timer_process()` runs *before*
`ev_poll_process()`. A class-2 wedge parks the CPU inside `ev_poll_process`, so
`ev_main` never returns to `main()` (`main.c:85`) and `ev_timer_process()` is
never entered again. A timer callback on `ev_timer` therefore stops firing in
class 2 exactly as it does in class 1 (where `ev_timer_executeCB` is stuck in an
earlier callback, `ev_timer.c:256-286`).

---

## 2. Chosen design — two-layer heartbeat ("watchdog of the watchdog")

This is the pattern liveness_no_fire.md §7 sketched as the "alternative worth one
line": keep a task-context sampler as the *source of truth*, and add a separate
`drv_hwTmr` IRQ whose only job is to detect that the task-context counter has
stopped advancing.

### 2.1 Precise definition of "progress"

Progress = `s_progress` (a `volatile u8`) changes between two hardware-timer
samples. `s_progress` is incremented by, and only by:

1. **Primary — the task-context progress ticker** `moes_livenessProgressTickerCb`,
   scheduled with `TL_ZB_TIMER_SCHEDULE` on the cooperative `ev_timer` list at
   `MOES_LIVENESS_PROGRESS_TICK_MS` = 1000 ms. The ticker is *deliberately* on the
   starved list: it advances only when `ev_timer` callbacks are actually serviced
   and `ev_main` is returning, which is precisely the "stack is alive" property we
   want.
2. **Corroboration — `moes_livenessStackActivity()`** (existing hook, called at
   `light/zb_appCb.c:202-206` on every BDB commissioning status) also increments
   `s_progress`. This is safe because neither wedge class fires commissioning
   callbacks: class 1 is a total freeze and class 2 sits in the APS/NWK/ZCL path
   with no BDB callback in the captured chain (`boothang_stack.md` §2). It simply
   shortens the "alive" proof during a rejoin storm.

Nothing about the radio (TX/RX) is required for progress. Coordinator-offline is
therefore trivially safe: the scheduler keeps running, so the ticker keeps
firing.

### 2.2 Hardware-timer IRQ sampler

`moes_livenessHwSampleCb` runs on `drv_hwTmr` `TIMER_IDX_0`,
`TIMER_MODE_SCLK`, period `MOES_LIVENESS_SAMPLE_MS` = 1000 ms
(`proj/drivers/drv_timer.c:193-201` init, `:219-230` set, `:234-237` irq0
handler; index table `drv_timer.h:46-50`).

IRQ-context pseudo-code (conceptual; no NV, no `zb_isDeviceJoinedNwk`, no
`moes_resetSkipNextBoot`):

```c
static volatile bool s_armed = FALSE;
static volatile u8   s_progress = 0;
/* IRQ-only (single writer = this IRQ): */
static u8 s_lastProgress = 0;
static u8 s_noProgress   = 0;

static int moes_livenessHwSampleCb(void *arg)
{
    (void)arg;

    if(!s_armed){
        /* Factory-new/pairing: nothing to return to. Keep the baseline fresh so
         * arming later never inherits a stale "no progress" stretch. */
        s_lastProgress = s_progress;
        s_noProgress   = 0;
        return MOES_LIVENESS_SAMPLE_MS * TIMER_TICK_1US_GET(TIMER_IDX_0) / 1000U;
    }

    if(s_progress != s_lastProgress){
        s_lastProgress = s_progress;
        s_noProgress   = 0;
    }else if(++s_noProgress >= MOES_LIVENESS_PROGRESS_RESET_TICKS){
        /* Wedged regardless of joined state. Deliberately UNMARKED: see 2.4. */
        SYSTEM_RESET();          /* drv_hw.h:31 = mcu_reset(); believed IRQ-safe (INFERRED) */
        s_noProgress = 0;        /* unreachable guard for a port where reset returns */
    }

    return MOES_LIVENESS_SAMPLE_MS * TIMER_TICK_1US_GET(TIMER_IDX_0) / 1000U;
}
```

`MOES_LIVENESS_PROGRESS_RESET_TICKS` =
`MOES_LIVENESS_PROGRESS_RESET_S * 1000U / MOES_LIVENESS_SAMPLE_MS`, with
`MOES_LIVENESS_PROGRESS_RESET_S` = 60. It replaces
`MOES_LIVENESS_UNJOINED_RESET_S` in meaning (the joined bit no longer appears in
the condition).

### 2.3 Arming semantics (unchanged from build 07)

- `moes_livenessBootedOnNetwork()` — `light/moes_liveness.c:88-92`, called at
  `light/zb_appCb.c:151-155` when `zbdemo_bdbInitCb` reports `joinedNetwork==1`
  (i.e. a boot with credentials, *before* the rejoin completes). Sets
  `s_armed=TRUE`, then starts both timers.
- `moes_livenessJoined()` — `light/moes_liveness.c:97-102`, called at
  `light/zb_appCb.c:210-214` on `BDB_COMMISSION_STA_SUCCESS`. Sets
  `s_armed=TRUE`, zeroes progress, starts both timers.
- A factory-new device is never armed until SUCCESS, so pairing is never reset
  (S3). The two target wedge classes both occur *after* one of these arm points,
  so the armed gate loses nothing.

`moes_livenessEnsureTimer()` (`light/moes_liveness.c:73-83`) now starts two
things:

```c
static bool s_tickerRunning = FALSE;
static bool s_hwArmed       = FALSE;

static void moes_livenessEnsureTimer(void)
{
    if(!s_tickerRunning){
        s_tickerRunning =
            (TL_ZB_TIMER_SCHEDULE(moes_livenessProgressTickerCb, NULL,
                                  MOES_LIVENESS_PROGRESS_TICK_MS) != NULL);
    }
    if(!s_hwArmed){
        drv_hwTmr_init(TIMER_IDX_0, TIMER_MODE_SCLK);
        drv_hwTmr_set(TIMER_IDX_0,
                      MOES_LIVENESS_SAMPLE_MS * 1000,
                      moes_livenessHwSampleCb, NULL);
        s_hwArmed = TRUE;
    }
}
```

If the ticker cannot be scheduled (pool full), `s_tickerRunning` stays FALSE and
the IRQ sampler will simply observe zero progress and reset after the fuse — the
*correct* outcome, and the opposite of build-07's "silently degrade to build 06"
(`liveness_no_fire.md` §2 #5). The old `t_liveness_timer_pool_exhausted` host
test must be rewritten accordingly (see §5).

### 2.4 Factory-reset-gesture safety of an *unmarked* IRQ reset

The IRQ path deliberately does **not** call `moes_resetSkipNextBoot()`
(`light/moes_flashcfg.c:112-115`). Justification:

1. **IRQ-safety:** `moes_resetSkipNextBoot()` calls `nv_flashWriteNew`, which
   takes IRQ-off critical sections in `drv_flash.c` and is not re-entrant. An NV
   write from a timer IRQ can corrupt the flash write or deadlock — precisely
   the kind of unrecoverable fault this device cannot afford (SWire writes
   broken, OTA-only recovery). This is the same reason the liveness_no_fire.md
   §7 sketch refuses the NV write in IRQ.
2. **The 3-power-cycle gesture is still safe** because the *unmarked* wedge reset
   cannot accumulate the power count. `factoryRst_init()`
   (`apps/common/factory_reset.c:77-86`) increments `factoryRst_powerCnt` on
   every boot and schedules a 2 s timer (`FACTORY_RESET_TIMEOUT`, `:31`) that
   resets the count to 0 (`factoryRst_timerCb`, `:57-68`). A wedge→reset→wedge
   cycle is ~60 s (fuse) + boot time — far above 2 s — so the count is always
   cleared before the next wedge reset arrives and can never reach
   `FACTORY_RESET_POWER_CNT_THRESHOLD`. The only reset cadence that could
   accumulate is one faster than ~2 s, which the liveness fuse cannot produce.
3. **Rescue probation is unaffected by the missing mark.** `moes_rescueBootCheck()`
   (`light/moes_rescue.c:38-74`) runs on every boot in task context and
   increments `s_failCnt` + writes NV on *every* un-clean boot regardless of the
   skip flag (the skip flag only suppresses the factory-reset power count, not
   the probation counter). So a wedge→reset loop still drives probation to
   `MOES_RESCUE_FAIL_THRESHOLD` and latches rescue mode.

The brief's phrase "no progress for N s -> marked SYSTEM_RESET" is therefore
satisfied in the sense that matters: the reset is *deliberate* and *tracked by
the rescue probation counter on the next boot*, even though no NV skip flag is
written from IRQ.

---

## 3. Probation / stable-timer fixes

Current defect: `moes_rescueStableTimerCb` (`light/moes_rescue.c:111-132`)
clears on `zb_isDeviceJoinedNwk()` alone. In the class-2 wedge `joined` stays 1
(`boothang_stack.md` §1: joined byte `0x8474c5 = 0xc4`, bit 2 set), so the
20-minute clock counted down and `moes_rescueClear()` zeroed the counter during
a boot that then wedged (`boothang_stack.md` §3).

New rule: **clear requires joined AND continuous progress, with a one-minute
confirmation window after the countdown reaches zero.**

```c
static bool s_confirm = FALSE;
static u8   s_stableLastProgress = 0;

static s32 moes_rescueStableTimerCb(void *arg)
{
    (void)arg;

    u8 p = moes_livenessProgressCount();      /* volatile read, task context */

    if(!zb_isDeviceJoinedNwk() || p == s_stableLastProgress){
        /* Fell off, or the scheduler stopped making progress: restart, do not
         * pause, and never commit. */
        s_minsLeft          = MOES_RESCUE_STABLE_MINUTES + 1;
        s_confirm           = FALSE;
        s_stableLastProgress = p;
        return 0;
    }

    s_stableLastProgress = p;

    if(s_minsLeft){
        s_minsLeft--;
    }

    if(s_minsLeft == 0 && !s_confirm){
        /* Countdown reached zero: require ONE more minute of joined+progress
         * before the NV write. This closes the boothang timing race, where the
         * wedge lands exactly at minute 20. */
        s_confirm = TRUE;
        return 0;
    }

    if(s_minsLeft == 0 && s_confirm){
        moes_rescueClear();
        s_stableTimer = NULL;
        return -1;
    }

    return 0;
}
```

- The per-tick `p == s_stableLastProgress` gate means "joined but wedged" can
  never decrement/clear. `s_progress` advances ~1 Hz (ticker) while healthy; a
  stable tick is 60 s, so a healthy boot always sees `p` change (mod-256 wrap of
  60 increments is non-zero).
- The confirmation minute (`s_confirm`) makes the *effective* healthy window
  `MOES_RESCUE_STABLE_MINUTES + 1` = 21 min, still below the ~30 min worst-case
  OTA (`moes_rescue.h:74-79`). A wedge that lands at minute 20+ε now hits before
  the clear commits, so the probation counter survives and the new fuse resets
  the device ~60 s later.
- The new getter `u8 moes_livenessProgressCount(void)` simply returns the
  volatile `s_progress`; no other coupling between the two modules.

---

## 4. Hook points (`light/` only)

The prebuilt MAC lib and the vendored SDK are **not** patched. All changes land
in the `light/` app sources we own; the design only *calls* SDK symbols that
already exist.

| # | File:line (current tree) | Change |
|---|---|---|
| 1 | `light/moes_liveness.c:20-24` | Replace `s_armed`, `s_silence`, `s_timer` with `volatile bool s_armed`, `volatile u8 s_progress`, IRQ-only `s_noProgress`/`s_lastProgress`, task-only `s_tickerRunning`/`s_hwArmed`. |
| 2 | `light/moes_liveness.c:33-68` | Delete `moes_livenessSampleCb`; add `moes_livenessProgressTickerCb` (task, `TL_ZB_TIMER_SCHEDULE`) and `moes_livenessHwSampleCb` (IRQ, `drv_hwTmr`). |
| 3 | `light/moes_liveness.c:73-83` | `moes_livenessEnsureTimer` starts ticker + `drv_hwTmr_init/set(TIMER_IDX_0, ...)`. |
| 4 | `light/moes_liveness.c:88-92, 97-102` | Arm functions keep signatures; set `s_armed` + ensure timers. |
| 5 | `light/moes_liveness.c:107-110` | `moes_livenessStackActivity` now `s_progress++` (corroboration). |
| 6 | `light/moes_liveness.h:31-63` | Rewrite rule doc: progress-based, joined-blind, unmarked IRQ reset. |
| 7 | `light/moes_liveness.h:80-95` | Rename `MOES_LIVENESS_UNJOINED_RESET_S` → `MOES_LIVENESS_PROGRESS_RESET_S` (=60); add `MOES_LIVENESS_PROGRESS_TICK_MS` (=1000); declare `u8 moes_livenessProgressCount(void)`. |
| 8 | `light/moes_rescue.c:24-28` | Add `s_confirm`, `s_stableLastProgress`. |
| 9 | `light/moes_rescue.c:111-132` | New `moes_rescueStableTimerCb` with progress gate + confirmation minute (§3). |
| 10 | `light/zb_appCb.c` | No edit required. Existing arm/activity call sites (`:151-155`, `:202-206`, `:210-214`) already feed the new design unchanged. |

No `build/tl_zigbee_sdk/**` edits, no `light/moes_flashcfg.c` edits, no changes
to `zb_isDeviceJoinedNwk` call sites beyond the stable-timer block.

---

## 5. Host-test additions (`tools/rescue_hosttest`)

The harness currently compiles the real `light/moes_rescue.c` and
`light/moes_liveness.c` (`test_rescue.c:1-130`). It must gain a second,
independently-tickable timer substrate and a wedge primitive before the new
design can be tested honestly:

- Add `host_hwTmrSchedule(cb, arg, us)` + a hardware-timer tick list, and a
  `tick_hw_all(n)` that fires only hw timers. Shim additions to
  `shim/tl_common.h`: `drv_hwTmr_init`, `drv_hwTmr_set`, `TIMER_IDX_0`,
  `TIMER_MODE_SCLK`, `TIMER_TICK_1US_GET`, `HW_TIMER_SUCC`, `timerCb_t`.
- Add `host_wedge(void)` — freeze the *task* timer list so `tick_all` stops
  firing task callbacks (models an earlier `ev_timer` cb that never returns, or
  `ev_poll` stuck), while `tick_hw_all` keeps firing the IRQ sampler. This
  closes the exact honesty gap liveness_no_fire.md §2 #1 called out: the old
  harness could not represent a blocking callback.

### Updated/kept scenarios (existing 22, revised)

- **Rewritten:** `liveness_wedge_reset` → now models a class-1 wedge via
  `host_wedge()`; the reset is **unmarked** (drop the `host_skipWritesAtReset>=1`
  assertion — there is no IRQ NV write).
- **Rewritten:** `liveness_pool_exhausted` → a full `ev_timer` pool means the
  *ticker* can't be scheduled, so the hw sampler must reset after the fuse
  (invert the old "degraded but alive" assertion).
- **Rewritten:** `liveness_activity_suppresses` → coordinator-offline is now
  modelled as "task list NOT wedged, joined toggling, no BDB activity"; assert
  zero resets.
- **Adapted:** `wedge_reset_rescue_chain` → keep, but drive it through the new
  two-layer substrate; assert the unmarked wedge reset still increments
  probation and latches rescue on boot `MOES_RESCUE_FAIL_THRESHOLD+1`.
- Keep unchanged: `fresh_device`, `below_threshold`, `at_threshold`,
  `above_threshold`, `gesture_clears`, `gesture_cannot_latch`,
  `garbage_nv_failsafe`, `unreadable_nv`, `unwritable_nv`,
  `timer_only_when_needed`, `latch_after_threshold`, `flash_wear_bound`,
  `watchdog_hang_chain`, `liveness_rejoin_restarts`, `liveness_pairing_never_reset`.
- `stable_clears` and `drop_restarts_clock` keep their intent but must now
  exercise the progress gate: a healthy run needs the task ticker firing, and a
  "drop" is joined=0 OR no-progress.

### New scenarios (required by this task)

1. **`liveness_class2_wedge_resets_while_joined`** — arm; `joined=1`; tick task
   normally for a few seconds (progress advances); `host_wedge()`; tick hw
   `MOES_LIVENESS_PROGRESS_RESET_TICKS` times. Assert exactly one reset and that
   `joined` stayed 1 throughout (proves the fuse is joined-blind).
2. **`rescue_stable_no_clear_when_wedged`** — boot with `nv_value=2`; `joined=1`;
   start stable timer; run 19 healthy minutes (ticker firing); `host_wedge()`
   before the 20th stable tick; tick task+ hw. Assert `moes_rescueFailCount()!=0`,
   no NV clear write, and (after the hw fuse) a reset.
3. **`rescue_stable_confirm_window_blocks_minute20_clear`** — healthy 20 min
   (countdown reaches 0, `s_confirm` set); `host_wedge()` during the confirmation
   minute; assert no `moes_rescueClear()` NV write and no count==0.
4. **`liveness_coordinator_offline_no_reset`** — arm; `joined=0` (or toggling);
   task list NOT wedged (ticker keeps firing); tick task+hw for several fuse
   windows. Assert zero resets (scheduler alive while network absent).
5. **`liveness_unmarked_reset_does_not_accumulate_factory_reset`** — model the
   `factory_reset.c` 2 s clear window vs the ~60 s wedge-reset cycle across many
   boots; assert `factoryRst_powerCnt` never reaches the threshold, proving the
   IRQ path's missing `moes_resetSkipNextBoot()` is safe (this replaces the
   deleted "marked" assertions with a real safety proof).

`make check` in `tools/rescue_hosttest` must stay green with all 22 original
(where kept) plus the 5 new scenarios.

---

## 6. Failure-mode analysis

| # | Failure mode | Effect | Mitigation / status |
|---|---|---|---|
| F1 | Wedge lands with IRQs disabled (e.g. stuck inside `mac_csmaStart`'s IRQ-off CCA section) | Timer0 IRQ blocked → sampler never fires | **Open hardware unknown.** Class-1 capture shows an `rf_rx_irq_handler` frame (IRQs live at capture), and class-2 is in `ev_poll` data processing (IRQs live); both are consistent with IRQs enabled, but not guaranteed for future wedges. Must verify `reg_irq_mask` (0x640) on a forced-hang bench run before shipping. Applies equally to the liveness_no_fire.md §7 sketch. |
| F2 | `TIMER_IDX_0` already used by another linked object | `drv_hwTmr_set` returns `HW_TIMER_IS_RUNNING`, timer overwritten | INFERRED free (only Timer2=watchdog, Timer3=MAC CSMA are documented users; `drv_timer.h:46-50`). Grep the build-08 ELF for `drv_hwTmr_set(TIMER_IDX_0`/`drv_timer_irq0_handler` before choosing an index; fall back to `TIMER_IDX_1`. |
| F3 | `mcu_reset()` from IRQ context | Reset not actually taken, or corruption | Believed safe (register write, no return; `drv_hw.h:31`); not host-testable. Mark INFERRED; bench-verify once. |
| F4 | A legitimate task blocks `ev_main` > fuse window | False reset | No known path blocks >60 s; OTA is chunked and flash sector ops are ~ms (INFERRED, confirm against OTA soak). The 60 s threshold gives ~60× margin over the ticker period. |
| F5 | BDB events keep firing during a future wedge, holding `s_progress` open via `StackActivity` | Fuse delayed | Not possible in the two known wedges (class 1 total freeze; class 2 has no BDB cb in the chain). Residual risk for an *unknown* third class; the primary ticker still stops in any `ev_timer`/`ev_main` starvation, and `StackActivity` only *adds* liveness, it cannot subtract it. Acceptable. |
| F6 | `s_progress` u8 wraps in < fuse window | Mis-detection | At 1 Hz the wrap is 256 s; the IRQ sampler compares equality, and a 256 s no-progress stretch is far above the 60 s fuse, so wrap cannot cause a missed reset. The stable timer samples 60 s apart (60 ≠ 0 mod 256), so no false "no progress". |
| F7 | Rescue stable timer itself starved during wedge | Cannot clear (desired) | The progress gate + confirmation minute ensure the *only* path to `moes_rescueClear()` requires demonstrable health; a starved stable timer simply never commits. |
| F8 | Coordinator offline > 20 min with `joined` staying 1 | Stable clock clears during a live outage | Residual: the progress gate alone does not distinguish "scheduler alive, network down" from "healthy". In practice `PARENT_LOST` clears `joined` ~70 s after loss (`mac_scan_wedge.md` §4), which restarts the clock. If this ever proves false on hardware, add a second gate requiring at least one *network-level* event (e.g. `T_DBG_irqTest[0]` TX-done change) within the stable window. Not required for the two observed wedges. |

---

## 7. Inferred / unverified, stated plainly

- **"TX-done fires regardless of ACK"** (signal A, S5/S1) is INFERRED from
  `mac_scan_wedge.md` §1 (the TX-done timeout is armed after `rf802154_tx()`
  regardless of ACK outcome) and standard 802.15.4 TX-IRQ semantics; not
  re-verified against the radio spec here.
- **IRQs live during both wedges** is inferred from the captured stacks
  (`rf_rx_irq_handler` frame in class 1; active data processing in class 2), not
  from a register read of `reg_irq_mask`.
- **Timer0 free / `mcu_reset()` IRQ-safe** are INFERRED and must be confirmed
  against the build-08 ELF and a bench run, exactly as liveness_no_fire.md §9
  already requires.
- **"No legitimate path blocks `ev_main` > 60 s"** is INFERRED from the OTA/flash
  call sites; it should be re-confirmed with an OTA soak on a bench unit.
- **Class-2 TX queue emptiness** (so signal A stops) is INFERRED from
  `boothang_stack.md` §1's "no active scan, TX slot post-timeout, radio RX"; a
  MAC-level retry is bounded by `macMaxCSMABackoffs`=4, so A would stop within a
  bounded time, but this is the reason A is not the primary signal.
- The **2 s factory-reset window** analysis (§2.4) is a code reading of
  `factory_reset.c:30-86`, not a bench run; the conclusion should be confirmed
  with a deliberate wedge loop before relying on it.

## 8. What this does NOT attempt

- It does **not** fix the underlying MAC scan or APS/NWK wedge; it only makes the
  device recover from them by resetting into probation/rescue.
- It does **not** make the 600 ms hardware watchdog fire; that remains the open
  silicon question owned by `watchdog_defeat.md`.
- It does **not** move the OTA/leave/exception reset paths or change rescue-mode
  behavior beyond the stable-clear gate.
