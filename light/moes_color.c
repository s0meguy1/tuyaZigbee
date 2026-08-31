/********************************************************************************************************
 * @file    moes_color.c
 *
 * @brief   Pure colour-space maths. No SDK, no ZCL, no hardware - so
 *          tools/color_hosttest compiles and runs THIS file, not a copy of it.
 *
 * @date    2026
 *******************************************************************************************************/
#include "moes_color.h"

/*********************************************************************
 * @fn      moes_xyToHueSat
 *
 * @brief   CIE 1931 xy -> sRGB -> HSV, integer only.
 *
 *          Build 20. Both firmware render paths (hwLight_levelUpdate() and
 *          tuyaLight_updateColor()) send every non-colour-temperature mode
 *          through hsvToRGB(currentHue, currentSaturation, ...). An XY colour
 *          command therefore has to leave equivalent HSV behind, or it moves
 *          attributes and nothing else - which is exactly what it did before
 *          this function existed.
 */
unsigned char moes_hueDegToZcl(unsigned short degrees)
{
	unsigned int d = degrees % 360u;   /* 360 wraps to 0, like a colour wheel */

	/* +180 rounds to nearest without floating point. */
	return (unsigned char)(((d * MOES_COLOR_HUE_MAX) + 180u) / 360u);
}

unsigned char moes_satPctToZcl(unsigned char percent)
{
	unsigned int p = (percent > 100u) ? 100u : percent;

	return (unsigned char)(((p * MOES_COLOR_SAT_MAX) + 50u) / 100u);
}

void moes_xyToHueSat(unsigned short x, unsigned short y,
                     unsigned char *hue, unsigned char *saturation)
{
	/* Per-mille working scale; luminance held at Y = 1000. */
	long xs = (long)(((unsigned long)x * 1000UL) / 65536UL);
	long ys = (long)(((unsigned long)y * 1000UL) / 65536UL);
	long zs = 1000 - xs - ys;

	/* y == 0 is not a realisable chromaticity - the tristimulus values would
	 * be infinite. Treat it as unsaturated white instead of dividing by zero. */
	if(ys <= 0){
		*hue = 0;
		*saturation = 0;
		return;
	}

	long X = (xs * 1000) / ys;
	long Y = 1000;
	long Z = (zs * 1000) / ys;

	/* Bound the tristimulus values before the matrix so every product below
	 * stays far inside 32 bits: 3241 * 20000 is about 6.5e7. */
	if(X > 20000){ X = 20000; }
	if(X < 0){ X = 0; }
	if(Z > 20000){ Z = 20000; }
	if(Z < 0){ Z = 0; }

	/* sRGB D65 matrix, coefficients scaled by 1000. */
	long r = ( 3241 * X - 1537 * Y -  499 * Z) / 1000;
	long g = (-969  * X + 1876 * Y +   42 * Z) / 1000;
	long b = (   56 * X -  204 * Y + 1057 * Z) / 1000;

	if(r < 0){ r = 0; }
	if(g < 0){ g = 0; }
	if(b < 0){ b = 0; }

	/* Identify the maximum channel BEFORE any scaling: deriving it afterwards
	 * by comparing against a normalised 255 is off-by-one fragile. */
	long mx = r, mn = r;
	unsigned char mxi = 0;            /* 0 = red, 1 = green, 2 = blue */

	if(g > mx){ mx = g; mxi = 1; }
	if(b > mx){ mx = b; mxi = 2; }
	if(g < mn){ mn = g; }
	if(b < mn){ mn = b; }

	if(mx <= 0){
		*hue = 0;
		*saturation = 0;
		return;
	}

	long delta = mx - mn;

	*saturation = (unsigned char)((delta * MOES_COLOR_SAT_MAX) / mx);

	if(delta == 0){
		*hue = 0;                     /* achromatic: hue is undefined */
		return;
	}

	long deg;
	if(mxi == 0){
		deg = (60 * (g - b)) / delta;
	}else if(mxi == 1){
		deg = 120 + (60 * (b - r)) / delta;
	}else{
		deg = 240 + (60 * (r - g)) / delta;
	}
	if(deg < 0){ deg += 360; }
	if(deg >= 360){ deg -= 360; }

	*hue = (unsigned char)((deg * MOES_COLOR_HUE_MAX) / 360);
}
