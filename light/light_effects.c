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
#include "zcl_include.h"
#include "tuyaLight.h"
#include "tuyaLightCtrl.h"
#include "moes_flashcfg.h"
#include "light_effects.h"
#include "moes_rescue.h"

moes_fx_t g_moesFx;

static ev_timer_event_t *fxTimer = NULL;

/* 64-step sine, amplitude 0..255, phase 0..63 == 0..2pi */
static const u16 fxSine[64] = {
	  0,  25,  50,  74,  98, 120, 142, 162, 180, 197, 212, 225, 235, 244, 250, 254,
	255, 254, 250, 244, 235, 225, 212, 197, 180, 162, 142, 120,  98,  74,  50,  25,
	  0,  25,  50,  74,  98, 120, 142, 162, 180, 197, 212, 225, 235, 244, 250, 254,
	255, 254, 250, 244, 235, 225, 212, 197, 180, 162, 142, 120,  98,  74,  50,  25,
};

/* scaled time: speed 1..100 maps to a period factor, faster == smaller */
static u32 fxPeriod(u32 slowMs){
	/* slowMs = full cycle at speed 50 */
	u32 sp = g_moesFx.speed ? g_moesFx.speed : 50;
	u32 p = (slowMs * 100) / (2 * sp);
	return p ? p : 1;
}

