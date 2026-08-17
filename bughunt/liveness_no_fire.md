# liveness_no_fire — why build 07's wedge fuse did not fire on the bench unit

**Scope:** paper analysis only. No code changed, nothing flashed, no radio/MQTT/docker action.
**Device:** bench unit `<redacted-device>`, build 07 (HEAD `87a4e2d`). OTA finished
14:29 UTC, probe answered 14:33:46 UTC, silent by 14:37 UTC, zero observable resets/rejoins
for 25+ min. Host tests 22/22 (`tools/rescue_hosttest`, `make check`).
**Verified against:** `light/moes_liveness.{c,h}`, `light/moes_rescue.{c,h}`,
`light/zb_appCb.c`, `light/tuyaLight.c`, `switch/zb_appCb.c`, the vendored SDK sources
(`ev_timer.c`, `ev.c`, `main.c`, `drv_timer.c`, `bdb.c`, `nwk_ctx.h`, `factory_reset.c`)
and the build-07 ELF listing `build/light/light_TS0505B.lst`. Claims that could not be fully
proven are marked INFERRED.

---

## Verdict up front

The fuse **could not fire by construction**: `moes_livenessSampleCb` is a *cooperative
software timer* on the very same `ev_timer.timer_head` list that the wedged MAC rejoin scan
occupies. The build-06 hang captures show the CPU stuck inside the scan timer callback
(`tl_zbMacScanRunning` → beacon-request → CSMA) with an RX IRQ on top of it; `ev_timer_process`
is called **only** from `ev_main()` (`ev.c:71`), so once the scan timer callback never returns,
`ev_timer_executeCB()` never advances to the liveness entry and the 60 s silence clock is never
serviced. The monitor that is supposed to detect "the stack stopped making progress" is itself
inside the stack's progress machinery.

Every other part of the chain (arm on the OTA-reboot boot, the joined-bit semantics, the
probation counter, the reset-skip mark) checks out on paper. The single fatal assumption is the
one written into `moes_liveness.h:11-14`: *"The main loop keeps running, so the hardware
watchdog keeps being fed and never fires."* The cited SRAM evidence says the opposite — the main
loop is **not** returning (`mac_scan_wedge.md` §3: the stack chain ends in `main`, byte-identical
across 3 min). A cooperative timer cannot observe a wedge that starves cooperative timers.

---

## 1. The chain as designed vs. what happened

Designed chain (`moes_liveness.h:31-63`):

```
armed once this boot has network credentials
  -> continuously unjoined  (zb_isDeviceJoinedNwk()==0)
  -> AND zero BDB commissioning events for 60 s
  -> moes_resetSkipNextBoot(); SYSTEM_RESET()
  -> next boot: moes_rescueBootCheck() increments probation
  -> boot 7 latches OTA-only rescue mode
```

Observed: OTA boot rejoined (probe answered 14:33:46), then parent loss ~70–95 s after the last
inbound frame, then nothing — no reset, no rejoin, no announce, for 25+ min. If the fuse had
tripped, a reset + rejoin would have produced observable traffic inside ~60 s.

The question is therefore not "was the logic wrong" — it is **"which assumption about the real
stack is false."** The ranked table below is the answer.

---

## 2. Ranked candidate failure modes

