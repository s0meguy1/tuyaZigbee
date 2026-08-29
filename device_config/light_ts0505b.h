/********************************************************************************************************
 * @file    light_ts0505b.h
 * Moes ZB-TDD6-RCW-4 downlight, Tuya Zigbee model TS0505B, manufacturer
 * "_TZ3210_b8jdosxo" (Tuya productId b8jdosxo). Tuya ZT3L module =
 * Telink TLSR8258F1KAT32, 1 MB SPI flash, 46 units deployed.
 *
 * Hardware (from the plaintext board-config JSON the stock firmware keeps
 * at flash 0x0F8000 - same data as the TS0501B strip-controller family,
 * all five channels populated):
 *
 *   channel      ZT3L physical pad  TLSR8258 GPIO   PWM channel
 *   red          13 / B4            PB4             PWM4
 *   green        6 / C3             PC3             PWM1
 *   blue         5 / D2             PD2             PWM3
 *   cool white   14 / B5            PB5             PWM5
 *   warm white   7 / C2             PC2             PWM0
 *   all channels active-high, pwmhz 4000
 *
 * The stock JSON's r_pin/g_pin/... values (4, 11, 13, 5, 10) are Tuya
 * configuration selectors, not physical ZT3L pad numbers. The values below
 * are compiled wiring; do not use the JSON selectors to wire a carrier.
 *
 * There is no button and no status LED on this board: pairing is the
 * stock 3-power-cycle gesture (factory_reset.c), reporting shows state.
 *
 * @date    2026
 *******************************************************************************************************/

#pragma once

