/********************************************************************************************************
 * @file    moes_color.h
 *
 * @brief   Pure colour-space maths, deliberately free of SDK, ZCL and hardware
 *          dependencies so the same code the firmware runs can be executed and
 *          checked on the host (tools/color_hosttest).
 *
 * @date    2026
 *******************************************************************************************************/
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* These mirror ZCL_COLOR_ATTR_HUE_MAX / ZCL_COLOR_ATTR_SATURATION_MAX. This
 * file must not include the ZCL headers - keeping it standalone is what makes
 * it host-testable - so tuyaLightCtrl.c asserts at compile time that the two
 * definitions still agree. */
#define MOES_COLOR_HUE_MAX      0xFE
#define MOES_COLOR_SAT_MAX      0xFE

/*
 * CIE 1931 xy chromaticity -> HSV hue/saturation, integer only.
 *
 * x and y are ZCL CurrentX/CurrentY (0..65535 == 0.0..1.0). Luminance is
 * deliberately NOT derived here: the output stage applies `level` separately,
 * so only chromaticity is converted. Outputs are on the ZCL 0..0xFE scale.
 */
void moes_xyToHueSat(unsigned short x, unsigned short y,
                     unsigned char *hue, unsigned char *saturation);

#ifdef __cplusplus
}
#endif
