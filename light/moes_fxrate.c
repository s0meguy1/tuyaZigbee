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

unsigned char moes_fxDensityThreshold(unsigned char densityPct)
{
	unsigned int d = (densityPct > 100u) ? 100u : densityPct;

	/* 100 percent must be every slot: 255 is the largest 8-bit hash value, and
	 * the comparison below is strict, so map onto 0..256 and cap. */
	unsigned int th = (d * 256u + 50u) / 100u;

	return (unsigned char)((th > 255u) ? 255u : th);
}

int moes_fxBurstActive(unsigned int t, unsigned char threshold, unsigned int seed)
{
	unsigned int slot = t / MOES_FX_BURST_SLOT_MS;

	/* Fold the seed in with an odd multiplier so nearby seeds give completely
	 * different sequences rather than the same one shifted by a slot. */
	unsigned int h = moes_fxHash32(slot ^ (seed * 2654435761u));

	if(threshold == 255u){
		return 1;   /* density 100: every slot, no hash can exceed 255 */
	}
	return ((h & 0xffu) < threshold) ? 1 : 0;
}

unsigned int moes_fxEffectivePhase(unsigned int phaseDeg, unsigned char index, unsigned int spreadDeg)
{
	unsigned int p = phaseDeg % 360u;

	if(index != 0xFFu){
		/* index <= 254 and spread <= 359: the product is under 92 000, far
		 * inside 32 bits even before the modulo. */
		p = (p + (unsigned int)index * (spreadDeg % 360u)) % 360u;
	}
	return p;
}

unsigned int moes_fxPhaseOffsetMs(unsigned int periodMs, unsigned int phaseDeg)
{
	/* The longest period any effect uses is 10 s at speed 50 and 500 s at
	 * speed 1 (fxPeriod(10000) at sp 1 = 500 000 ms); 500 000 * 359 is 1.8e8,
	 * inside 32 bits. */
	return (periodMs * (phaseDeg % 360u)) / 360u;
}

unsigned char moes_fxFadeLevel(unsigned char from, unsigned char to,
                               unsigned int elapsedMs, unsigned int fadeMs)
{
	unsigned int k;

	if(fadeMs == 0u || elapsedMs >= fadeMs){
		return to;
	}
	/* elapsed < fade <= 65535 and |to - from| <= 255: the product is under
	 * 1.7e7. Rounded to nearest so the last step lands exactly on `to`. */
	k = elapsedMs;
	if(to >= from){
		return (unsigned char)(from + ((unsigned int)(to - from) * k + fadeMs / 2u) / fadeMs);
	}
	return (unsigned char)(from - ((unsigned int)(from - to) * k + fadeMs / 2u) / fadeMs);
}
