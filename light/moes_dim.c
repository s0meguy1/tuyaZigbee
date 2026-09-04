/********************************************************************************************************
 * @file    moes_dim.c
 *
 * @brief   See moes_dim.h. No SDK, ZCL or hardware dependencies - that is what
 *          makes tools/level_curve_hosttest able to execute the real firmware
 *          arithmetic instead of a copy of it.
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
