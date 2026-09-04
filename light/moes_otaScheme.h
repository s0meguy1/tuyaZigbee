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

/*=======================================================================
 * OTA-CRITICAL. Prove any change below on the SWire bench before it
 * reaches a live fixture.
 *
 * A fault in this path does not look like a bad fade or a wrong colour.
 * It looks like a fixture that will not boot or will not rejoin, and a
 * fixture in that state CANNOT be recovered over the air. The only way
 * back is SWire, with the light physically down from the ceiling. That
 * has already cost this project one fixture: INCIDENT_2026-08-15.md.
 *
 * The host suites cannot catch it. They do not run the bootloader, do
 * not write flash and never perform a transfer, so green tests say
 * nothing about whether an update still installs.
 *
 * Before this reaches any live device:
 *   1. OTA_TEST_PLAN.md, "Phase 0 - bench unit. Mandatory."
 *   2. MOES_EDITING_GUIDE.md S1, the four invariants.
 *   3. A real OTA onto the bench fixture, then a power cycle, then a
 *      rejoin. An image that boots once is not proof.
 *
 * Ordinary application work - rendering, effects, ZCL behaviour - does
 * NOT need this. Build 38 is the worked example: it rewrote the whole
 * dimming path and touched no file carrying this banner, so the bench
 * stayed in its box. Check which side of the line you are on rather
 * than assuming either answer.
 *=====================================================================*/

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