/* base colour for colour-following effects: current ZCL hue/sat */
static void fxCurHsv(u8 *h, u8 *s, u8 *v){
	zcl_lightColorCtrlAttr_t *pColor = zcl_colorAttrGet();
	zcl_onOffAttr_t *pOnOff = zcl_onoffAttrGet();
	zcl_levelAttr_t *pLevel = zcl_levelAttrGet();
	*h = pColor->currentHue;
	*s = pColor->currentSaturation;
	*v = pOnOff->onOff ? pLevel->curLevel : 0;
	if(*v < 0x20){ *v = 0x20; }
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

/* ------------------------------------------------------------------ */
/* renderers: fill rgb (0..255 each). White channels only where used.  */

static void fxRender(u32 t){
	u8 r = 0, g = 0, b = 0, cw = 0, ww = 0;
	u8 h, s, v;
	u32 p;

	switch(g_moesFx.effect){
	case MOES_EF_RAINBOW:
		/* full hue wheel, ~10 s at speed 50 */
		p = fxPeriod(10000);
		h = (u8)(((t % p) * 255u) / p);
		hsvToRGB(h, 254, 254, &r, &g, &b);
		break;

	case MOES_EF_PULSE:
		/* breathe on the current colour */
		fxCurHsv(&h, &s, &v);
		p = fxPeriod(4000);
		v = (u16)fxSine[(t % p) * 64u / p] * v / 255;
		hsvToRGB(h, s, v ? v : 1, &r, &g, &b);
		break;

	case MOES_EF_CANDLE:
		/* warm white flicker */
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
		r = fxNoise(200, 55, t, 3);
		g = fxNoise(70, 45, t, 4);
		b = 0;
		ww = fxNoise(30, 25, t, 5);
		break;

	case MOES_EF_STROBE:
		/* hard flash at speed: 2 Hz .. 20 Hz */
		p = fxPeriod(1000);
		if((t % p) < (p >> 1)){
			fxCurHsv(&h, &s, &v);
			hsvToRGB(h, s, v, &r, &g, &b);
		}
		break;

	case MOES_EF_WAVE:
		/* hue oscillates around the current hue */
		fxCurHsv(&h, &s, &v);
		p = fxPeriod(6000);
		{
			s16 dh = ((s16)fxSine[(t % p) * 64u / p] - 128) * 60 / 128;
			s16 hh = (s16)h + dh;
			if(hh < 0){ hh += 255; }
			if(hh > 254){ hh -= 255; }
			hsvToRGB((u8)hh, s, v, &r, &g, &b);
		}
		break;

	case MOES_EF_LIGHTNING:
		/* mostly dark, random strike sequences on cool white */
		p = fxPeriod(3000);
		{
			u32 tb = t / p;              /* which cycle */
			u32 r0 = zb_random() ^ (tb * 2246822519u);
			r0 ^= r0 >> 13;
			u32 strikeAt = (r0 % (p * 3 / 4));
			u32 dt = (t % p);
			if(dt >= strikeAt && dt < strikeAt + 120){
				/* 2-3 sub-flashes in 120 ms */
				u8 sub = ((dt - strikeAt) / 40) & 1;
				cw = sub ? 255 : 40;
			}
		}
		break;

	case MOES_EF_CHASE:
		/* group choreography: hue from the phase offset, wheel driven by t.
		 * Broadcast the same command to the group, give each light a
		 * different phase, and the rooms chase. */
		{
			p = fxPeriod(8000);
			u16 deg = (g_moesFx.phase * 255u / 359 + ((t % p) * 255u) / p) % 255;
			hsvToRGB((u8)deg, 254, 254, &r, &g, &b);
		}
		break;

	case MOES_EF_COLOR_STEP:
		/* discrete hue steps, ~12 per wheel */
		p = fxPeriod(6000);
		/* The (u8) cast used to bind tighter than the '%', so the sum (up to
		 * 488) was truncated to 8 bits *before* the modulo and the phase
		 * offset folded the wheel back on itself. Reduce first, cast last -
		 * same shape as MOES_EF_CHASE just below. */
		h = (u8)(((((t % p) * 12u / p) * 255u / 12u) + (g_moesFx.phase * 255u / 359u)) % 255u);
		hsvToRGB(h, 254, 254, &r, &g, &b);
		break;

	case MOES_EF_SNOW:
		/* cool-white shimmer */
		cw = fxNoise(120, 100, t, 6);
		ww = fxNoise(40, 40, t, 7);
		break;

	default:
		break;
	}

	moes_outSet(r, g, b, cw, ww);
}

static s32 fxTick(void *arg){
	(void)arg;
	g_moesFx.t += MOES_EFFECT_TICK_MS;
	fxRender(g_moesFx.t);
	return MOES_EFFECT_TICK_MS;
}

/* ------------------------------------------------------------------ */
bool lightFx_start(u8 effect, u8 speed, u16 phase){
	if(effect >= MOES_EF_MAX){
		return FALSE;
	}

#if MOES_TS0505B
	/* A light that has failed to reach a stable state MOES_RESCUE_FAIL_THRESHOLD
	 * times running does not get a 25 fps timer. The engine is off by default
	 * anyway, but "off by default" is not the same as "cannot be turned on by
	 * a broadcast to a group while the light is trying to take an OTA". */
	if(moes_rescueActive() && effect != MOES_EF_STEADY){
		return FALSE;
	}
#endif

	if(effect == MOES_EF_STEADY){
		if(fxTimer){
			TL_ZB_TIMER_CANCEL(&fxTimer);
			fxTimer = NULL;
		}
		g_moesFx.effect = MOES_EF_STEADY;
		/* return to the ZCL attribute state */
		light_adjust();
		return TRUE;
	}

	g_moesFx.effect = effect;
	g_moesFx.speed = speed ? speed : 50;
	g_moesFx.phase = phase % 360;
	g_moesFx.t = 0;

	if(!fxTimer){
		fxTimer = TL_ZB_TIMER_SCHEDULE(fxTick, NULL, MOES_EFFECT_TICK_MS);
	}
	fxRender(0);
	return TRUE;
}

bool lightFx_active(void){
	return g_moesFx.effect != MOES_EF_STEADY;
}

void lightFx_init(void){
	memset((u8 *)&g_moesFx, 0, sizeof(g_moesFx));
	g_moesFx.speed = 50;
}

#endif  /* __PROJECT_TL_DIMMABLE_LIGHT__ */
