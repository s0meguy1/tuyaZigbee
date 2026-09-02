/********************************************************************************************************
 * @file    light_effects.h
 *
 * @brief   On-device light-show engine.
 *
 * An effect takes over the 5-channel output stage and renders frames at
 * 50 fps until stopped (effect 0 returns the light to its ZCL attributes).
 * Effects are started/stopped through the Tuya manufacturer cluster
 * (cluster 0xEF00, see zcl_tuyaMfg.c and moes_fxwire.h) which zigbee2mqtt
 * drives from a small external converter, and can be broadcast to a Zigbee
 * group so the whole fleet runs one show.
 *
 * BUILD 36 - choreography moves onto the chip. Written against the field
 * notes and wish list of 2026-09-02, after the first real show was built on
 * build 35 and ran into a ~1.55 frame/s transport:
 *
 *   * The timeline runs on the system clock, not on a count of timer
 *     callbacks. A late callback no longer slows the effect, so two fixtures
 *     started together stay together and a phase offset holds.
 *   * PHASE is applied: a real offset into every periodic effect's period.
 *     Plus a persisted fixture INDEX and a broadcast SPREAD (degrees per index
 *     step), so one group frame lays a chase or a wave across the house.
 *   * A frame can carry several datapoints and a DELAY, so "in 400 ms start
 *     explode at level 254" is one atomic broadcast every member schedules
 *     for the same instant.
 *   * A CUE LIST of up to 32 timed entries runs autonomously: a 33-second
 *     show costs one trigger frame instead of thirty-six.
 *   * Show HUE/SATURATION/LEVEL live in the engine, not in the ZCL attributes.
 *     They can be staged while a fixture is dark (nothing shows until an
 *     effect runs), an explicit level renders sparks at full from an OFF
 *     fixture without a visible flare first, a level 0 is a blackout the
 *     effect survives, and STOP always returns to the fixture's own ZCL state
 *     - both stop paths now agree.
 *   * TAKEOVER policy: 0 (default) = any ZCL command takes the output back,
 *     as before; 1 = the effect persists, level/colour commands update the
 *     fixture's own state underneath it, OFF blacks it out and ON restores.
 *   * DURATION self-stops an effect; DENSITY controls burst texture on its
 *     own; a SOLID effect renders a steady show colour from the engine.
 *   * The state is readable: the chip answers a dataQuery with every value
 *     and reports after any unicast change (never after group frames, so a
 *     show does not trigger nineteen replies).
 *
 * @date    2026
 *******************************************************************************************************/

#pragma once

#include "moes_fxwire.h"

#if defined(__cplusplus)
extern "C" {
#endif

typedef enum {
	MOES_EF_STEADY = 0,
	MOES_EF_RAINBOW = 1,
	MOES_EF_PULSE = 2,
	MOES_EF_CANDLE = 3,
	MOES_EF_TWINKLE = 4,
	MOES_EF_FIRE = 5,
	MOES_EF_STROBE = 6,
	MOES_EF_WAVE = 7,
	MOES_EF_LIGHTNING = 8,
	MOES_EF_CHASE = 9,
	MOES_EF_COLOR_STEP = 10,
	MOES_EF_SNOW = 11,
	/* Build 25. Chaotic short bursts of hard strobe with darkness between, for
	 * a "sparks / electronics failing" look. Speed (or, from build 36, the
	 * density datapoint) sets how often bursts land; every fixture draws its own
	 * seed per run, so a room scatters rather than flashing in unison. Runs
	 * entirely on the chip: one command starts it, one stops it. */
	MOES_EF_BURST = 12,
	/* Build 26. One-shot bloom: golden hue swelling into full bright white, then
	 * HELD rather than looped - the explosion at the end of the countdown. Speed
	 * sets how violent the bloom is. Send "stop" to hand the output back. */
	MOES_EF_EXPLODE = 13,
	/* Build 36. A steady frame of the show colour at the show level, owned by
	 * the engine. The way to light a dark fixture in a chosen colour from one
	 * frame without touching its ZCL state, or to black it out (level 0) and
	 * bring it back. */
	MOES_EF_SOLID = 14,
	MOES_EF_MAX
} moes_effect_e;

/* Everything a show can set. Reportable; persists in RAM until changed (the
 * index persists in NV). Nothing here is reset by a stop: the state is always
 * exactly what was last written. */
typedef struct {
	u8  effect;      /* moes_effect_e, MOES_EF_STEADY when idle */
	u8  speed;       /* 1..100, 50 default */
	u16 phase;       /* 0..359, this fixture's own timeline offset */
	u16 spread;      /* 0..359, degrees of extra phase per index step */
	u8  index;       /* 0..254, MOES_FX_INDEX_NONE when unassigned */
	u16 hue;         /* 0..359 or MOES_FX_HUE_FOLLOW */
	u8  sat;         /* 0..100 */
	u8  level;       /* 0..254 or MOES_FX_LEVEL_FOLLOW */
	u8  takeover;    /* 0 = a ZCL command stops the effect, 1 = it persists */
	u8  density;     /* 0 = from speed, else 1..100 percent of burst slots */
	u16 duration;    /* ms an effect runs before stopping itself, 0 = forever */
	u8  cueRun;      /* 0 idle, 1 running once, 2 looping */
	u8  cueCount;    /* entries loaded (contiguous from 0) */
} moes_fx_t;

extern moes_fx_t g_moesFx;

/* Before stack_init(): a plain memset, no stack or NV dependency. */
void lightFx_init(void);

/* After stack_init() (nv_init() lives inside it): restore the fixture index. */
void lightFx_nvLoad(void);

/* Start an effect with the current parameters; the same effect restarts from
 * its first frame. MOES_EF_STEADY stops everything, including a cue list.
 * Returns FALSE for an unknown effect or when no timer could be allocated. */
bool lightFx_start(u8 effect);

/* Hand the output back to the ZCL state. A running cue list carries on unless
 * abortSequence is set. */
void lightFx_stop(bool abortSequence);

/* True while an effect owns the output stage. */
bool lightFx_active(void);

/* True while an effect must survive a ZCL command (takeover policy 1). */
bool lightFx_holdsOutput(void);

/* Called by the on/off handler before it renders: under takeover policy 1 an
 * OFF blacks the effect out and an ON brings it back. */
void lightFx_zclOnOff(u8 onOff);

/* Apply a parsed datapoint frame now (delay is the caller's business). */
bool lightFx_applyFrame(const moes_fxFrame_t *f);

/* Cue list. Entries arrive in wire form (moes_fxwire.h), n at a time from
 * `start`; start 0 begins a new list. A list runs only once every entry from 0
 * to the highest loaded has arrived. */
bool lightFx_cueLoad(u8 start, const u8 *wire, u8 n);
bool lightFx_cueRun(u8 mode);

/* Fill a report with the current state. */
void lightFx_report(moes_fxReport_t *out);

/* Implemented by zcl_tuyaMfg.c. The engine changed state on its own (a
 * duration ran out, a cue list finished) - report it if anyone is listening. */
void tuyaFx_stateChanged(void);

#if defined(__cplusplus)
}
#endif
