/********************************************************************************************************
 * @file    moes_nvitems.h
 *
 * @brief   Private NV_MODULE_APP item ids used by the MOES TS0505B firmware.
 *
 * Keep these in one place. Build 30 accidentally gave the four-byte ownership
 * marker and the one-byte reset-skip flag the same item id (0x70). The SDK's
 * length matching accepts a four-byte record for a one-byte read, so the owner
 * marker was consumed as a true reset flag and replaced. The preprocessor
 * checks below make that class of collision a build failure.
 *
 * @date    2026
 *******************************************************************************************************/
#pragma once

#define MOES_NV_ITEM_SKIP_RST          0x70
#define MOES_NV_ITEM_BOOT_PROBATION    0x71
#define MOES_NV_ITEM_BOOT_YOUNG        0x72
#define MOES_NV_ITEM_OWNER             0x73
/* Build 36: the fixture's light-show index, one byte (0..254, 0xFF none). Set
 * once at commissioning; written only when it changes. */
#define MOES_NV_ITEM_FX_INDEX          0x74

#if (MOES_NV_ITEM_SKIP_RST == MOES_NV_ITEM_BOOT_PROBATION) || \
    (MOES_NV_ITEM_SKIP_RST == MOES_NV_ITEM_BOOT_YOUNG) || \
    (MOES_NV_ITEM_SKIP_RST == MOES_NV_ITEM_OWNER) || \
    (MOES_NV_ITEM_SKIP_RST == MOES_NV_ITEM_FX_INDEX) || \
    (MOES_NV_ITEM_BOOT_PROBATION == MOES_NV_ITEM_BOOT_YOUNG) || \
    (MOES_NV_ITEM_BOOT_PROBATION == MOES_NV_ITEM_OWNER) || \
    (MOES_NV_ITEM_BOOT_PROBATION == MOES_NV_ITEM_FX_INDEX) || \
    (MOES_NV_ITEM_BOOT_YOUNG == MOES_NV_ITEM_OWNER) || \
    (MOES_NV_ITEM_BOOT_YOUNG == MOES_NV_ITEM_FX_INDEX) || \
    (MOES_NV_ITEM_OWNER == MOES_NV_ITEM_FX_INDEX)
#error "MOES private NV item ids must be unique"
#endif

/* The SDK does not require an exact length match when reading: it accepts any
 * stored length divisible by the requested length, then copies the STORED
 * length. Build 30 therefore copied a four-byte owner record into a one-byte
 * reset-skip local. Every private byte read must query and require size 1
 * before giving nv_flashReadNew() a one-byte destination. Callers include this
 * header only after the SDK types/API are visible. */
static inline nv_sts_t moes_nvReadByteExact(u8 itemId, u8 *value)
{
	u16 storedLen = 0;
	nv_sts_t status = nv_flashSingleItemSizeGet(NV_MODULE_APP, itemId, &storedLen);

	if(status != NV_SUCC){
		return status;
	}
	if(storedLen != 1){
		return NV_ITEM_LEN_NOT_MATCH;
	}
	return nv_flashReadNew(1, NV_MODULE_APP, itemId, 1, value);
}
