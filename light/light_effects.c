/********************************************************************************************************
 * @file    light_effects.c
 *
 * @brief   See light_effects.h.
 *
 * Rendering is all integer math: a 64-entry sine table and an integer HSV
 * mix, matching the SDK's arithmetic style. The engine writes the output
 * stage through moes_outSet() so gamma correction, white-balance trim and
 * the active-level map from the factory JSON apply to shows exactly as
 * they do to normal control.
 *
 * @date    2026
 *******************************************************************************************************/

#if (__PROJECT_TL_DIMMABLE_LIGHT__)

#include "../common/comm_cfg.h"
#include "tl_common.h"
#include "zb_api.h"
#include "zcl_include.h"
#include "tuyaLight.h"
#include "tuyaLightCtrl.h"
#include "moes_flashcfg.h"
#include "moes_fxrate.h"
#include "moes_fxwire.h"
#include "moes_color.h"
#include "moes_nvitems.h"
#include "light_effects.h"

/* moes_fxrate.h deliberately does not include the device config, so the tick it
 * assumes when bounding the strobe period is pinned to the real one here - same
 * arrangement tuyaLightCtrl.c uses for the ZCL colour constants. */
typedef char moes_fxTickMustMatch[(MOES_EFFECT_TICK_MS == MOES_FX_TICK_MS) ? 1 : -1];

moes_fx_t g_moesFx;

static ev_timer_event_t *fxTimer = NULL;

/* ------------------------------------------------------------------ */
/* Wall clock.
 *
 * Builds 24-35 advanced the timeline by MOES_EFFECT_TICK_MS per timer callback.
 * The SDK's ev_timer reloads a periodic timer only when its callback runs, so
 * every late callback (routing traffic, an NV write, a report) pushed the
 * whole timeline back by the lateness, on that fixture only. Measured on two
 * porch fixtures: a half-period start stagger had decayed to unison within
 * about five seconds. The engine now counts real system-timer ticks, with the
 * sub-millisecond remainder carried, so a late frame is simply a late frame
 * and the next one lands where it should. */
static u32 fxClkLast;   /* clock_time() at the last update */
static u32 fxClkRem;    /* ticks not yet accounted for in fxClkMs */
static u32 fxClkMs;     /* ms, advances only while the engine timer runs */

static void fxClockUpdate(void)
{
	u32 now = clock_time();
	u32 d = now - fxClkLast;   /* u32 wrap-safe as long as ticks stay under 268 s apart */
	u32 perMs = (u32)S_TIMER_CLOCK_1US * 1000u;

	fxClkLast = now;
	d += fxClkRem;
	fxClkMs += d / perMs;
	fxClkRem = d % perMs;
}

/* ------------------------------------------------------------------ */
/* Run state */
static u32  fxStartMs;     /* fxClkMs when the effect (re)started */
static u32  fxSeed;        /* per-run randomness for burst */
static bool fxBlackout;    /* takeover policy 1: an OFF landed since the start */

/* Level fade */
static bool fxFading;
static u8   fxFadeFrom;
static u8   fxFadeTo;
static u32  fxFadeStartMs;
static u16  fxFadeMs;

/* Cue list, kept in wire layout: 9 bytes per entry, 288 bytes for 32. */
static u8   fxCue[MOES_FX_CUE_MAX][MOES_FX_CUE_WIRE_LEN];
static u32  fxCueLoaded;   /* bit i set once entry i has arrived */
static u8   fxCueNext;
static u32  fxCueStartMs;

/* 64-step sine, amplitude 0..255, phase 0..63 == 0..2pi */
static const u16 fxSine[64] = {
	  0,  25,  50,  74,  98, 120, 142, 162, 180, 197, 212, 225, 235, 244, 250, 254,
	255, 254, 250, 244, 235, 225, 212, 197, 180, 162, 142, 120,  98,  74,  50,  25,
	  0,  25,  50,  74,  98, 120, 142, 162, 180, 197, 212, 225, 235, 244, 250, 254,
	255, 254, 250, 244, 235, 225, 212, 197, 180, 162, 142, 120,  98,  74,  50,  25,
};

