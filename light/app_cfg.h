/********************************************************************************************************
 * @file    app_cfg.h
 *
 * @brief   This is the header file for app_cfg
 *
 * @author  Zigbee Group
 * @date    2021
 *
 * @par     Copyright (c) 2021, Telink Semiconductor (Shanghai) Co., Ltd. ("TELINK")
 *			All rights reserved.
 *
 *          Licensed under the Apache License, Version 2.0 (the "License");
 *          you may not use this file except in compliance with the License.
 *          You may obtain a copy of the License at
 *
 *              http://www.apache.org/licenses/LICENSE-2.0
 *
 *          Unless required by applicable law or agreed to in writing, software
 *          distributed under the License is distributed on an "AS IS" BASIS,
 *          WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *          See the License for the specific language governing permissions and
 *          limitations under the License.
 *
 *******************************************************************************************************/

#pragma once

/* Enable C linkage for C++ Compilers: */
#if defined(__cplusplus)
extern "C" {
#endif


/**********************************************************************
 * Version configuration
 */
#include "version_cfg.h"

/**********************************************************************
 * Product Information
 */

/* HCI interface */
#define	ZBHCI_UART						0

/* RGB or CCT */
//#define COLOR_RGB_SUPPORT				0
//#define COLOR_CCT_SUPPORT				0

/* BDB */
#define TOUCHLINK_SUPPORT				1
#define FIND_AND_BIND_SUPPORT			0


/* Voltage detect module */
/* If VOLTAGE_DETECT_ENABLE is set,
 * 1) if MCU_CORE_826x is defined, the DRV_ADC_VBAT_MODE mode is used by default,
 * and there is no need to configure the detection IO port;
 * 2) if MCU_CORE_8258 or MCU_CORE_8278 is defined, the DRV_ADC_VBAT_MODE mode is used by default,
 * we need to configure the detection IO port, and the IO must be in a floating state.
 * 3) if MCU_CORE_B91 is defined, the DRV_ADC_BASE_MODE mode is used by default,
 * we need to configure the detection IO port, and the IO must be connected to the target under test,
 * such as VCC.
 */
#define VOLTAGE_DETECT_ENABLE						0

#if defined(MCU_CORE_826x)
	#define VOLTAGE_DETECT_ADC_PIN					0
#elif defined(MCU_CORE_8258) || defined(MCU_CORE_8278)
	#define VOLTAGE_DETECT_ADC_PIN					GPIO_PC5
#elif defined(MCU_CORE_B91)
	#define VOLTAGE_DETECT_ADC_PIN					ADC_GPIO_PB0
#endif

/* Watch dog module */
#define MODULE_WATCHDOG_ENABLE						1

/* MOES: boot-phase watchdog (2026-08-28, build 15). The stock main() starts
 * the runtime watchdog only AFTER user_init() returns, and enables IRQs only
 * then too. A software forward-progress stall in early initialization can
 * therefore persist across a reset-pin reset if it recurs at the same point.
 * The bench rf_setTrxState freeze was observed with brownout/silicon
 * confounders, so it is not firmware proof of that path.
 * 1 = start a long watchdog after drv_platform_init() returns (clock is up,
 * user_init() not yet entered), then tighten to the normal 600 ms interval
 * only after MOES_BOOT_WATCHDOG_SETTLE_MS of runtime (build 16; build 15
 * tightened immediately after user_init() and that window change is the
 * prime suspect for the bench corruption cascade). Coverage starts only
 * there and applies to
 * equivalent software stalls while Timer2 and its clock remain functional;
 * it cannot recover a frozen timer/SRAM domain. user_init() normally
 * completes in well under a second; the interval below is headroom while
 * still bounding such a stall. Flash writes and erases feed the watchdog
 * around their operations, but plain flash_read() does not. Normal reads
 * are short; a hung read is intentionally bounded only if Timer2 continues
 * advancing. Since build 17 the interval also has to cover an unjoined
 * boot's commissioning phases (see MOES_BOOT_WATCHDOG_BOOT_MS). */
#define MOES_BOOT_WATCHDOG_ENABLE					1
/* MOES (build 17): 30 s (was 10 s). A blank-NV boot runs channel scans and
 * commissioning before the join completes; 10 s starved that phase on the
 * field pilot (2026-08-29) and rescue-mode boots never reached their OTA
 * query. The main loop still clears the watchdog every iteration, so a true
 * hang stays bounded at this interval. */
#define MOES_BOOT_WATCHDOG_BOOT_MS					30000

/* MOES (build 16): keep the long boot window for this much *runtime* after
 * user_init() returns before tightening to the stock 600 ms interval. The
 * first post-boot cycles run the heaviest reporting/binding NV
 * normalization; build 15 tightened immediately and the bench then lost a
 * module to a mid-sequence corruption cascade
 * (bughunt/build15_runaway_cascade.md). 15 s covers the post-boot and
 * post-OTA normalization with wide margin; a genuine hang in the window is
 * still bounded by the 10 s boot interval. */
#define MOES_BOOT_WATCHDOG_SETTLE_MS				15000

/* UART module */
#if ZBHCI_UART
#define	MODULE_UART_ENABLE							1
#endif

#if (ZBHCI_USB_PRINT || ZBHCI_USB_CDC || ZBHCI_USB_HID || ZBHCI_UART)
	#define ZBHCI_EN								1
#endif


/**********************************************************************
 * ZCL cluster support setting
 */
#define ZCL_ON_OFF_SUPPORT							1
#define ZCL_LEVEL_CTRL_SUPPORT						1
#if (COLOR_RGB_SUPPORT || COLOR_CCT_SUPPORT)
#define ZCL_LIGHT_COLOR_CONTROL_SUPPORT				1
#endif
#define ZCL_GROUP_SUPPORT							1
#define ZCL_SCENE_SUPPORT							1
#define ZCL_OTA_SUPPORT								1
#define ZCL_GP_SUPPORT								1
#define ZCL_WWAH_SUPPORT							0
#if TOUCHLINK_SUPPORT
#define ZCL_ZLL_COMMISSIONING_SUPPORT				1
#endif

#define MY_OTA_PERIODIC_QUERY_INTERVAL (6*60*60U) //seconds

#define AF_TEST_ENABLE								0


/**********************************************************************
 * Stack configuration
 */
#include "stack_cfg.h"


/**********************************************************************
 * EV configuration
 */
typedef enum{
	EV_POLL_ED_DETECT,
	EV_POLL_HCI,
    EV_POLL_IDLE,
	EV_POLL_MAX,
}ev_poll_e;

/* Disable C linkage for C++ Compilers: */
#if defined(__cplusplus)
}
#endif
