/********************************************************************************************************
 * @file    moes_rescue.h
 *
 * @brief   Boot probation and rescue mode: make this firmware unable to lock
 *          itself out of OTA.
 *
 * The risk model for this device (see POSTMORTEM_2026-08-14.md): a bug that
 * stops the light staying up long enough to receive an OTA is unrecoverable
 * without physical access, because SWire writes are broken on this silicon
 * and the stock bootloader's UART path does not answer. Every other class of
 * bug is cosmetic by comparison.
 *
 * The mechanism:
 *
 *   - One byte in our own NV module counts *consecutive boots that never
 *     reached a stable state*. It is incremented once per boot, right after
 *     stack_init() returns (nv_init() lives inside zb_init(), so anything
 *     earlier is operating on an uninitialised NV subsystem - that ordering
 *     mistake is what cost us a fixture).
 *
 *   - It is cleared when the light has been *joined* continuously for
 *     MOES_RESCUE_STABLE_MINUTES, which is set longer than an OTA takes. A
 *     firmware that can hold the network that long is by definition
 *     recoverable, so the counter goes back to zero.
 *
 *     Build 09: "joined continuously" is now "joined AND the scheduler is
 *     still making progress", with a one-minute confirmation window after the
 *     countdown reaches zero. A joined-but-wedged stack (boothang_stack.md)
 *     leaves zb_isDeviceJoinedNwk() set while the cooperative scheduler has
 *     stalled; the clear must not commit on such a boot, or the probation
 *     history that a wedge needs is erased. The effective healthy window is
 *     therefore MOES_RESCUE_STABLE_MINUTES + 1 minutes.
 *
 *   - It is also cleared by a completed 3-power-cycle factory-reset gesture,
 *     so a human deliberately re-pairing a light can never drive it into
 *     rescue mode (see MOES_RESCUE_FAIL_THRESHOLD below).
 *
 *   - Once the counter reaches MOES_RESCUE_FAIL_THRESHOLD the light latches
 *     rescue mode: it joins the network, services the OTA cluster, and does
 *     nothing else. No effect engine, no power-cycle counter, no attribute
 *     persistence, no manufacturer-cluster commands, no light_adjust().
 *
 * Flash wear is bounded by construction. A light in a reset loop writes the
 * counter at most MOES_RESCUE_FAIL_THRESHOLD times and then *stops* - it does
 * not write on a boot that latches rescue mode. Rescue mode additionally
 * skips factoryRst_init() (two NV writes per boot) and the attribute-store
 * timer, so a latched light in a permanent loop does zero NV writes.
 *
 * The mechanism is fail-safe in the direction that matters: any failure to
 * read or write the counter, and any garbage value, ends in rescue mode or a
 * normal boot - never in a state where OTA is unavailable.
 *
 * See FALLBACK_DESIGN.md for the full design, failure modes and test plan.
 *
 * @date    2026
 *******************************************************************************************************/

#pragma once

#if defined(__cplusplus)
extern "C" {
#endif

/* Master switch. Turning this off restores the pre-rescue behaviour exactly. */
#ifndef MOES_RESCUE_ENABLE
#define MOES_RESCUE_ENABLE              1
#endif

/* Consecutive unstable boots before the light gives up and comes up minimal.
 *
 * Must stay comfortably above the 3-power-cycle pairing gesture (rstnum:3),
 * because a user performing that gesture does produce three quick boots. Six
 * gives 2x margin, and factoryRst_handler() clears the counter when the
 * gesture completes, so the gesture can never latch rescue mode on its own.
 *
 * A genuine reset loop at ~10 s/cycle reaches six in about a minute. */
#ifndef MOES_RESCUE_FAIL_THRESHOLD
#define MOES_RESCUE_FAIL_THRESHOLD      6
#endif

/* How long the light must stay joined before it is declared healthy.
 *
 * This is deliberately longer than an OTA download (~30 min is the observed
 * worst case, but that includes stalls; 20 min of *continuous* uptime is
 * enough to start and mostly finish one, and a light that can do that can be
 * retried). Setting it shorter would let a firmware that faults after, say,
 * 12 minutes clear its counter on every boot and never trip rescue mode. */
#ifndef MOES_RESCUE_STABLE_MINUTES
#define MOES_RESCUE_STABLE_MINUTES      20
#endif

/* How often a light in rescue mode asks the coordinator for an image.
 * Normal operation uses MY_OTA_PERIODIC_QUERY_INTERVAL (6 hours); a light
 * that has told us it cannot run should be asking far more often than that. */
#ifndef MOES_RESCUE_OTA_QUERY_SECONDS
#define MOES_RESCUE_OTA_QUERY_SECONDS   (10 * 60U)
#endif

/* Define MOES_RESCUE_FORCE at build time to come up in rescue mode
 * unconditionally. This is how rescue mode gets soak-tested on a bench unit
 * without having to write a firmware that actually crashes. Never ship it. */

/*
 * Decide this boot's mode. MUST be called after stack_init() returns and
 * before anything else touches NV, schedules a timer, or drives the output.
 */
void moes_rescueBootCheck(void);

/* TRUE if this boot is running minimal/rescue. Safe to call at any time,
 * including before moes_rescueBootCheck() (returns FALSE). */
bool moes_rescueActive(void);

/*
 * Start (or leave running) the "has been joined and making progress long
 * enough" clock. Called from the BDB callbacks whenever the light reports it
 * is on a network. Cheap and idempotent.
 */
void moes_rescueStableTimerStart(void);

/*
 * Declare this light healthy now and zero the counter. Called by the stable
 * clock, and by factoryRst_handler() when the user completes the
 * 3-power-cycle gesture.
 */
void moes_rescueClear(void);

/* Current consecutive-unstable-boot count, for diagnostics. */
u8 moes_rescueFailCount(void);

#if defined(__cplusplus)
}
#endif
