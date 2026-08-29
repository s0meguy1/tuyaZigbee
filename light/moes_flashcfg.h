/********************************************************************************************************
 * @file    moes_flashcfg.h
 *
 * @brief   Runtime board configuration from the Tuya factory blocks.
 *
 * The stock firmware keeps the board's whole hardware definition as a
 * plaintext (unquoted-key) JSON block at flash 0x0F8000, prefixed by the
 * magic EF FE ED FE, and the device's Zigbee identity as an ASCII EUI-64
 * inside the Tuya factory block at 0x0FB000 ("productId\0"... secret,
 * token, then 16 hex chars starting with the Telink OUI a4:c1:38).
 *
 * Reading these at boot keeps the replacement firmware data-driven the
 * way stock is: pin assignment, active level, PWM frequency, white-balance
 * trim and the factory-reset power-cycle count all come from flash, so a
 * hardware revision with different wiring needs no recompile. Everything
 * has a compiled fallback in device_config/light_ts0505b.h.
 *
 * @date    2026
 *******************************************************************************************************/

#pragma once

#if defined(__cplusplus)
extern "C" {
#endif

/* Tuya factory flash layout (1 MB part) */
#define MOES_FLASH_CFG_ADDR            0x0F8000
#define MOES_FLASH_CFG_MAGIC           {0xEF, 0xFE, 0xED, 0xFE}
#define MOES_FLASH_TUYA_ID_ADDR        0x0FB000
#define MOES_FLASH_EUI_ASCII_OFF       0x58    /* "<redacted-device>" */
#define MOES_FLASH_EUI_ASCII_LEN       16

typedef struct {
	u8 valid;            /* block parsed OK */
	u16 pwmhz;
	u8  rstnum;
	u8  pmemory;

	/* module pin numbers per channel, 0xFF = unknown */
	u8  pin_r, pin_g, pin_b, pin_c, pin_w;
	u8  lv_r, lv_g, lv_b, lv_c, lv_w;   /* active level, 1 = active high */

	/* white-balance trim, percent */
	u8  gmwr, gmwg, gmwb;

	u8  defbright;       /* percent */
	u8  deftemp;         /* percent */
} moes_cfg_t;

extern moes_cfg_t g_moesCfg;

/* Parse the blocks. Safe to call more than once. */
void moes_flashCfgLoad(void);

/* Tuya JSON selector -> TLSR8258 GPIO + PWM channel. The selector is not a
 * physical ZT3L module pad number. Returns FALSE if it is unknown. */
typedef struct {
	u8  pin;             /* Tuya JSON selector, not a module pad number */
	u32 gpio;            /* GPIO_PB4 ... */
	u8  pwmChannel;      /* silicon-fixed PWM channel for this pin */
	u16 pwmMux;          /* gpio_set_func() mux value */
} moes_pinmap_t;
bool moes_pinmapLookup(u8 jsonPin, moes_pinmap_t *out);

/* Tuya ASCII EUI-64 -> 8 bytes. FALSE if the block is absent or not
 * 16 hex digits. The Telink OUI (a4:c1:38) prefix is asserted. */
bool moes_flashGetIeee(u8 *ieee);

/* The 3-power-cycle reset counter must not count the soft resets the OTA
 * swap and the bank migration perform: set the skip flag before every
 * intentional SYSTEM_RESET(). Persisted in our own NV module. */
void moes_resetSkipNextBoot(void);
bool moes_resetConsumeSkipFlag(void);

#if defined(__cplusplus)
}
#endif