/* scaled time: speed 1..100 maps to a period factor, faster == smaller */
static u32 fxPeriod(u32 slowMs){
	/* slowMs = full cycle at speed 50. Maths lives in moes_fxrate.c so the host
	 * test executes the same object code the firmware runs. */
	return moes_fxPeriodMs(slowMs, g_moesFx.speed);
}

/* Timeline with the fixture's phase applied: t plus the phase's share of the
 * effect's own period. Every periodic renderer goes through this, which is
 * what makes one group broadcast a chase. */
static u32 fxPhased(u32 t, u32 period){
	u32 ph = moes_fxEffectivePhase(g_moesFx.phase, g_moesFx.index, g_moesFx.spread);
	return t + moes_fxPhaseOffsetMs(period, ph);
}

static bool fxBusy(void){
	return (g_moesFx.effect != MOES_EF_STEADY) || (g_moesFx.cueRun != 0);
}

/* True when the show level is something other than "follow the fixture":
 * an explicit level, a fade towards one, or a takeover blackout. Own-colour
 * effects (fire, explode, ...) scale by the show level only then, so that
 * with nothing set they still render at full from any state, as the show
 * relies on. */
static bool fxLevelExplicit(void){
	return fxFading || (g_moesFx.level != MOES_FX_LEVEL_FOLLOW) || (g_moesFx.takeover && fxBlackout);
}

/* The level the show renders at, before the blackout gate. */
static u8 fxRawLevel(bool floorFollow){
	if(fxFading){
		return moes_fxFadeLevel(fxFadeFrom, fxFadeTo, fxClkMs - fxFadeStartMs, fxFadeMs);
	}
	if(g_moesFx.level != MOES_FX_LEVEL_FOLLOW){
		return g_moesFx.level;
	}
	{
		zcl_onOffAttr_t *pOnOff = zcl_onoffAttrGet();
		zcl_levelAttr_t *pLevel = zcl_levelAttrGet();
		u8 v = pOnOff->onOff ? pLevel->curLevel : 0;
		/* The build-25 floor for the colour-following effects, kept in follow
		 * mode only so that nothing changes for a show that sets no level. */
		if(floorFollow && v < 0x20){ v = 0x20; }
		return v;
	}
}

static u8 fxShowLevel(void){
	if(g_moesFx.takeover && fxBlackout){
		return 0;
	}
	return fxRawLevel(TRUE);
}

/* base colour for colour-following effects: the show colour if one is set,
 * else the fixture's own ZCL colour (white when it is in colour-temperature
 * mode, where the hue attribute is stale) */
static void fxCurHsv(u8 *h, u8 *s, u8 *v){
	zcl_lightColorCtrlAttr_t *pColor = zcl_colorAttrGet();

	if(g_moesFx.hue == MOES_FX_HUE_FOLLOW){
		if(pColor->colorMode == ZCL_COLOR_MODE_COLOR_TEMPERATURE_MIREDS){
			*h = 0;
			*s = 0;
		}else{
			*h = pColor->currentHue;
			*s = pColor->currentSaturation;
		}
	}else{
		*h = moes_hueDegToZcl(g_moesFx.hue);
		*s = moes_satPctToZcl(g_moesFx.sat);
	}
	*v = fxShowLevel();
}

/* candle/fire noise: sparse random jitter around a base */
static u8 fxNoise(u8 base, u8 amp, u32 t, u8 seedPhase){
	u32 bucket = t >> 7;   /* change every ~128 ms */
	u32 r = zb_random();
	/* xor-shift the bucket in so it varies over time but is stable inside a bucket */
	r ^= (bucket << 8) ^ (bucket >> 3) ^ (seedPhase << 16);
	r ^= r >> 13; r ^= r >> 7;
	u8 jitter = (r & 0xff) * amp / 255;
	return base > (255 - jitter) ? 255 : base + jitter;
}

static u8 fxScale(u8 c, u8 level){
	return (u8)(((u16)c * level) / 254u);
}

/* ------------------------------------------------------------------ */
/* renderers: fill rgb (0..255 each). White channels only where used.  */

