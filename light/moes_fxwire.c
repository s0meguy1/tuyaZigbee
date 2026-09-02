/********************************************************************************************************
 * @file    moes_fxwire.c
 *
 * @brief   See moes_fxwire.h. No SDK, no ZCL, no hardware: tools/fxwire_hosttest
 *          compiles and runs THIS file.
 *
 * @date    2026
 *******************************************************************************************************/
#include "moes_fxwire.h"

#define MOES_FXW_HDR_LEN   2u   /* seq */
#define MOES_FXW_DP_HDR    4u   /* dp, type, len16 */

static unsigned int moes_fxWireFold(const unsigned char *p, unsigned int n)
{
	unsigned int v = 0;
	unsigned int i;

	/* Big-endian, at most four bytes - the same fold builds 27-35 used. A
	 * longer "value" is not something any known sender emits; its high bytes
	 * are ignored rather than wrapped. */
	if(n > 4u){
		p += n - 4u;
		n = 4u;
	}
	for(i = 0; i < n; i++){
		v = (v << 8) | p[i];
	}
	return v;
}

moes_fxWireStatus_e moes_fxWireParse(const unsigned char *payload, unsigned int len,
                                     unsigned char effectMax, moes_fxFrame_t *out,
                                     moes_fxCueLoadFn cueFn, void *cueCtx)
{
	unsigned int off = MOES_FXW_HDR_LEN;
	moes_fxFrame_t f;
	unsigned int dps = 0;
	unsigned int loads = 0;

	if(!payload || !out){
		return MOES_FXW_MALFORMED;
	}

	/* The seq plus at least one complete datapoint header. */
	if(len < MOES_FXW_HDR_LEN + MOES_FXW_DP_HDR){
		return MOES_FXW_MALFORMED;
	}

	f.present = 0;
	f.effect = 0; f.speed = 0; f.phase = 0; f.spread = 0; f.index = 0;
	f.hue = 0; f.sat = 0; f.level = 0; f.fade = 0; f.delay = 0;
	f.duration = 0; f.takeover = 0; f.density = 0; f.cueRun = 0;

	while(off < len){
		unsigned char dp;
		unsigned char type;
		unsigned int vlen;
		const unsigned char *val;
		unsigned int v;

		if(off + MOES_FXW_DP_HDR > len){
			return MOES_FXW_MALFORMED;
		}
		dp = payload[off];
		type = payload[off + 1];
		vlen = ((unsigned int)payload[off + 2] << 8) | payload[off + 3];
		/* Compared in unsigned int, never after a 16-bit truncation: the
		 * length is attacker-controlled and 0xFFFF + 6 used to wrap. */
		if(off + MOES_FXW_DP_HDR + vlen > len){
			return MOES_FXW_MALFORMED;
		}
		val = payload + off + MOES_FXW_DP_HDR;
		off += MOES_FXW_DP_HDR + vlen;
		(void)type;

		if(dp == MOES_DP_CUE_LOAD){
			unsigned char n;
			if(vlen < 1u + MOES_FX_CUE_WIRE_LEN || ((vlen - 1u) % MOES_FX_CUE_WIRE_LEN) != 0u){
				return MOES_FXW_INVALID;
			}
			n = (unsigned char)((vlen - 1u) / MOES_FX_CUE_WIRE_LEN);
			if((unsigned int)val[0] + n > MOES_FX_CUE_MAX){
				return MOES_FXW_INVALID;
			}
			if(!cueFn || !cueFn(cueCtx, val[0], val + 1, n)){
				return MOES_FXW_INVALID;
			}
			loads++;
			continue;
		}

		if(vlen == 0){
			return MOES_FXW_MALFORMED;
		}
		v = moes_fxWireFold(val, vlen);

		switch(dp){
		case MOES_DP_EFFECT:
			if(v >= effectMax){ return MOES_FXW_INVALID; }
			f.effect = (unsigned char)v;
			f.present |= MOES_FXF_EFFECT;
			break;
		case MOES_DP_SPEED:
			if(v < 1u || v > 100u){ return MOES_FXW_INVALID; }
			f.speed = (unsigned char)v;
			f.present |= MOES_FXF_SPEED;
			break;
		case MOES_DP_PHASE:
			if(v > 359u){ return MOES_FXW_INVALID; }
			f.phase = (unsigned short)v;
			f.present |= MOES_FXF_PHASE;
			break;
		case MOES_DP_HUE:
			f.hue = (v >= 360u) ? (unsigned short)MOES_FX_HUE_FOLLOW : (unsigned short)v;
			f.present |= MOES_FXF_HUE;
			break;
		case MOES_DP_SAT:
			if(v > 100u){ return MOES_FXW_INVALID; }
			f.sat = (unsigned char)v;
			f.present |= MOES_FXF_SAT;
			break;
		case MOES_DP_INDEX:
			if(v > 255u){ return MOES_FXW_INVALID; }
			f.index = (unsigned char)v;   /* 255 = none */
			f.present |= MOES_FXF_INDEX;
			break;
		case MOES_DP_SPREAD:
			if(v > 359u){ return MOES_FXW_INVALID; }
			f.spread = (unsigned short)v;
			f.present |= MOES_FXF_SPREAD;
			break;
		case MOES_DP_LEVEL:
			if(v > 255u){ return MOES_FXW_INVALID; }
			f.level = (unsigned char)v;   /* 255 = follow */
			f.present |= MOES_FXF_LEVEL;
			break;
		case MOES_DP_FADE:
			if(v > 0xFFFFu){ return MOES_FXW_INVALID; }
			f.fade = (unsigned short)v;
			f.present |= MOES_FXF_FADE;
			break;
		case MOES_DP_DELAY:
			if(v > 0xFFFFu){ return MOES_FXW_INVALID; }
			f.delay = (unsigned short)v;
			f.present |= MOES_FXF_DELAY;
			break;
		case MOES_DP_DURATION:
			if(v > 0xFFFFu){ return MOES_FXW_INVALID; }
			f.duration = (unsigned short)v;
			f.present |= MOES_FXF_DURATION;
			break;
		case MOES_DP_TAKEOVER:
			if(v > 1u){ return MOES_FXW_INVALID; }
			f.takeover = (unsigned char)v;
			f.present |= MOES_FXF_TAKEOVER;
			break;
		case MOES_DP_DENSITY:
			if(v > 100u){ return MOES_FXW_INVALID; }
			f.density = (unsigned char)v;
			f.present |= MOES_FXF_DENSITY;
			break;
		case MOES_DP_CUE_RUN:
			if(v > 2u){ return MOES_FXW_INVALID; }
			f.cueRun = (unsigned char)v;
			f.present |= MOES_FXF_CUE_RUN;
			break;
		default:
			return MOES_FXW_INVALID;
		}
		dps++;
	}

	*out = f;

	if(dps == 0){
		return loads ? MOES_FXW_EMPTY : MOES_FXW_MALFORMED;
	}
	return MOES_FXW_OK;
}