| # | Candidate | Mechanism | Evidence (`file:line`) | Why host test can't see it | On-hardware confirm / refute |
|---|---|---|---|---|---|
| **1** | **Cooperative-timer starvation — the sampler never runs during the wedge** | `moes_livenessEnsureTimer` schedules the sampler with `TL_ZB_TIMER_SCHEDULE` = `ev_timer_taskPost` (`ev_timer.h:134`) into the single `ev_timer.timer_head` list. `ev_timer_process()` (which calls `ev_timer_update` then `ev_timer_executeCB`) is reached **only** from `ev_main()` (`ev.c:69-75`), which the main `while(1)` calls at `main.c:85`. The MAC scan timer is scheduled via the same `ev_timer_taskPost` (INFERRED, per `mac_scan_wedge.md`: "scheduled via ev_timer_taskPost at tl_zbMacScanRequestHandler"). When the scan timer cb wedges and never returns, `ev_timer_executeCB` is stuck at its `timerEvt->cb(...)` call (`ev_timer.c:263`) and never reaches `moes_livenessSampleCb`. | `light/moes_liveness.c:79`; `proj/os/ev_timer.c:220-311`, `:256-286`, `:263`; `proj/os/ev.c:69-75`; `apps/common/main.c:73-96`; wedge stack in `bughunt/mac_scan_wedge.md` §2 | `tools/rescue_hosttest/test_rescue.c:88-130`: `host_timerSchedule` returns an independent slot, and `tick_all` calls *every* live timer's cb each step. There is no list ordering, no "earlier cb never returns", and no IRQ that can preempt a cb — so a blocking scan timer cannot be represented. | SRAM: `s_armed`==1, `s_silence` frozen at a value < 12, `s_timer` != NULL, `g_zbNwkCtx` joined bit == 0 (see §8 addresses). Also `ev_timer.timer_head` shows the scan timer entry `timeout==0`/`isRunning==1` and the liveness entry `timeout` unchanged between two captures 3 min apart. |
| **2** | **`zb_isDeviceJoinedNwk()` still returns TRUE on *this* wedge (joined bit never cleared)** | The first branch of the sampler (`moes_liveness.c:37-43`) returns 0 and zeroes `s_silence` whenever joined is true. If build 07's wedge leaves `g_zbNwkCtx.joined` set, the fuse never counts silence. **The function itself reads the correct bit** — disassembly §3 — so this is *not* a "wrong bit" bug; it requires a different wedge flavor than build 06. | `light/moes_liveness.c:37-43`; §3 disassembly; `nwk_ctx.h:105` | Host test models joined as an `int` toggled by the scenario (`test_rescue.c:70-72`); it cannot model the stack failing to clear the real `g_zbNwkCtx.joined` bit. | REFUTED for build-06 (captures show joined==0, `moes_liveness.h:20-22`); UNVERIFIED for build-07. SRAM: byte `g_zbNwkCtx+0x2d` bit 2 (0x04) == 0 → refuted; == 1 → this mode is live. |
| **3** | **BDB commissioning events keep firing during the wedge and hold the fuse open** | Every commissioning callback calls `moes_livenessStackActivity()` before the switch (`zb_appCb.c:202-206`), which zeroes `s_silence`. If the nwk layer's rejoin retry loop completes-and-fails repeatedly (each `REJOIN_FAILURE`/`PARENT_LOST` reaching the app cb) instead of freezing, the fuse is continuously reset. | `light/zb_appCb.c:199-206`, `:270-272`; `light/moes_liveness.c:107-110` | Host test models activity as explicit `moes_livenessStackActivity()` calls between ticks; it cannot model the *stack* autonomously emitting events during a scan. | REFUTED for build-06 (total freeze — no callback fires, `mac_scan_wedge.md` §2/§4); UNVERIFIED for build-07. SRAM: `s_silence` would be stuck low while a scan-retry counter keeps advancing; z2m would see repeated rejoin/announce traffic — none was seen. |
| **4** | **Monitor never armed on the OTA-reboot boot** | If the boot after a successful OTA did not call `moes_livenessBootedOnNetwork()`, the monitor would stay disarmed and never fire. | `light/zb_appCb.c:145-156` (arm in `zbdemo_bdbInitCb` on `joinedNetwork==1`); `:302-321` (OTA handler `ota_mcuReboot()`); `light/tuyaLight.c:463` (`bdb_init`). | Host test arms the monitor by explicit calls; it does not model the `bdb_init` callback path. | REFUTED: the OTA boot rejoined (probe answered), so `zbdemo_bdbInitCb(SUCCESS, joined==1)` ran → armed; the later SUCCESS also called `moes_livenessJoined()`. |
| **5** | **Timer pool full → `TL_ZB_TIMER_SCHEDULE` returns NULL → "silently degrades to build 06"** | `moes_liveness.c:79-82` and the host test assume a NULL return just means no sampler this boot. On the real stack `ev_timer_add` does `ZB_EXCEPTION_POST(SYS_EXCEPTTION_COMMON_TIMER_EVEVT)` before returning NULL (`ev_timer.c:175-181`), and `ZB_EXCEPTION_POST` → `sys_exceptionPost` → the app's `tuyaLightSysException` → `SYSTEM_RESET()` (`ev.c:33-44`, `ev.h:77`, `light/tuyaLight.c:340-372,423`). So a full pool is an *unmarked reset*, not silence — the opposite of the no-fire. | `light/moes_liveness.c:79-82`; `proj/os/ev_timer.c:175-181`; `proj/os/ev.c:33-44`; `light/tuyaLight.c:340-372` | Host test's `host_timerSchedule` returns NULL with no exception (`test_rescue.c:88-103`), and `t_liveness_timer_pool_exhausted` asserts "no reset". The real pool-full path resets. | This mode predicts *more* resets, so it cannot explain 25 min of silence. Listed because it is a real host-test divergence that must be fixed in any patch that touches this code. |
| **6** | **`SYSTEM_RESET()` reached but ineffective** | If the fuse tripped and `mcu_reset()` failed to actually reset the chip. | `light/moes_liveness.c:61`; `drv_hw.h:31` (`SYSTEM_RESET()` = `mcu_reset()`) | Host test's `host_systemReset` is a counter; it cannot test silicon reset. | Extremely unlikely (the same macro is used by the OTA/leave/exception paths, all proven to reset). Would be refuted by `s_failCnt`>1 in SRAM — it is 1, so no reset happened at all. |