static void fxRender(u32 t){
	u8 r = 0, g = 0, b = 0, cw = 0, ww = 0;
	u8 h, s, v;
	u32 p, tp;
	bool ownColour = FALSE;

	switch(g_moesFx.effect){
	case MOES_EF_RAINBOW:
		/* full hue wheel, ~10 s at speed 50 */
		ownColour = TRUE;
		p = fxPeriod(10000);
		tp = fxPhased(t, p);
		h = (u8)(((tp % p) * 255u) / p);
		hsvToRGB(h, 254, 254, &r, &g, &b);
		break;

	case MOES_EF_PULSE:
		/* breathe on the current colour */
		fxCurHsv(&h, &s, &v);
		p = fxPeriod(4000);
		tp = fxPhased(t, p);
		v = (u16)fxSine[(tp % p) * 64u / p] * v / 255;
		hsvToRGB(h, s, v, &r, &g, &b);
		break;

	case MOES_EF_CANDLE:
		/* warm white flicker */
		ownColour = TRUE;
		ww = fxNoise(150, 90, t, 1);
		cw = fxNoise(40, 30, t, 2);
		break;

	case MOES_EF_TWINKLE: {
		/* current colour with random sparkle bursts */
		fxCurHsv(&h, &s, &v);
		u32 bucket = t >> 6;   /* ~64 ms slots */
		u32 r0 = zb_random() ^ (bucket * 2654435761u);
		r0 ^= r0 >> 15;
		u8 spark = (r0 & 7) ? (r0 & 0x3f) : 255;
		/* (v * (64 + spark)) >> 8 reaches 316 for v=254, spark=255. Passed
		 * straight to hsvToRGB()'s u8 level it wrapped to 60, so the
		 * brightest sparkles rendered as the darkest frames. Clamp. */
		u32 sv = ((u32)v * (64u + spark)) >> 8;
		if(sv > 254){ sv = 254; }
		hsvToRGB(h, s, (u8)sv, &r, &g, &b);
		break;
	}

	case MOES_EF_FIRE:
		/* red/orange flame noise */
		ownColour = TRUE;
		r = fxNoise(200, 55, t, 3);
		g = fxNoise(70, 45, t, 4);
		b = 0;
		ww = fxNoise(30, 25, t, 5);
		break;

	case MOES_EF_STROBE:
		/* Hard flash, 1 Hz at speed 1 up to 12 Hz at speed 100. Mapped by
		 * FREQUENCY (moes_fxStrobePeriodMs) rather than by scaling a fade
		 * cycle: the old fxPeriod(1000) gave 0.02 Hz at speed 1 and only 2 Hz
		 * at speed 100, so even full speed never looked like a strobe. */
		p = moes_fxStrobePeriodMs(g_moesFx.speed);
		tp = fxPhased(t, p);
		if((tp % p) < (p >> 1)){
			fxCurHsv(&h, &s, &v);
			hsvToRGB(h, s, v, &r, &g, &b);
		}
		break;

	case MOES_EF_BURST: {
		/* Mostly dark. Inside a burst slot, strobe; outside one, leave every
		 * channel at 0 so the fixture is genuinely OFF rather than dimmed
		 * (fxCurHsv floors a followed level at 0x20, so rendering the colour
		 * dark would still glow). The burst/dark decision is a pure function of
		 * t, threshold and this run's seed - see moes_fxrate.h.
		 *
		 * Build 36: with an explicit density the strobe INSIDE a burst runs at
		 * the speed's own rate, so density and rate are independent controls.
		 * Density 0 keeps the build-25 mapping: speed picks the density and the
		 * flashes run flat out. */
		u8 thresh = g_moesFx.density ? moes_fxDensityThreshold(g_moesFx.density)
		                             : moes_fxBurstThreshold(g_moesFx.speed);
		if(moes_fxBurstActive(t, thresh, fxSeed)){
			p = moes_fxStrobePeriodMs(g_moesFx.density ? g_moesFx.speed : 100);
			/* Build 26: a fixed SHORT on-time, not the strobe's 50% duty. At the
			 * 83 ms full-rate period that was ~41 ms of light per flash, which
			 * reads as a blink; one tick reads as a spark. One tick is also the
			 * floor - see MOES_FX_BURST_FLASH_MS. */
			if((t % p) < MOES_FX_BURST_FLASH_MS){
				fxCurHsv(&h, &s, &v);
				hsvToRGB(h, s, v, &r, &g, &b);
			}
		}
		break;
	}

	case MOES_EF_EXPLODE: {
		/* Golden bloom into full white, then held. Deliberately ignores the
		 * current hue - unlike every other effect here, the colour IS the
		 * effect. */
		u32 dur = moes_fxExplodeRampMs(g_moesFx.speed);
		ownColour = TRUE;

		if(t >= dur){
			/* Hold: every channel flat out, the brightest white the fixture
			 * can make. Held rather than looped because the scene ends lit. */
			r = g = b = 255; cw = 255; ww = 255;
		}else{
			u8 k = (u8)((t * 255u) / dur);   /* 0..254 across the ramp */

			/* Hue 32/255 is about 45 degrees - amber. Saturation falls to zero
			 * as it blooms, so the colour walks gold -> white while the value
			 * rises from black, giving a fade-IN rather than a cut. */
			hsvToRGB(32, (u8)(255u - k), k, &r, &g, &b);

			ww = k;                                     /* warm white tracks the gold */
			cw = (k > 128u) ? (u8)((k - 128u) * 2u) : 0; /* cool white only late: the hard flash */
		}
		break;
	}

	case MOES_EF_SOLID:
		/* Build 36: the show colour at the show level, held. */
		fxCurHsv(&h, &s, &v);
		hsvToRGB(h, s, v, &r, &g, &b);
		break;

	case MOES_EF_WAVE:
		/* hue oscillates around the current hue */
		fxCurHsv(&h, &s, &v);
		p = fxPeriod(6000);
		tp = fxPhased(t, p);
		{
			s16 dh = ((s16)fxSine[(tp % p) * 64u / p] - 128) * 60 / 128;
			s16 hh = (s16)h + dh;
			if(hh < 0){ hh += 255; }
			if(hh > 254){ hh -= 255; }
			hsvToRGB((u8)hh, s, v, &r, &g, &b);
		}
		break;

	case MOES_EF_LIGHTNING:
		/* mostly dark, random strike sequences on cool white */
		ownColour = TRUE;
		p = fxPeriod(3000);
		tp = fxPhased(t, p);
		{
			u32 tb = tp / p;              /* which cycle */
			u32 r0 = zb_random() ^ (tb * 2246822519u);
			r0 ^= r0 >> 13;
			u32 strikeAt = (r0 % (p * 3 / 4));
			u32 dt = (tp % p);
			if(dt >= strikeAt && dt < strikeAt + 120){
				/* 2-3 sub-flashes in 120 ms */
				u8 sub = ((dt - strikeAt) / 40) & 1;
				cw = sub ? 255 : 40;
			}
		}
		break;

	case MOES_EF_CHASE:
		/* group choreography: a full-saturation wheel driven by the phased
		 * timeline. Broadcast the same command to an indexed group with a
		 * spread and the rooms chase. (Build 36: the phase used to be added to
		 * the hue here; an offset into the period is the same thing and is now
		 * how every periodic effect treats it.) */
		ownColour = TRUE;
		p = fxPeriod(8000);
		tp = fxPhased(t, p);
		hsvToRGB((u8)(((tp % p) * 255u) / p), 254, 254, &r, &g, &b);
		break;

	case MOES_EF_COLOR_STEP:
		/* discrete hue steps, ~12 per wheel */
		ownColour = TRUE;
		p = fxPeriod(6000);
		tp = fxPhased(t, p);
		h = (u8)((((tp % p) * 12u / p) * 255u / 12u) % 255u);
		hsvToRGB(h, 254, 254, &r, &g, &b);
		break;

	case MOES_EF_SNOW:
		/* cool-white shimmer */
		ownColour = TRUE;
		cw = fxNoise(120, 100, t, 6);
		ww = fxNoise(40, 40, t, 7);
		break;

	default:
		break;
	}

	/* Own-colour effects ignore the fixture's level by design (an explosion
	 * must reach full from a dark room). An EXPLICIT show level is a master
	 * fader over them too, which is what "fire dying to a cut" needs. */
	if(ownColour && fxLevelExplicit()){
		u8 L = fxShowLevel();
		r = fxScale(r, L); g = fxScale(g, L); b = fxScale(b, L);
		cw = fxScale(cw, L); ww = fxScale(ww, L);
	}

	moes_outSet(r, g, b, cw, ww);
}

