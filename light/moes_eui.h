/********************************************************************************************************
 * @file    moes_eui.h
 *
 * @brief   Factory EUI-64 parsing with no SDK, ZCL, or hardware dependencies.
 *
 * The exact production implementation is deliberately host-testable in
 * tools/eui_hosttest.  The factory value is written in display order, while
 * Telink's MAC PIB stores an IEEE address least-significant byte first.
 *
 * @date    2026
 *******************************************************************************************************/
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MOES_EUI_ASCII_LEN 16
#define MOES_EUI_BYTES      8

/* Parse exactly 16 ASCII hex digits in display order (MSB first), validate
 * the Telink a4:c1:38 OUI in that order, and return the SDK-internal LSB-first
 * byte sequence.  On failure sdkEui is not modified. */
bool moes_euiParseDisplayAscii(const unsigned char displayAscii[MOES_EUI_ASCII_LEN],
                               unsigned char sdkEui[MOES_EUI_BYTES]);

#ifdef __cplusplus
}
#endif
