/********************************************************************************************************
 * @file    moes_flashcfg.c
 *
 * @brief   See moes_flashcfg.h.
 *
 * @date    2026
 *******************************************************************************************************/

#if (__PROJECT_TL_DIMMABLE_LIGHT__)

#include "../common/comm_cfg.h"
#include "tl_common.h"
#include "drivers/drv_flash.h"
#include "zb_api.h"
#include "zcl_include.h"
#include "factory_reset.h"
#include "moes_eui.h"
#include "moes_flashcfg.h"
#include "moes_nvitems.h"

STATIC_ASSERT(MOES_FLASH_EUI_ASCII_LEN == MOES_EUI_ASCII_LEN);

moes_cfg_t g_moesCfg;

/* ------------------------------------------------------------------ */
/* Tuya JSON selector -> TLSR8258 GPIO/PWM. These selector values are not
 * physical ZT3L module pad numbers; see light_ts0505b.h for the actual pad
 * mapping. The normal build currently uses compiled GPIO fallbacks. */
static const moes_pinmap_t moes_pinTable[] = {
	{ 4,  GPIO_PB4, 4, AS_PWM4 },
	{ 5,  GPIO_PB5, 5, AS_PWM5 },
	{10,  GPIO_PC2, 0, AS_PWM0 },
	{11,  GPIO_PC3, 1, AS_PWM1 },
	{13,  GPIO_PD2, 3, AS_PWM3 },
};

bool moes_pinmapLookup(u8 jsonPin, moes_pinmap_t *out){
	for(u8 i = 0; i < sizeof(moes_pinTable)/sizeof(moes_pinTable[0]); i++){
		if(moes_pinTable[i].pin == jsonPin){
			*out = moes_pinTable[i];
			return TRUE;
		}
	}
	return FALSE;
}

/* ------------------------------------------------------------------ */
void moes_flashCfgLoad(void){
	memset((u8 *)&g_moesCfg, 0, sizeof(g_moesCfg));

	/* Compiled-in board constants (device_config/light_ts0505b.h). These were
	 * read out of the factory JSON at 0x0F8000 on a real unit and independently
	 * confirmed against doctor64's traced schematic for the same ZT3L board,
	 * so they are known-good for every unit in this fleet.
	 *
	 * NOTE: parsing that JSON at runtime is deliberately NOT done. The whole
	 * fleet is one hardware revision, so it buys nothing, and the parser is a
	 * boot-path failure mode: a bad parse yields nonsense pin numbers or a
	 * nonsense PWM period on a device that cannot be recovered without wires.
	 * moes_flashGetIeee() below still reads the per-device identity block,
	 * which is the one value that genuinely differs per unit. */
	g_moesCfg.pwmhz     = MOES_PWM_FREQUENCY_DEFAULT;
	g_moesCfg.rstnum    = FACTORY_RESET_POWER_CNT_THRESHOLD;
	g_moesCfg.pmemory   = 1;
	g_moesCfg.defbright = 100;
	g_moesCfg.deftemp   = 100;
	g_moesCfg.gmwr = 100; g_moesCfg.gmwg = 100; g_moesCfg.gmwb = 100;

	/* pin_* left 0 on purpose: moes_chanInit() then takes the compiled
	 * GPIO/PWM-channel path rather than a module-pin lookup. */
	g_moesCfg.valid = 0;
}

/* ------------------------------------------------------------------ */
bool moes_flashGetIeee(u8 *ieee){
	u8 ascii[MOES_FLASH_EUI_ASCII_LEN];

	flash_read(MOES_FLASH_TUYA_ID_ADDR + MOES_FLASH_EUI_ASCII_OFF,
			   MOES_FLASH_EUI_ASCII_LEN, ascii);

	return moes_euiParseDisplayAscii(ascii, ieee);
}

/* ------------------------------------------------------------------ */
/* Reset-counter skip flag: one byte in our own NV module so the swap and
 * bank-migration soft resets are never mistaken for user power cycles.
 * (nv item ids are per-module; APP module id 6, we take a private id.) */

void moes_resetSkipNextBoot(void){
	u8 one = 1;
	nv_flashWriteNew(1, NV_MODULE_APP, MOES_NV_ITEM_SKIP_RST, 1, &one);
}

bool moes_resetConsumeSkipFlag(void){
	u8 flag = 0;
	if(moes_nvReadByteExact(MOES_NV_ITEM_SKIP_RST, &flag) == NV_SUCC && flag){
		flag = 0;
		nv_flashWriteNew(1, NV_MODULE_APP, MOES_NV_ITEM_SKIP_RST, 1, &flag);
		return TRUE;
	}
	return FALSE;
}

#endif  /* __PROJECT_TL_DIMMABLE_LIGHT__ */