/* ------------------------------------------------------------------ */
/* Cue list */

static void fxSetLevel(u8 level, u16 fadeMs)
{
	if(level == MOES_FX_LEVEL_FOLLOW || fadeMs == 0){
		fxFading = FALSE;
		g_moesFx.level = level;
		return;
	}
	/* From wherever the show is now - unfloored, so a fade-in from a dark
	 * fixture starts at black. A fade set while nothing runs simply plays from
	 * the moment an effect starts: the clock is resynchronised then. */
	fxFadeFrom = fxRawLevel(FALSE);
	fxFadeTo = level;
	fxFadeStartMs = fxClkMs;
	fxFadeMs = fadeMs;
	fxFading = TRUE;
}

static void fxCueApply(const moes_fxCue_t *c)
{
	if(c->speed){
		g_moesFx.speed = (c->speed > 100) ? 100 : c->speed;
	}
	if(c->hue != MOES_FX_KEEP16){
		g_moesFx.hue = (c->hue >= 360u) ? MOES_FX_HUE_FOLLOW : c->hue;
	}
	if(c->sat != MOES_FX_KEEP8){
		g_moesFx.sat = (c->sat > 100) ? 100 : c->sat;
	}
	if(c->level != MOES_FX_KEEP8){
		fxSetLevel(c->level, (u16)c->fade * 100u);
	}
	if(c->effect != MOES_FX_KEEP8){
		if(c->effect == MOES_EF_STEADY){
			lightFx_stop(FALSE);
		}else{
			lightFx_start(c->effect);
		}
	}
}