**Top candidate: #1.** #2 and #3 are the only alternatives that could independently produce the
same 25-min silence, but both are refuted for the build-06 wedge and have no positive evidence
for build-07. #1 is a structural property of the firmware, not a wedge-specific accident, so it
applies to *every* wedge flavor, including the one already proven on this silicon.

---

## 3. `zb_isDeviceJoinedNwk()` — it reads exactly the bit the analysis says

Build-07 ELF, `light_TS0505B.lst:92716`:

```
zb_isDeviceJoinedNwk():
  35d9c: tmovs r3, #45 (0x2d)
  35d9e: tloadr r2, [pc, #8]    ; r2 = 0x008474a0 (g_zbNwkCtx)
  35da0: tloadrb r0, [r2, r3]    ; byte at g_zbNwkCtx + 0x2d
  35da2: tshftls r0, r0, #29     ; isolate bit 2 (joined)
  35da4: tshftrs r0, r0, #31     ; sign-extend -> nonzero iff joined
```

`nwk_ctx.h:103-105` lays out the bitfield byte at offset 45 (`0x2d`) as
`is_factory_new:1, permit_join:1, joined:1, ...` — so bit 2 **is** `g_zbNwkCtx.joined`.
Conclusion: **the function the monitor calls reads the same `joined` bit the build-06 captures
showed as 0.** The "reads a different bit" hypothesis is dead. The residual risk is only that a
*different* wedge leaves `joined` set (candidate #2), which is unverified for build-07 and should
be checked against SRAM before any firmware change is shipped.

---

## 4. Timer architecture — the shared-list starvation, with source

- `TL_ZB_TIMER_SCHEDULE(cb,arg,t)` is literally `ev_timer_taskPost(cb,arg,t)` (`ev_timer.h:134`).
- `ev_timer_taskPost` → `ev_timer_add` → `ev_on_timer` inserts into the **single** list
  `ev_timer.timer_head` (`ev_timer.c:122-202`). There is one list for app timers and every
  stack timer that uses `ev_timer_taskPost`.
- `ev_timer_process()` does the only `ev_timer_update()` + `ev_timer_executeCB()` call
  (`ev_timer.c:288-311`). It is reached **only** from `ev_main()` (`ev.c:71`), which is the first
  call in the main `while(1)` (`main.c:85`).
- `ev_timer_executeCB()` is a `while` over `ev_timer.timer_head` and calls each ready timer's
  `cb` in line (`ev_timer.c:256-286`). A callback that never returns stops the loop; the timers
  behind it are never serviced.
- The MAC scan timer is a `ev_timer_taskPost` timer (INFERRED, `mac_scan_wedge.md`: the scan
  timer task id in SRAM is an `ev_timer_event_t`, and the wedge stack runs through the timer
  service into `tl_zbMacScanRunning`). Therefore the wedged scan and the liveness sampler are
  neighbours on the same list.

Consequence, stated plainly: when the scan wedges, `ev_main()` never returns to `main()`, so
`drv_wd_clear()` at `main.c:88/94` is never reached **and** `moes_livenessSampleCb` is never
called. The liveness monitor cannot fire because it is scheduled on the subsystem that the wedge
starves. (The hardware watchdog's own non-fire is a separate, still-open silicon question owned
by `watchdog_defeat.md`; it is not the reason the *liveness* fuse failed.)

---

## 5. Every boot flavour arms the monitor (so "not armed" is not the cause)

- **OTA-reboot boot:** `tuyaLight_otaProcessMsgHandler` marks the reboot, then `ota_mcuReboot()`
  (`zb_appCb.c:302-321`) → the new image boots → `user_init` → `stack_init()` → `bdb_init()`
  (`tuyaLight.c:396,463`). With credentials present, `zbdemo_bdbInitCb(SUCCESS, joinedNetwork=1)`
  runs and calls `moes_livenessBootedOnNetwork()` (`zb_appCb.c:145-156`) — armed before any
  rejoin completes.
- **Fresh join:** `BDB_COMMISSION_STA_SUCCESS` → `moes_livenessJoined()` (`zb_appCb.c:210-214`).
- **Rejoin / rescue boot:** same two callbacks; `moes_liveness.h:62` deliberately runs the monitor
  in rescue mode.
- The arm runs in task context (BDB callbacks), never in IRQ, and is after `stack_init()`, so it
  satisfies `MOES_EDITING_GUIDE.md` §0.

The probe answer at 14:33:46 proves at least one SUCCESS happened post-OTA, so both arm points
had fired. The monitor was armed and its timer was scheduled.

---

## 6. Switch app's `BDB_COMMISSION_STA_PARENT_LOST` handling — adopting it would not help

- Light app (`light/zb_appCb.c:208-280`): handles `REJOIN_FAILURE` (immediate `zb_rejoinReq`),
  **no** `PARENT_LOST` case.
- Switch app (`switch/zb_appCb.c:224-228`): handles `PARENT_LOST` with an immediate
  `zb_rejoinReq`, plus a 60 s backoff timer on `REJOIN_FAILURE` (`:229-234`).
- The stack already sets `PARENT_LOST` and schedules the BDB rejoin-done path itself
  (`bdb.c:1239-1269` → `bdb_task` `BDB_STATE_REJOIN_DONE` → `bdb_topLevelCommissiongConfirm`),
  so the app's callback is a *notification*, not the only rejoin trigger.

Adopting switch-style `PARENT_LOST` handling in the light app would issue an **extra**
`zb_rejoinReq` into the exact `tl_zbMacScanRunning` beacon-request path that wedges. It cannot
un-wedge a frozen scan; it can only offer the stack another chance to enter it. It also does not
change the fuse semantics: both `PARENT_LOST` and `REJOIN_FAILURE` already call
`moes_livenessStackActivity()` first (the switch at `zb_appCb.c:208-280` is preceded by
`moes_livenessStackActivity()` at `:202-206`), so both would hold the fuse open identically.
**Net: neutral-to-harmful for this wedge class; do not adopt it as a fix.** The light app's
existing `REJOIN_FAILURE` retry already covers the "coordinator offline but stack alive" case.

---

## 7. Minimal patch sketch (top candidate #1)

The fix is not to change the fuse's *rule*; it is to move the sampler off the starved cooperative
list and onto a hardware-timer IRQ, which `drv_hwTmr_set` services independently of `ev_main`
(`drv_timer.c:155-176` — the callback runs inside `drv_timer_irq0_handler`, `:234-237`).

```c
/* light/moes_liveness.c — conceptual diff only; do NOT apply without the checks in §8. */

#if (MOES_TS0505B && MOES_LIVENESS_ENABLE)

#define MOES_LIVENESS_RESET_TICKS   (MOES_LIVENESS_UNJOINED_RESET_S * 1000U / MOES_LIVENESS_SAMPLE_MS)

/* volatile: the sampler now runs in the hardware-timer IRQ and the arm/activity
 * calls run in task context; both sides touch these. */
static volatile bool s_armed   = FALSE;
static volatile u8   s_silence = 0;
static bool          s_hwArmed = FALSE;

static s32 moes_livenessSampleCb(void *arg)
{
	(void)arg;

	if(zb_isDeviceJoinedNwk()){
		/* Joined: healthy AND proof of credentials. Self-arm here so no join
		 * path can be missed. RAM-only read, IRQ-safe. */
		s_armed = TRUE;
		s_silence = 0;
		return 0;                       /* re-arm same period (drv_timer.c:166-175) */
	}

	if(!s_armed){
		return 0;                       /* factory-new / pairing: nothing to return to */
	}

	if(s_silence < 0xFF){
		s_silence++;
	}

	if(s_silence >= MOES_LIVENESS_RESET_TICKS){
		/* Wedged. NOTE: we deliberately do NOT call moes_resetSkipNextBoot()
		 * here. This cb now runs in IRQ context, and that helper does an NV
		 * flash write which is NOT IRQ-safe (drv_flash.c takes IRQ-off critical
		 * sections and is not re-entrant). The reset is therefore unmarked.
		 *
		 * That is safe for the 3-power-cycle gesture: factoryRst_init()
		 * increments the count on every boot but its 2 s timer clears it again
		 * (factory_reset.c:62-73,129-135). A wedge->reset->wedge cycle is ~70 s
		 * long, far above the 2 s window, so the count never accumulates past 1
		 * and can never reach FACTORY_RESET_POWER_CNT_THRESHOLD. Rescue
		 * probation is unaffected: moes_rescueBootCheck() runs on every boot in
		 * task context and still counts each wedge-reset boot toward the latch.
		 *
		 * INFERRED, must be bench-verified: mcu_reset() from IRQ context works,
		 * and Timer0/Timer1 are truly free on this target (Timer2 = watchdog,
		 * Timer3 = MAC CSMA). */
		SYSTEM_RESET();
		s_silence = 0;                  /* unreachable; guard a port that returns */
	}

	return 0;
}

static void moes_livenessEnsureTimer(void)
{
	if(s_hwArmed){
		return;
	}

	/* Hardware timer, not TL_ZB_TIMER_SCHEDULE. ev_timer.timer_head is the same
	 * list the wedged MAC scan occupies; a sampler there is starved by the very
	 * wedge it is meant to detect. drv_hwTmr IRQ runs regardless of ev_main. */
	drv_hwTmr_init(TIMER_IDX_0, TIMER_MODE_SCLK);
	drv_hwTmr_set(TIMER_IDX_0, MOES_LIVENESS_SAMPLE_MS * 1000, moes_livenessSampleCb, NULL);
	s_hwArmed = TRUE;
}
```

`moes_livenessBootedOnNetwork`, `moes_livenessJoined` and `moes_livenessStackActivity` keep their
current call signatures; only `EnsureTimer` changes. `s_timer` is deleted. The arm/activity
functions stay task-context, so the §0 ordering rule is untouched.

What this sketch intentionally leaves to follow-up (all must be verified before shipping):

1. **IRQ-enablement during the wedge.** The build-06 stack shows an `rf_rx_irq_handler` frame on
   top of the scan path, which implies IRQs were live at the wedge instant — but `mac_csmaStart`
   also enters an IRQ-off CCA section. If the wedge lands with IRQs disabled, a Timer0 IRQ is
   blocked too. This is the single most important on-hardware unknown. Confirm/refute: the wedge
   SRAM PC + `reg_irq_mask` (0x640) / `reg_irq_src` state, or a forced-hang bench run.
2. **`mcu_reset()` from IRQ.** Believed safe (register write + no return); not host-testable.
3. **Timer0/Timer1 availability.** Grep the linked ELF for other `drv_hwTmr_set(TIMER_IDX_0/1)`
   users before choosing an index.
4. **Host-test honesty.** `t_liveness_timer_pool_exhausted` and the pool-full comments are wrong
   against the real stack (§2 #5): pool-full raises an exception → `SYSTEM_RESET()`, it does not
   silently degrade. The host harness must model the hardware-timer substrate (or at least
   document the divergence) when this patch lands, and it should add a scenario proving the
   sampler still fires when a *different* `ev_timer` callback never returns.

An alternative worth one line in the design discussion: keep the task-context sampler as-is, and
add a *separate* `drv_hwTmr` IRQ whose only job is to detect that `s_silence` has not advanced
for > fuse — a "watchdog of the watchdog". That preserves the marked task-context reset while
still being immune to `ev_timer` starvation, but it is more state and more code; the sketch above
is the smaller change.

---

## 8. SRAM confirm/refute map (build-07, from `light_TS0505B.lst`)

For the concurrent hardware-read agent. Two captures ~3 min apart make the starved-vs-advancing
distinction unambiguous.

| variable | address | expected in a starved wedge | what a different value means |
|---|---|---|---|
| `s_armed` | `0x842470` | `0x01` | `0x00` → arm never happened (candidate #4) |
| `s_silence` | `0x842471` | frozen `< 12` (likely `0x00`) | advancing → sampler runs (candidate #1 refuted); `>= 12` with no reset → reset path broken |
| `s_timer` (build-07 pointer) | `0x842474` | non-NULL (an `ev_timer_event_t *`) | NULL → sampler never scheduled (candidate #5 or arm gap) |
| `s_failCnt` | `0x842479` | `0x01` (proves zero resets since OTA boot) | `>1` → a reset *did* happen and was invisible on air |
| `s_rescue` | `0x842480` | `0x00` | `0x01` → rescue already latched (not this failure) |
| `s_minsLeft` | `0x842478` | `0x00` or frozen | advancing → rescue stable clock runs (and thus `ev_timer` still services *some* callbacks) |
| `g_zbNwkCtx` | `0x8474a0` | byte at `+0x2d` = `0x8474cd`, bit 2 (mask `0x04`) == 0 | bit set → candidate #2 (joined never cleared) |
| `ev_timer` ctrl | `0x846e74` (`timer_head`), pool from `0x846e7c` (24 × 24 B) | the wedged scan entry has `timeout==0`/`isRunning==1`; the liveness entry's `timeout` is unchanged between captures | liveness `timeout` decrementing → sampler is being serviced (candidate #1 refuted) |

The single decisive byte is `s_silence` at `0x842471`: **frozen** (with `s_armed==1` and the
`g_zbNwkCtx` joined bit clear) confirms candidate #1; **advancing** refutes it and points at #2
or #3.

---

## 9. Unverified / inferred, stated plainly

- The build-07 wedge is *assumed* identical to the build-06 wedge; the build-07 unit has not been
  SRAM-captured. Candidates #2 and #3 cannot be excluded until §8's reads happen.
- The claim that the MAC scan timer lives on `ev_timer.timer_head` is INFERRED from
  `mac_scan_wedge.md`'s note that it is scheduled via `ev_timer_taskPost`; the prebuilt library's
  exact scheduling call was not independently re-disassembled here.
- `drv_hwTmr` Timer0/Timer1 being free, and `mcu_reset()` being IRQ-safe, are not yet verified
  against the linked ELF/bench.
- The 2 s factory-reset-window analysis (§7) is a code reading, not a bench run; the conclusion
  (unmarked ~70 s-cycle wedge resets do not accumulate the power-count) is sound from
  `factory_reset.c` but should be confirmed with a deliberate wedge loop before relying on it.
