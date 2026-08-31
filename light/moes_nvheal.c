/********************************************************************************************************
 * @file    moes_nvheal.c
 *
 * @brief   See moes_nvheal.h.
 *
 * @date    2026
 *******************************************************************************************************/

#if (__PROJECT_TL_DIMMABLE_LIGHT__)

#include "../common/comm_cfg.h"
#include "tl_common.h"
#include "zb_api.h"
#include "moes_nvheal.h"

#if MOES_TS0505B

/* Item id in NV_MODULE_APP, alongside 0x71 probation and 0x72 boot-young. */
#define MOES_NV_ITEM_OWNER      0x70

/* "MOES". Cannot arise from erased flash (0xFFFFFFFF) or zeroed flash, and the
 * stock application has no reason to write it. */
#define MOES_NV_OWNER_MAGIC     0x4D4F4553u

/* The exact range an SWire erase was proven to clear the wedge over. */
#define MOES_NV_REGION_START    0xD8000
#define MOES_NV_REGION_END      0xE8000

/* NV_MODULE_APP is module 6, so its two sectors start at
 * NV_BASE_ADDRESS + 0x1000 * 2 * 6. Scanned directly because the pre-stack pass
 * runs before nv_init() and cannot use nv_flashReadNew(). */
#define MOES_NV_APP_START       (MOES_NV_REGION_START + 0x1000 * 2 * 6)
#define MOES_NV_APP_LEN         (2 * 0x1000)

static bool s_healed = FALSE;

/*********************************************************************
 * @fn      moes_nvHealMarkerPresent
 *
 * @brief   Direct flash scan for the marker, in 256-byte chunks so this costs
 *          one small stack buffer rather than 8 KB.
 */
static bool moes_nvHealMarkerPresent(void)
{
	u8 buf[256 + 3];
	u32 off;

	for(off = 0; off < MOES_NV_APP_LEN; off += 256){
		/* Overlap by 3 bytes so a marker straddling a chunk boundary is still
		 * seen. The tail read stays inside the module's own sectors. */
		u16 len = (u16)((off + sizeof(buf) <= MOES_NV_APP_LEN) ? sizeof(buf) : (MOES_NV_APP_LEN - off));

		flash_read(MOES_NV_APP_START + off, len, buf);

		for(u16 i = 0; i + 4 <= len; i++){
			u32 v = ((u32)buf[i]) | ((u32)buf[i + 1] << 8)
			      | ((u32)buf[i + 2] << 16) | ((u32)buf[i + 3] << 24);
			if(v == MOES_NV_OWNER_MAGIC){
				return TRUE;
			}
		}
	}

	return FALSE;
}

void moes_nvHealPreStack(void)
{
	u32 addr;

	s_healed = FALSE;

	if(moes_nvHealMarkerPresent()){
		/* Our NV. The overwhelmingly common path: a short flash scan, then
		 * nothing at all. */
		return;
	}

	/* Foreign NV - a conversion from stock, or a factory-new module. Erase
	 * before nv_init() ever looks at it, so the NV layer comes up against blank
	 * flash and its state cannot disagree with the flash. drv_flash feeds the
	 * watchdog around each erase, so 16 sectors cannot trip it. */
	for(addr = MOES_NV_REGION_START; addr < MOES_NV_REGION_END; addr += 0x1000){
		flash_erase(addr);
	}

	s_healed = TRUE;
}

void moes_nvHealPostStack(void)
{
	u32 owner = MOES_NV_OWNER_MAGIC;

	if(!s_healed){
		return;
	}

	/* Safe now: nv_init() has run against the blank region this boot, so the
	 * in-RAM index and the flash agree. This is exactly what build 28 got wrong
	 * by writing before that was true. */
	nv_flashWriteNew(1, NV_MODULE_APP, MOES_NV_ITEM_OWNER,
	                 sizeof(owner), (u8 *)&owner);
}

#endif  /* MOES_TS0505B */
#endif  /* __PROJECT_TL_DIMMABLE_LIGHT__ */
