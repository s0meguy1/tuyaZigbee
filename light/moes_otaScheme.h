/********************************************************************************************************
 * @file    moes_otaScheme.h
 *
 * @brief   Atomic OTA scheme migration (bootloader mode -> no-bootloader
 *          dual-bank). Ported from romasku/tuya-zigbee-switch
 *          src/telink/ota_reformating/ (ensure_ota_scheme.c), same
 *          silicon and stock-bootloader family.
 *
 * The fleet gets this firmware over the air, so it first runs at 0x8000
 * under the stock Tuya bootloader. At the top of main() (RAM code only!)
 * this module moves the running image to the 0x40000 bank, clears the
 * startup flags at 0x0 and 0x8000, and reboots. From then on the TLSR8258
 * hardware bank-scan (0x0 / 0x20000 / 0x40000) boots us and every later
 * OTA is an atomic single-byte flag flip with the old bank intact -
 * no brick window, self-healing, forever.
 *
 * @date    2026
 *******************************************************************************************************/

#pragma once

#if defined(__cplusplus)
extern "C" {
#endif

/* call first thing from main(); never returns if a migration happened
 * (it resets). RAM-code only - no flash-resident calls before the
 * platform init when running under a bootloader. */
void ensure_correct_ota_scheme(void);

#if defined(__cplusplus)
}
#endif