void moes_fxCueDecode(const unsigned char *w, moes_fxCue_t *out)
{
	out->tMs    = (unsigned short)(((unsigned short)w[0] << 8) | w[1]);
	out->effect = w[2];
	out->speed  = w[3];
	out->hue    = (unsigned short)(((unsigned short)w[4] << 8) | w[5]);
	out->sat    = w[6];
	out->level  = w[7];
	out->fade   = w[8];
}

/* ---- report encoder ---- */

static unsigned int moes_fxWirePut(unsigned char *buf, unsigned int cap, unsigned int off,
                                   unsigned char dp, unsigned char type,
                                   unsigned int value, unsigned char width)
{
	unsigned int i;

	if(off + MOES_FXW_DP_HDR + width > cap){
		return 0;
	}
	buf[off++] = dp;
	buf[off++] = type;
	buf[off++] = 0;
	buf[off++] = width;
	for(i = 0; i < width; i++){
		buf[off++] = (unsigned char)(value >> (8u * (width - 1u - i)));
	}
	return off;
}

unsigned int moes_fxWireReportBuild(unsigned char *buf, unsigned int cap,
                                    unsigned short seq, const moes_fxReport_t *st)
{
	unsigned int off = 0;
	unsigned int hue = (st->hue == MOES_FX_HUE_FOLLOW) ? MOES_FX_HUE_WIRE_FOLLOW : st->hue;

	if(!buf || !st || cap < MOES_FXW_HDR_LEN){
		return 0;
	}
	buf[off++] = (unsigned char)(seq >> 8);
	buf[off++] = (unsigned char)seq;

	/* Each line is a datapoint. A zero return from the helper is "did not
	 * fit"; propagate it rather than emit a truncated report. */
#define MOES_FXW_EMIT(dp, type, v, w) \
	do { off = moes_fxWirePut(buf, cap, off, (dp), (type), (v), (w)); if(!off){ return 0; } } while(0)

	MOES_FXW_EMIT(MOES_DP_EFFECT,   MOES_DPT_ENUM,  st->effect,   1);
	MOES_FXW_EMIT(MOES_DP_SPEED,    MOES_DPT_VALUE, st->speed,    1);
	MOES_FXW_EMIT(MOES_DP_PHASE,    MOES_DPT_VALUE, st->phase,    2);
	MOES_FXW_EMIT(MOES_DP_HUE,      MOES_DPT_VALUE, hue,          2);
	MOES_FXW_EMIT(MOES_DP_SAT,      MOES_DPT_VALUE, st->sat,      1);
	MOES_FXW_EMIT(MOES_DP_INDEX,    MOES_DPT_VALUE, st->index,    1);
	MOES_FXW_EMIT(MOES_DP_SPREAD,   MOES_DPT_VALUE, st->spread,   2);
	MOES_FXW_EMIT(MOES_DP_LEVEL,    MOES_DPT_VALUE, st->level,    1);
	MOES_FXW_EMIT(MOES_DP_TAKEOVER, MOES_DPT_ENUM,  st->takeover, 1);
	MOES_FXW_EMIT(MOES_DP_DURATION, MOES_DPT_VALUE, st->duration, 2);
	MOES_FXW_EMIT(MOES_DP_DENSITY,  MOES_DPT_VALUE, st->density,  1);
	MOES_FXW_EMIT(MOES_DP_CUE_RUN,  MOES_DPT_ENUM,  st->cueRun,   1);
	MOES_FXW_EMIT(MOES_DP_CUE_COUNT, MOES_DPT_VALUE, st->cueCount, 1);
#undef MOES_FXW_EMIT

	return off;
}
