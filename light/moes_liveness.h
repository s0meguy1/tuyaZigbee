/********************************************************************************************************
 * @file    moes_liveness.h
 *
 * @brief   Wedge-proof liveness monitor: turn either of the two silent-stack
 *          wedge classes into a reboot, so the boot-probation/rescue machinery
 *          (moes_rescue.c) can do its job.
 *
 * The failure this closes (bughunt/liveness_no_fire.md, bughunt/boothang_stack.md,
 * bughunt/liveness_redesign.md): the stack can freeze in two different places.
 *
 *   - Class 1 (build 06, mac_scan_wedge.md): an unjoined rejoin scan freezes
 *     inside the prebuilt MAC library. No callback ever fires again.
 *   - Class 2 (build 06/07, boothang_stack.md): a joined device wedges in the
 *     APS/NWK link-status + ZCL-report data path. z2m hears nothing, but the
 *     NWK "joined" bit stays set.
 *
 * Build 07 tried to catch class 1 with a sampler on the cooperative ev_timer
 * list. That cannot work: the sampler runs on the very same list the wedge
 * starves, so it is never serviced during a wedge (liveness_no_fire.md S1).
 * Build 07's joined-bit gate also made it blind to class 2.
 *
 * The build-09 design is a two-layer heartbeat ("watchdog of the watchdog"):
 *
 *   - A task-context *progress ticker* runs on the cooperative ev_timer list
 *     and increments a volatile counter every MOES_LIVENESS_PROGRESS_TICK_MS.
 *     It is DELIBERATELY on the starved list: it advances only while ev_main is
 *     returning and ev_timer callbacks are being serviced, which is exactly the
 *     "stack is alive" property we want. When the stack wedges, the ticker
 *     starves - and that starvation is the signal.
 *
 *   - A hardware-timer IRQ sampler (drv_hwTmr TIMER_IDX_0) runs independently
 *     of ev_main and watches the counter. If the counter has not advanced for
 *     MOES_LIVENESS_PROGRESS_RESET_S, it forces SYSTEM_RESET(). The sampler is
 *     joined-blind: it does not read zb_isDeviceJoinedNwk(), so a class-2
 *     joined-but-silent wedge trips it exactly like a class-1 unjoined wedge.
 *
 * Build 10 makes that sampler a one-shot: the callback explicitly re-arms the
 * hardware timer (stop -> reload init+capture -> start) instead of relying on
 * the SDK's free-run periodic re-arm, which did not produce a real 1 s cadence
 * on silicon (bughunt/fuse_no_fire_b09.md §1d/§5; rationale lives in
 * moes_liveness.c above the re-arm).
 *
 * The rule is therefore:
 *
 *     once this boot has had network credentials (armed), if the cooperative
 *     scheduler has made no progress for MOES_LIVENESS_PROGRESS_RESET_S, force
 *     a reset - joined or not.
 *
 * Consequences, by scenario:
 *
 *   - Wedged rejoin scan (class 1): the ticker starves, the IRQ sampler fires
 *     after ~60 s. The next boot increments probation, so a repeating wedge
 *     latches rescue mode instead of bricking.
 *   - Joined-but-silent APS/NWK wedge (class 2): same - joined no longer
 *     suppresses the fuse.
 *   - Coordinator genuinely offline: the scheduler keeps running, so the ticker
 *     keeps firing and no reset happens. The fuse no longer depends on radio
 *     traffic at all.
 *   - Factory-new pairing: never armed (no credentials yet), so never reset.
 *   - Leave / factory-reset gesture: those paths reboot on their own within
 *     seconds, long before the fuse expires.
 *
 * A false positive costs one boot + one probation count and self-heals after
 * 21 stable minutes; a false negative is a brick. The bias is deliberate.
 *
 * The forced reset is deliberately UNMARKED (it does NOT call
 * moes_resetSkipNextBoot()). moes_resetSkipNextBoot() performs an NV flash
 * write, which is not IRQ-safe (drv_flash.c takes IRQ-off critical sections and
 * is not re-entrant), and this reset is taken from the hardware-timer IRQ. The
 * missing mark is safe for the 3-power-cycle gesture because the ~60 s
 * wedge->reset->wedge cycle is far longer than factory_reset.c's 2 s clear
 * window, so the power count can never accumulate to the threshold
 * (liveness_redesign.md S2.4). Rescue probation is unaffected: it counts every
 * un-clean boot on the next boot regardless of the skip flag.
 *
 * Runs in rescue mode too - a rescue-mode light is exactly the one whose wedge
 * must keep causing resets.
 *
 * @date    2026
 *******************************************************************************************************/

#pragma once

#if defined(__cplusplus)
extern "C" {
#endif

/* Master switch. Turning this off restores the pre-monitor behaviour exactly. */
#ifndef MOES_LIVENESS_ENABLE
#define MOES_LIVENESS_ENABLE                1
#endif

/* Hardware-timer IRQ sample period. One comparison of the progress counter per
 * tick; cheap and IRQ-safe. */
#ifndef MOES_LIVENESS_SAMPLE_MS
#define MOES_LIVENESS_SAMPLE_MS             1000U
#endif

/* Task-context progress ticker period. Runs on the cooperative ev_timer list
 * and is SUPPOSED to starve when the stack wedges. */
#ifndef MOES_LIVENESS_PROGRESS_TICK_MS
#define MOES_LIVENESS_PROGRESS_TICK_MS      1000U
#endif

/* How long "armed + no scheduler progress" may persist before the monitor
 * declares the stack wedged and reboots. This is the fuse for BOTH wedge
 * classes; the joined bit is deliberately absent from the condition.
 *
 * Must comfortably exceed a legitimate rejoin and any short task-context stall.
 * A live scheduler advances the progress ticker every second, so any value from
 * ~30 s up is safe against false positives; 60 s keeps the wedge->reset->rescue
 * chain under ~15 minutes of wall time. */
#ifndef MOES_LIVENESS_PROGRESS_RESET_S
#define MOES_LIVENESS_PROGRESS_RESET_S      60U
#endif

/*
 * This boot has network credentials and expects to be joined: arm the monitor.
 * Call from the BDB init callback when it reports the device is on a network.
 * Without this, a boot-time rejoin that wedges before ever joining would be
 * invisible to the monitor.
 */
void moes_livenessBootedOnNetwork(void);

/*
 * The stack completed a join/rejoin this boot: arm the monitor and restart the
 * progress baseline. Call from the BDB commissioning callback on
 * BDB_COMMISSION_STA_SUCCESS.
 */
void moes_livenessJoined(void);

/*
 * Proof the stack's commissioning state machine is alive. Call from the BDB
 * commissioning callback on *every* status, before the switch. Corroboration
 * only: it advances the progress counter, so it shortens the "alive" proof
 * during a rejoin storm but can never subtract liveness.
 */
void moes_livenessStackActivity(void);

/*
 * Current progress counter, for the rescue stable clock's progress gate. The
 * counter is volatile and incremented by the task-context ticker plus any
 * StackActivity corroboration.
 */
u8 moes_livenessProgressCount(void);

#if defined(__cplusplus)
}
#endif
