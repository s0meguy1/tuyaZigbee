/********************************************************************************************************
 * @file    moes_fxrate.h
 *
 * @brief   Pure effect-timing maths, deliberately free of SDK, ZCL and hardware
 *          dependencies so the same code the firmware runs can be executed and
 *          checked on the host (tools/fxrate_hosttest).
 *
 * @date    2026
 *******************************************************************************************************/
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Must equal MOES_EFFECT_TICK_MS from device_config/light_ts0505b.h. This file
 * must not include the device config - staying standalone is what makes it
 * host-testable - so light_effects.c asserts at compile time that the two still
 * agree, the same arrangement moes_color.h uses for the ZCL colour constants. */
#define MOES_FX_TICK_MS         20u

/* A square wave needs at least one tick fully on and one fully off, so the
 * shortest renderable period is two ticks. Anything faster aliases into a
 * ragged, uneven flash rather than a faster strobe. */
#define MOES_FX_MIN_PERIOD_MS   (2u * MOES_FX_TICK_MS)

/* Strobe range actually reachable at the tick above, in tenths of a hertz. */
#define MOES_FX_STROBE_MIN_DHZ  10u   /*  1.0 Hz at speed 1   */
#define MOES_FX_STROBE_MAX_DHZ  120u  /* 12.0 Hz at speed 100 */

/*
 * Full-cycle period for the smooth, fading effects (rainbow, pulse, wave, …).
 *
 * `slowMs` is the cycle length at the middle speed 50, and speed scales it
 * inversely: higher speed, shorter cycle. Speed is clamped to 1..100; 0 is
 * treated as the 50 default, matching the firmware's own guard.
 */
unsigned int moes_fxPeriodMs(unsigned int slowMs, unsigned char speed);

/*
 * Full-cycle period for STROBE specifically.
 *
 * Strobe is a square wave, not a fade, so it is mapped by FREQUENCY rather than
 * by scaling a cycle length: speed 1..100 maps linearly onto
 * MOES_FX_STROBE_MIN_DHZ..MOES_FX_STROBE_MAX_DHZ. The result is never shorter
 * than MOES_FX_MIN_PERIOD_MS, so every returned period is actually renderable
 * at the tick rate.
 *
 * The previous mapping reused moes_fxPeriodMs(1000, speed), which yielded
 * 0.02 Hz at speed 1 and only 2 Hz at speed 100 - despite the code claiming
 * "2 Hz .. 20 Hz" - so even maximum speed did not look like a strobe.
 */
unsigned int moes_fxStrobePeriodMs(unsigned char speed);

/*
 * BURST STROBE (build 25) - short chaotic bursts of hard strobe, dark between.
 *
 * Built for a self-destruct-countdown look: a room mostly dark, with sparks of
 * strobe going off at unpredictable moments, like electronics breaking down.
 * Deliberately NOT periodic - a metronome reads as decorative, randomness reads
 * as failure.
 *
 * Time is divided into MOES_FX_BURST_SLOT_MS slots and each slot is
 * independently either a burst or dark, decided by hashing the slot index. The
 * decision is a pure function of (t, threshold, seed), so:
 *   - it is stable WITHIN a slot, so a burst is a coherent flash rather than
 *     per-tick flicker;
 *   - it needs no RNG state per frame, so it is host-testable and identical
 *     for a given seed;
 *   - adjacent slots occasionally chain, giving the odd longer burst for free;
 *   - the SEED decorrelates fixtures, so a room looks chaotic rather than
 *     synchronised (build 36 draws it per run; see below).
 *
 * The whole effect runs on the chip: one command starts it, one stops it, and
 * nothing is sent in between. Driving bursts from the coordinator instead would
 * be capped at roughly 2 commands/second and would drop exactly the frames that
 * exceed it.
 */
#define MOES_FX_BURST_SLOT_MS   1000u

/* Chance that any given slot is a burst, as a 0..255 threshold. Deliberately
 * low across the whole range - "mostly off" is the point. */