static void fxSequencerStep(void)
{
	if(!g_moesFx.cueRun || !g_moesFx.cueCount){
		return;
	}

	while(fxCueNext < g_moesFx.cueCount){
		moes_fxCue_t c;

		moes_fxCueDecode(fxCue[fxCueNext], &c);
		if((fxClkMs - fxCueStartMs) < c.tMs){
			return;
		}
		fxCueApply(&c);
		fxCueNext++;

		if(fxCueNext >= g_moesFx.cueCount){
			if(g_moesFx.cueRun == 2){
				/* Loop: the last entry's time is the loop length. Advance the
				 * origin by exactly that, so a loop never drifts, and restart
				 * at most once per tick so a degenerate list cannot spin. */
				fxCueStartMs += c.tMs;
				fxCueNext = 0;
				return;
			}
			g_moesFx.cueRun = 0;
			tuyaFx_stateChanged();
		}
	}
}

/* ------------------------------------------------------------------ */

static s32 fxTick(void *arg){
	(void)arg;

	fxClockUpdate();
	fxSequencerStep();

	if(fxFading && (fxClkMs - fxFadeStartMs) >= fxFadeMs){
		fxFading = FALSE;
		g_moesFx.level = fxFadeTo;
	}

	if(g_moesFx.effect != MOES_EF_STEADY){
		u32 t = fxClkMs - fxStartMs;

		if(g_moesFx.duration && t >= g_moesFx.duration){
			lightFx_stop(FALSE);
			tuyaFx_stateChanged();
		}else{
			fxRender(t);
		}
	}

	if(!fxBusy()){
		/* Only the tick ever ends the timer, and only by returning -1 here:
		 * cancelling it from inside its own callback and then returning would
		 * free the entry twice. A stop from a command therefore costs one idle
		 * tick (20 ms) before the timer goes away. */
		fxTimer = NULL;
		return -1;
	}
	return MOES_EFFECT_TICK_MS;
}

