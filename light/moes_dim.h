/********************************************************************************************************
 * @file    moes_dim.h
 *
 * @brief   Pure dimming/output arithmetic, deliberately free of SDK, ZCL and
 *          hardware dependencies so the same code the firmware runs can be
 *          executed and checked on the host (tools/level_curve_hosttest).
 *
 *          Build 38. The render chain used to quantize three separate times -
 *          the colour-temperature split, the brightness curve and the PWM
 *          duty were all u8 or integer-percent - which left 78 distinct PWM
 *          values out of the ~12000 the hardware can produce, and collapsed
 *          ZCL levels 3,4,5,6 onto a single output. Everything here works in
 *          the wide 8.8 domain so the quantization happens once, at the
 *          hardware compare register.
 *
 * @date    2026
 *******************************************************************************************************/
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* The wide output domain: an 0..255 output level carried in 8.8 fixed point,
 * so 0..65280. Deliberately NOT 65535: full scale must stay an exact whole
 * number of u8 units so the u8 wrappers round-trip. */
#define MOES_DIM_MAX256         (255u * 256u)

/* Stock's brightness curve, as percentages of full duty.
 *
 * These are the factory config block's brightmin/brightmax at 0xF8000. The
 * shape is LINEAR and must stay linear: see the long comment in
 * tuyaLightCtrl.c for the hardware measurements behind that, and section 3 of
 * AI_BUILD38_FADE_BRIEF.md for why changing it would break every stored scene
 * and the match against the remaining stock fixtures. Only the precision of
 * the arithmetic changed in build 38, never the shape. */
#define MOES_BRIGHT_MIN_PCT     1
#define MOES_BRIGHT_MAX_PCT     100

/*
 * The brightness curve in the wide domain.
 *
 * This is the exact real-valued function the old integer-percent code
 * approximated: out = 2.55 + 0.99*v for a lit channel, zero for zero. The old
 * code rounded through whole percents and so produced only 101 distinct
 * outputs for 256 inputs; this differs from it by at most 3/255 of full scale
 * and is monotone non-decreasing over the whole domain.
 *
 * Zero must stay zero: brightmin is a dimming floor for a LIT channel, not an
 * output floor, and the Off path drives this same function.
 */
unsigned int moes_dimCurve256(unsigned int v256);

/* Active-level inversion, in the wide domain so it does not re-quantize. */
unsigned int moes_dimInvert256(unsigned int v256, unsigned char activeLow);

/* Wide duty -> PWM compare ticks. The single remaining quantizer. */
unsigned int moes_dimCmpTick(unsigned int v256, unsigned int pwmMaxTick);

/*
 * Colour temperature -> cool/warm channel split, in the wide domain.
 *
 * Linear interpolation between the physical mired endpoints, exactly as the
 * u8 version did, but carrying the 8.8 level so a dim fade does not collapse
 * both channels onto single digits.
 */
void moes_dimSplitCW256(unsigned short mireds, unsigned short minMireds, unsigned short maxMireds,
                        unsigned short level256, unsigned short *C256, unsigned short *W256);

#ifdef __cplusplus
}
#endif