#define MOES_FX_BURST_MIN_THRESH  8u    /* speed 1   -> ~3% of slots */
#define MOES_FX_BURST_MAX_THRESH  90u   /* speed 100 -> ~35% of slots */

unsigned char moes_fxBurstThreshold(unsigned char speed);

/*
 * ON-time of a single flash inside a burst (build 26).
 *
 * The burst used the strobe's 50% duty, which at the full-rate 83 ms period is
 * ~41 ms of light per flash and reads as a blink. A spark wants a much shorter
 * ON and a long dark gap.
 *
 * ONE TICK IS THE FLOOR. The renderer only runs every MOES_FX_TICK_MS, so an ON
 * time is quantised to a multiple of it - asking for 10 ms would still light a
 * whole 20 ms tick, and asking for 30 ms would render as 20 or 40 depending on
 * where the flash fell. Going below 20 ms means raising the tick rate, which
 * doubles the engine's work for every effect, not just this one.
 */
#define MOES_FX_BURST_FLASH_MS  MOES_FX_TICK_MS

/*
 * EXPLOSION (build 26): ramp length for MOES_EF_EXPLODE, the golden-to-white
 * bloom. Speed picks how violent it is - 100 is a near-instant flash, 1 is a
 * slow swell. After the ramp the effect HOLDS at full white rather than
 * looping, because the scene it exists for ends with the room lit.
 */
#define MOES_FX_EXPLODE_MIN_MS  300u    /* speed 100 - a hard flash   */
#define MOES_FX_EXPLODE_MAX_MS  3000u   /* speed 1   - a slow swell   */

unsigned int moes_fxExplodeRampMs(unsigned char speed);

/*
 * Build 36: density is its own control. The threshold is a 0..255 chance per
 * slot; moes_fxBurstThreshold() derives it from speed (the build-25 behaviour,
 * kept for density 0 = auto) and moes_fxDensityThreshold() from an explicit
 * 1..100 percent of slots. The seed is per RUN, not per fixture layout: build 36
 * draws it from the radio's RNG when the effect starts, so every fixture bursts
 * on its own schedule from one group broadcast and two neighbours can never
 * flash together by construction. (Build 25 mixed the phase in for the same
 * purpose; with a real per-run seed the phase is free for the periodic
 * effects, where it now means something.)
 */
unsigned char moes_fxDensityThreshold(unsigned char densityPct);

/* Non-zero when time t falls inside a burst. Pure: same inputs, same answer. */
int moes_fxBurstActive(unsigned int t, unsigned char threshold, unsigned int seed);

/*
 * PHASE (build 36). Builds 27-35 stored the phase but never applied it to the
 * timeline (measured: two fixtures at 0 and 180 degrees pulsed in unison), so a
 * chase was impossible. A phase is now a genuine offset into the effect's own
 * period: 180 degrees on a 4 s pulse is 2 s.
 *
 * The effective phase is the fixture's own phase plus index * spread. The index
 * is set once per fixture and persisted, the spread is broadcast per show: with
 * nineteen fixtures indexed 0..18 and a spread of 19 degrees, one group
 * broadcast lays a full wheel across the house. index 0xFF (none) contributes
 * nothing.
 */
unsigned int moes_fxEffectivePhase(unsigned int phaseDeg, unsigned char index, unsigned int spreadDeg);
unsigned int moes_fxPhaseOffsetMs(unsigned int periodMs, unsigned int phaseDeg);

/*
 * LEVEL FADE (build 36). Linear ramp from one show level to another over
 * fadeMs; elapsed at or beyond fadeMs returns `to`. fadeMs 0 is an instant
 * cut. Integer only, monotonic in elapsed.
 */
unsigned char moes_fxFadeLevel(unsigned char from, unsigned char to,
                               unsigned int elapsedMs, unsigned int fadeMs);

#ifdef __cplusplus
}
#endif