/* Enable C linkage for C++ Compilers: */
#if defined(__cplusplus)
extern "C" {
#endif

/* ---- colour capability: extended colour light (hue/sat + xy + temp) ---- */
#define COLOR_RGB_SUPPORT                   1
#define COLOR_CCT_SUPPORT                   1

/* Debug mode config */
#define UART_PRINTF_MODE                    1
#define USB_PRINTF_MODE                     0

/* Identity: keep stock strings so an updated light keeps its z2m database
 * entry and existing converters keep matching it. */
/* Zigbee character strings are length-prefixed: the leading byte MUST equal
 * the number of characters that follow. "_TZ3210_b8jdosxo" is 16, not 17 -
 * an overstated prefix appends a garbage byte, so z2m's fingerprint match on
 * manufacturerName fails and the device shows up unsupported. */
#define ZCL_BASIC_MODEL_ID          {7,'T','S','0','5','0','5','B'}
#define ZCL_BASIC_MFG_NAME          {16,'_','T','Z','3','2','1','0','_','b','8','j','d','o','s','x','o'}

#define HARDWARE_REV                0x01

/* OTA identity. Manufacturer 0x6464 + imageType 0x0315 (light 0x05 family,
 * no-boot variant) is unique to this firmware and deliberately distinct
 * from the stock pair (0x1141/0xD3A3) so no public-index image can ever
 * be offered to an updated light. The from-stock conversion file overrides
 * the served header to 0x1141/0xD3A3 - see tools/make_ota.py -c/-t. */
#ifdef BUILD_BOOTLOADER
#define IMAGE_TYPE                          ((CHIP_TYPE << 8) | IMAGE_TYPE_BOOTLOADER)
#else
#define IMAGE_TYPE                          ((CHIP_TYPE << 8) | IMAGE_TYPE_LIGHT_0505B)
#endif

/* ---- PWM / LED wiring (fallback values; runtime values come from the
 *      Tuya JSON block when present) ---- */
#define LED_R                               GPIO_PB4    /* PWM4 */
#define LED_G                               GPIO_PC3    /* PWM1 */
#define LED_B                               GPIO_PD2    /* PWM3 */
#define LED_CW                              GPIO_PB5    /* PWM5 */
#define LED_WW                              GPIO_PC2    /* PWM0 */

#define PWM_R_CHANNEL                       4
#define PWM_G_CHANNEL                       1
#define PWM_B_CHANNEL                       3
#define PWM_CW_CHANNEL                      5
#define PWM_WW_CHANNEL                      0

#define PWM_R_CHANNEL_SET()                 do{ gpio_set_func(LED_R, AS_PWM4); }while(0)
#define PWM_G_CHANNEL_SET()                 do{ gpio_set_func(LED_G, AS_PWM1); }while(0)
#define PWM_B_CHANNEL_SET()                 do{ gpio_set_func(LED_B, AS_PWM3); }while(0)
#define PWM_CW_CHANNEL_SET()                do{ gpio_set_func(LED_CW, AS_PWM5); }while(0)
#define PWM_WW_CHANNEL_SET()                do{ gpio_set_func(LED_WW, AS_PWM0); }while(0)

#define R_LIGHT_PWM_CHANNEL                 PWM_R_CHANNEL
#define G_LIGHT_PWM_CHANNEL                 PWM_G_CHANNEL
#define B_LIGHT_PWM_CHANNEL                 PWM_B_CHANNEL
#define WARM_LIGHT_PWM_CHANNEL              PWM_WW_CHANNEL
#define COOL_LIGHT_PWM_CHANNEL              PWM_CW_CHANNEL
#define R_LIGHT_PWM_SET()                   PWM_R_CHANNEL_SET()
#define G_LIGHT_PWM_SET()                   PWM_G_CHANNEL_SET()
#define B_LIGHT_PWM_SET()                   PWM_B_CHANNEL_SET()
#define WARM_LIGHT_PWM_SET()                PWM_WW_CHANNEL_SET()
#define COOL_LIGHT_PWM_SET()                PWM_CW_CHANNEL_SET()

/* stock pwmhz:4000 - camera-safe, 12000 duty ticks at the 48 MHz PWM clock */
#define MOES_PWM_FREQUENCY_DEFAULT          4000

/* No status LEDs on this board */
#define LED_POWER                           NULL
#define LED_PERMIT                          NULL

/* No buttons: the stock gesture (3 power cycles) pairs/resets. The key
 * scan tables still need the VK ids so app_ui.c and drv_keyboard.c compile;
 * nothing on this board can press them.
 *
 * DO NOT SET THIS TO 1, and do not remove the HAVE_NET_BUTTON guards in
 * app_ui.c / tuyaLight.c. There is no button, and the pins named below are
 * NOT configured as inputs (no PC0_INPUT_ENABLE / PULL_WAKEUP_SRC_PC0 -
 * deliberately, since nothing is known to be connected to them on this
 * board). With the input buffer disabled gpio_read_all() returns 0 for
 * them, and kb_key_pressed() reads LOW as "key pressed". The scanner would
 * therefore see VK_SW1 held from the second poll after boot; five seconds
 * later app_key_handler() -> buttonKeepPressed() -> zb_factoryReset(), and
 * the light reboots into the same state forever. An OTA needs ~30 minutes
 * of uptime, so that is a fixture out of the ceiling. */
#define HAVE_NET_BUTTON                     0

#define  VK_SW1  0x01
#define  VK_SW2  0x02

#define	KB_MAP_NORMAL	{\
		{VK_SW1,}, \
		{VK_SW2,}, }
#define	KB_MAP_NUM		KB_MAP_NORMAL
#define	KB_MAP_FN		KB_MAP_NORMAL
#define KB_DRIVE_PINS  {NULL }
#define KB_SCAN_PINS   {GPIO_PC0, GPIO_PD4}

/* UART: debug printf shares the module UART TX (ZT3L pin 16 = TL_B1). */
#if ZBHCI_UART
	#error please configurate uart PIN!!!!!!
#endif

#if UART_PRINTF_MODE
	#define DEBUG_INFO_TX_PIN               GPIO_PB1
#endif

/* factory reset: stock rstnum:3 (3 power cycles within the window). The
 * migration/OTA reboot paths set a "don't count next boot" flag so soft
 * resets never feed the counter. */
#define FACTORY_RESET_POWER_CNT_THRESHOLD   3
#define FACTORY_RESET_TIMEOUT               2   /* seconds between cycles */

/* Effect engine */
#define MOES_EFFECTS_MAX                    16  /* effect ids 0..15 */
#define MOES_EFFECT_TICK_MS                 40  /* 25 fps */

/* Disable C linkage for C++ Compilers: */
#if defined(__cplusplus)
}
#endif
