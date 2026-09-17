/********************************************************************************************************
 * @file    moes_dim.c
 *
 * @brief   See moes_dim.h. No SDK, ZCL or hardware dependencies - that is what
 *          makes tools/level_curve_hosttest able to execute the real firmware
 *          arithmetic instead of a copy of it.
 *
 *          Build 40: the curve deliberately keeps brightmin's 1% floor for a
 *          lit channel and does NOT ramp it out below one level. That floor is
 *          stock's own (factory block brightmin:1) and stock ran this hardware
 *          for nine months without the fade artefact. Going below it means
 *          sub-2.5 us on-times at 4 kHz, which the driver was observed not to
 *          hold cleanly.
 *
 * @date    2026
 *******************************************************************************************************/
#include "moes_dim.h"

/* 'unsigned int' rather than u32/unsigned long on purpose: it is 32 bits on
 * both the tc32 target and the host, so the host test exercises the same
 * overflow and truncation behaviour the firmware has. The widest intermediate
 * below is v256 * pwmMaxTick = 65280 * 65535, which is 4.28e9 and still fits. */

unsigned int moes_dimCurve256(unsigned int v256)
{
	if(v256 == 0u){
		return 0u;
	}

	if(v256 > MOES_DIM_MAX256){
		v256 = MOES_DIM_MAX256;
	}

	return (((unsigned int)MOES_BRIGHT_MIN_PCT * MOES_DIM_MAX256) +
	        ((unsigned int)(MOES_BRIGHT_MAX_PCT - MOES_BRIGHT_MIN_PCT) * v256)) /
	       (unsigned int)MOES_BRIGHT_MAX_PCT;
}

/* Build 42. The extinction tail.
 *
 * Measured with calibrated photometry on the bench fixture (2026-09-16,
 * MOES/bench_photometry/RESULTS_2026-09-16.md): light is linear in duty from
 * level 1 down to the brightmin floor, but the driver saturates above roughly a
 * third of the duty range, so the 1% floor is 5.7% of FULL light - 38% of the
 * perceived range. A with-on-off fade to off paces its 8.8 level to zero, the
 * curve above floors every non-zero channel at 1%, and the light therefore sits
 * on that plateau for 1-2 s and then cuts. That plateau-and-cut is the cliff.
 *
 * Build 41 ramped the floor out inside moes_dimCurve256() for channel values
 * below 256. Wrong place: the colour-temperature split hands the warm channel
 * less than one level's worth for ZCL levels 1-4, so steady levels 1-4 dimmed
 * (level 1 by 37%, measured). This scales the LEVEL-1 rendering instead, by the
 * sub-level fraction, and is applied by moes_outSet256() after the curve. No
 * steady state can ask for level256 < 256 (ZCL level 1 is the minimum), so
 * every whole level renders byte-for-byte as build 40 did.
 *
 * Build 41's fade recordings showed the driver following the tail
 * monotonically down to under 0.1% duty with no plateau, so the region below
 * the floor that build 40 avoided is fine on this hardware. */
unsigned int moes_dimTail256(unsigned int curve256, unsigned int level256)
{
	if(level256 >= 256u){
		return curve256;
	}
	return (curve256 * level256) / 256u;
}

unsigned int moes_dimInvert256(unsigned int v256, unsigned char activeLow)
{
	if(v256 > MOES_DIM_MAX256){
		v256 = MOES_DIM_MAX256;
	}

	return activeLow ? (MOES_DIM_MAX256 - v256) : v256;
}

unsigned int moes_dimCmpTick(unsigned int v256, unsigned int pwmMaxTick)
{
	if(v256 > MOES_DIM_MAX256){
		v256 = MOES_DIM_MAX256;
	}

	return (v256 * pwmMaxTick) / MOES_DIM_MAX256;
}

void moes_dimSplitCW256(unsigned short mireds, unsigned short minMireds, unsigned short maxMireds,
                        unsigned short level256, unsigned short *C256, unsigned short *W256)
{
	unsigned int span;
	unsigned int w;

	if(mireds < minMireds){
		mireds = minMireds;
	}
	if(mireds > maxMireds){
		mireds = maxMireds;
	}

	span = (unsigned int)maxMireds - (unsigned int)minMireds;

	/* A zero span would be a divide by zero. On this part the integer division
	 * routine polls the hardware divider's status bit and never checks for a
	 * zero divisor, so that is a CPU hang, not a wrong answer. The endpoints
	 * come from ZCL attributes; guard them. */
	if(span == 0u){
		w = 0u;
	}else{
		w = ((unsigned int)(mireds - minMireds) * (unsigned int)level256) / span;
	}

	*W256 = (unsigned short)w;
	*C256 = (unsigned short)((unsigned int)level256 - w);
}