static bool fxEnsureTimer(void)
{
	if(fxTimer){
		return TRUE;
	}
	/* Starting from idle: resynchronise the clock so idle time is not counted
	 * (the delta could even have wrapped), and every relative time below
	 * starts from now. */
	fxClkLast = clock_time();
	fxClkRem = 0;

	/* TL_ZB_TIMER_SCHEDULE returns NULL when the 24-entry pool is full.
	 * This used to go unchecked and still return TRUE, so a failed
	 * allocation reported success to the coordinator and rendered a single
	 * static frame that never animated - indistinguishable from a working
	 * effect that happens to be paused. Fail honestly instead. */
	fxTimer = TL_ZB_TIMER_SCHEDULE(fxTick, NULL, MOES_EFFECT_TICK_MS);
	return fxTimer != NULL;
}

bool lightFx_start(u8 effect){
	if(effect >= MOES_EF_MAX){
		return FALSE;
	}

	if(effect == MOES_EF_STEADY){
		lightFx_stop(TRUE);
		return TRUE;
	}

	/* Build 25: starting an effect takes ownership of the output. Without this,
	 * a level/colour transition already in flight keeps stepping, and every step
	 * calls light_fresh(), which stops the effect we are starting right here -
	 * so an effect sent a second after a 3 s fade died ~100 ms later while the
	 * same effect sent after the fade finished survived. See tuyaLightCtrl.h. */
	tuyaLight_levelTransitionCancel();
	tuyaLight_colorTransitionCancel();

	if(!fxEnsureTimer()){
		g_moesFx.effect = MOES_EF_STEADY;
		return FALSE;
	}

	g_moesFx.effect = effect;
	fxStartMs = fxClkMs;
	fxSeed = ((u32)zb_random() << 16) ^ (u32)zb_random();
	fxBlackout = FALSE;

	fxRender(0);
	return TRUE;
}

void lightFx_stop(bool abortSequence){
	if(abortSequence){
		g_moesFx.cueRun = 0;
	}
	if(fxFading){
		/* State stays "what was last written": an interrupted fade lands on
		 * its target rather than reverting to the value before it. */
		fxFading = FALSE;
		g_moesFx.level = fxFadeTo;
	}
	if(g_moesFx.effect == MOES_EF_STEADY){
		return;
	}
	g_moesFx.effect = MOES_EF_STEADY;
	/* return to the ZCL attribute state (a no-op when called from inside
	 * light_fresh(), which renders that state itself) */
	light_adjust();
}

bool lightFx_active(void){
	return g_moesFx.effect != MOES_EF_STEADY;
}

bool lightFx_holdsOutput(void){
	return lightFx_active() && g_moesFx.takeover;
}

void lightFx_zclOnOff(u8 onOff){
	if(lightFx_holdsOutput()){
		fxBlackout = onOff ? FALSE : TRUE;
	}
}

static bool fxSetIndex(u8 index)
{
	if(index == g_moesFx.index){
		return TRUE;
	}
	g_moesFx.index = index;
	/* One byte, written only on change: a commissioning-time value, not a
	 * per-show one. After stack_init() by construction - this is a command
	 * handler, so the stack is up. */
	nv_flashWriteNew(1, NV_MODULE_APP, MOES_NV_ITEM_FX_INDEX, 1, &index);
	return TRUE;
}

bool lightFx_applyFrame(const moes_fxFrame_t *f)
{
	/* Parameters first, then the effect, then the sequence, so a frame that
	 * carries all three starts the effect with its parameters already in place
	 * and never renders an intermediate frame. */
	if(f->present & MOES_FXF_INDEX){    fxSetIndex(f->index); }
	if(f->present & MOES_FXF_SPREAD){   g_moesFx.spread = f->spread; }
	if(f->present & MOES_FXF_PHASE){    g_moesFx.phase = f->phase; }
	if(f->present & MOES_FXF_SPEED){    g_moesFx.speed = f->speed; }
	if(f->present & MOES_FXF_HUE){      g_moesFx.hue = f->hue; }
	if(f->present & MOES_FXF_SAT){      g_moesFx.sat = f->sat; }
	if(f->present & MOES_FXF_TAKEOVER){ g_moesFx.takeover = f->takeover; }
	if(f->present & MOES_FXF_DENSITY){  g_moesFx.density = f->density; }
	if(f->present & MOES_FXF_DURATION){ g_moesFx.duration = f->duration; }
	if(f->present & MOES_FXF_LEVEL){
		fxSetLevel(f->level, (f->present & MOES_FXF_FADE) ? f->fade : 0);
	}
	if(f->present & MOES_FXF_EFFECT){
		if(!lightFx_start(f->effect)){
			return FALSE;
		}
	}
	if(f->present & MOES_FXF_CUE_RUN){
		if(!lightFx_cueRun(f->cueRun)){
			return FALSE;
		}
	}
	return TRUE;
}

