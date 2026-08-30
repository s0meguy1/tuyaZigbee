/********************************************************************************************************
 * @file    moes_rescue.h
 *
 * @brief   Boot-storm probation: an ADVISORY health flag, never a behaviour
 *          change. Make this firmware unable to lock itself out of OTA
 *          without ever stopping being a light.
 *
 * The risk model for this device (see POSTMORTEM_2026-08-14.md): a bug that
 * stops the light staying up long enough to receive an OTA is unrecoverable
 * without physical access. Every other class of bug is cosmetic by
 * comparison.
 *
 * BUILD 18 REDESIGN. Through build 17, a tripped probation counter latched a
 * separate "rescue mode": dim fixed white output, every command ignored,
 * application init skipped, cleared only by 20 joined minutes plus another
 * power cycle. On 2026-08-29 that held a healthy converted fixture hostage
 * for half an hour, and the design is untenable for a house: routine power
 * events must never cost responsiveness, and a state-restore automation
 * needs the light to answer the moment mains returns. The new contract:
 *
 *   - A light ALWAYS boots fully functional and answers commands from the
 *     first second, probation or not. Nothing in this module may gate
 *     output, clusters, attributes, or init.
 *
 *   - Probation counts RAPID-BOOT STORMS only. Every boot writes a "young"
 *     marker; a 60 s timer (moes_rescueGrownupCb) clears it. A boot that
 *     inherits a still-young predecessor is part of a storm (any
 *     watchdog-bounded loop dies inside its boot interval) and increments
 *     probation. A boot whose predecessor grew up - a routine power-on, an
 *     outage restore, a breaker cycle - wipes the history instead. The
 *     2 s-windowed factory-reset gesture therefore can never trip it
 *     either (three quick gesture boots build at most 3 of 6).
 *
 *   - When the counter reaches MOES_RESCUE_FAIL_THRESHOLD the flag is set
 *     and does exactly two things, both advisory: the OTA query interval
 *     drops to MOES_RESCUE_OTA_QUERY_SECONDS (a storming light phones home
 *     every cycle - that is the entire recovery story), and the join blink
 *     runs 5 pulses instead of 2 so a human can tell. No other behaviour
 *     exists to change.
 *
 *   - The stable clock (joined + scheduler progress for
 *     MOES_RESCUE_STABLE_MINUTES, with the build-09 confirmation minute)
 *     still clears the counter during long healthy runs, and the
 *     3-power-cycle factory-reset gesture still clears it on completion.
 *
 * Flash wear is bounded: healthy boots write nothing here; a storming light
 * stops writing probation once latched and writes the one-byte young marker
 * per boot attempt.
 *
 * The mechanism is fail-safe in the direction that matters: any failure to
 * read or write the counters, and any garbage value, ends in an advisory
 * flag or a clean boot - never in a state where the light stops being a
 * light or OTA is unavailable.
 *
 * See FALLBACK_DESIGN.md for the original design and its history.
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

/* Rapid-boot-storm boots before the advisory flag latches.
 *
 * Comfortably above the 3-power-cycle pairing gesture, whose quick boots
 * build at most 3 (and whose completed gesture calls moes_rescueClear()
 * anyway). A watchdog-bounded reset loop reaches six in a few minutes. */
#ifndef MOES_RESCUE_FAIL_THRESHOLD
#define MOES_RESCUE_FAIL_THRESHOLD      6
#endif

/* How long the light must stay joined (and making scheduler progress)
 * before the stable clock clears the probation count. Purely advisory now:
 * a routine power-on after this much runtime already wiped the history at
 * its own boot, so this only shortens the flag's tail on a light that
 * recovered mid-storm without a power event. */
#ifndef MOES_RESCUE_STABLE_MINUTES
#define MOES_RESCUE_STABLE_MINUTES      20
#endif

/* How often a storm-flagged light asks the coordinator for an image.
 * Normal operation uses MY_OTA_PERIODIC_QUERY_INTERVAL (6 hours); a light
 * that keeps dying should ask far more often than that. */
#ifndef MOES_RESCUE_OTA_QUERY_SECONDS
#define MOES_RESCUE_OTA_QUERY_SECONDS   (10 * 60U)
#endif

/* Define MOES_RESCUE_FORCE at build time to set the advisory flag
 * unconditionally - the early OTA cadence plus the 5-pulse join blink, with
 * no behavioural difference to test around. This is how the flag path gets
 * soak-tested on a bench unit. Never ship it. */

/*
 * Decide this boot's storm status. MUST be called after stack_init()
 * returns and before anything else touches NV, schedules a timer, or
 * drives the output.
 */
void moes_rescueBootCheck(void);

/* TRUE if this device recently storm-counted. ADVISORY ONLY (build 18):
 * gates the OTA query cadence and the join-blink pattern, nothing else. */
bool moes_rescueActive(void);

/* One-shot: clears the boot "young" marker. Scheduled by
 * moes_rescueBootCheck(); exposed for the hosttest shim. */
s32 moes_rescueGrownupCb(void *arg);

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

/* Current storm-boot count, for diagnostics. */
u8 moes_rescueFailCount(void);

#if defined(__cplusplus)
}
#endif
