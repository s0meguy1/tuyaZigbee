/********************************************************************************************************
 * @file    moes_fxrate.c
 *
 * @brief   See moes_fxrate.h.
 *
 * @date    2026
 *******************************************************************************************************/

#include "moes_fxrate.h"

static unsigned char moes_fxSpeedClamp(unsigned char speed)
{
	/* 0 is the firmware's "unset" value and means the 50 default. */
	if(speed == 0){
		return 50;
	}
	if(speed > 100){
		return 100;
	}
	return speed;
}

unsigned int moes_fxPeriodMs(unsigned int slowMs, unsigned char speed)
{
	unsigned char sp = moes_fxSpeedClamp(speed);
	unsigned int p;

	/* slowMs is the cycle at speed 50, so the divisor is 2*sp rather than sp:
	 * at sp == 50 the factor is exactly 100/100 == 1. Kept byte-for-byte as the
	 * original expression - the smooth effects are not what is being fixed here,
	 * and the largest caller value (10000) leaves the product far inside 32 bits. */
	p = (slowMs * 100u) / (2u * sp);

	return p ? p : 1u;
}

unsigned int moes_fxStrobePeriodMs(unsigned char speed)
{
	unsigned char sp = moes_fxSpeedClamp(speed);
	unsigned int dHz;
	unsigned int p;

	/* Linear in FREQUENCY across the speed range, in tenths of a hertz.
	 * Interpolating the period instead would crowd almost the whole speed
	 * dial into the slowest few hertz. */
	dHz = MOES_FX_STROBE_MIN_DHZ
		+ ((unsigned int)(sp - 1u) * (MOES_FX_STROBE_MAX_DHZ - MOES_FX_STROBE_MIN_DHZ)) / 99u;

	/* period[ms] = 10000 / dHz, rounded to nearest so the slowest steps do not
	 * all truncate onto the same period. */
	p = (10000u + dHz / 2u) / dHz;

	if(p < MOES_FX_MIN_PERIOD_MS){
		p = MOES_FX_MIN_PERIOD_MS;
	}

	return p;
}

/* 32-bit avalanche mixer (the well-known "lowbias32" constants). Used instead of
 * zb_random() so a slot's burst/dark decision is stable while the slot lasts and
 * reproducible on the host - the twinkle/candle effects use the RNG because they
 * WANT per-frame noise; a burst must not flicker in and out mid-flash. */
static unsigned int moes_fxHash32(unsigned int x)
{
	x ^= x >> 16;
	x *= 0x7feb352dU;
	x ^= x >> 15;
	x *= 0x846ca68bU;
	x ^= x >> 16;
	return x;
}

unsigned char moes_fxBurstThreshold(unsigned char speed)
{
	unsigned char sp = moes_fxSpeedClamp(speed);

	return (unsigned char)(MOES_FX_BURST_MIN_THRESH
		+ ((unsigned int)(sp - 1u) * (MOES_FX_BURST_MAX_THRESH - MOES_FX_BURST_MIN_THRESH)) / 99u);
}

unsigned int moes_fxExplodeRampMs(unsigned char speed)
{
	unsigned char sp = moes_fxSpeedClamp(speed);

	/* Inverted against the other mappings on purpose: more speed means a
	 * SHORTER ramp, i.e. a more violent explosion. */
	return MOES_FX_EXPLODE_MAX_MS
		- ((unsigned int)(sp - 1u) * (MOES_FX_EXPLODE_MAX_MS - MOES_FX_EXPLODE_MIN_MS)) / 99u;
}

int moes_fxBurstActive(unsigned int t, unsigned char speed, unsigned int phase)
{
	unsigned int slot = t / MOES_FX_BURST_SLOT_MS;

	/* Fold the phase in with an odd multiplier so neighbouring phases give
	 * completely different sequences rather than the same one shifted by a
	 * slot - two fixtures one degree apart must not burst together. */
	unsigned int h = moes_fxHash32(slot ^ (phase * 2654435761u));

	return ((h & 0xffu) < moes_fxBurstThreshold(speed)) ? 1 : 0;
}