bool lightFx_cueLoad(u8 start, const u8 *wire, u8 n)
{
	u8 i;

	if(!wire || n == 0 || (u16)start + n > MOES_FX_CUE_MAX){
		return FALSE;
	}
	/* Validate everything before touching the list, so a bad entry rejects the
	 * frame whole rather than half-loading it. */
	for(i = 0; i < n; i++){
		moes_fxCue_t c;
		moes_fxCueDecode(wire + (u16)i * MOES_FX_CUE_WIRE_LEN, &c);
		if(c.effect != MOES_FX_KEEP8 && c.effect >= MOES_EF_MAX){ return FALSE; }
		if(c.speed > 100){ return FALSE; }
		if(c.sat != MOES_FX_KEEP8 && c.sat > 100){ return FALSE; }
	}

	if(start == 0){
		/* A new list. Stop a running one first: its entries are about to
		 * change under it. */
		g_moesFx.cueRun = 0;
		g_moesFx.cueCount = 0;
		fxCueLoaded = 0;
	}
	memcpy(fxCue[start], wire, (u16)n * MOES_FX_CUE_WIRE_LEN);
	for(i = 0; i < n; i++){
		fxCueLoaded |= (1u << (start + i));
	}
	if((u8)(start + n) > g_moesFx.cueCount){
		g_moesFx.cueCount = (u8)(start + n);
	}
	return TRUE;
}

static bool fxCueComplete(void)
{
	u32 need;

	if(g_moesFx.cueCount == 0){
		return FALSE;
	}
	need = (g_moesFx.cueCount >= 32) ? 0xFFFFFFFFu : ((1u << g_moesFx.cueCount) - 1u);
	return (fxCueLoaded & need) == need;
}

bool lightFx_cueRun(u8 mode)
{
	if(mode == 0){
		lightFx_stop(TRUE);
		return TRUE;
	}
	if(mode > 2 || !fxCueComplete()){
		/* An incomplete list (a lost upload frame) must not run: it would play
		 * garbage at the gap. Refusing is visible on a dataQuery. */
		return FALSE;
	}
	if(!fxEnsureTimer()){
		return FALSE;
	}
	g_moesFx.cueRun = mode;
	fxCueNext = 0;
	fxCueStartMs = fxClkMs;
	/* Entries at t = 0 land now, not one tick later. */
	fxSequencerStep();
	return TRUE;
}

void lightFx_report(moes_fxReport_t *out)
{
	out->effect = g_moesFx.effect;
	out->speed = g_moesFx.speed;
	out->phase = g_moesFx.phase;
	out->hue = g_moesFx.hue;
	out->sat = g_moesFx.sat;
	out->index = g_moesFx.index;
	out->spread = g_moesFx.spread;
	out->level = g_moesFx.level;
	out->takeover = g_moesFx.takeover;
	out->duration = g_moesFx.duration;
	out->density = g_moesFx.density;
	out->cueRun = g_moesFx.cueRun;
	out->cueCount = g_moesFx.cueCount;
}

void lightFx_init(void){
	memset((u8 *)&g_moesFx, 0, sizeof(g_moesFx));
	g_moesFx.speed = 50;
	g_moesFx.index = MOES_FX_INDEX_NONE;
	g_moesFx.hue = MOES_FX_HUE_FOLLOW;
	g_moesFx.sat = 100;
	g_moesFx.level = MOES_FX_LEVEL_FOLLOW;
	fxCueLoaded = 0;
}

void lightFx_nvLoad(void){
	u8 v = MOES_FX_INDEX_NONE;

	if(moes_nvReadByteExact(MOES_NV_ITEM_FX_INDEX, &v) == NV_SUCC){
		g_moesFx.index = v;
	}
}

#endif  /* __PROJECT_TL_DIMMABLE_LIGHT__ */
