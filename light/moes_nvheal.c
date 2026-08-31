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
#include "moes_nvitems.h"
#include "moes_nvheal.h"
#include "moes_rescue.h"

#if MOES_TS0505B

/* "MOES". Cannot arise from erased flash (0xFFFFFFFF) or zeroed flash, and the
 * stock application has no reason to write it. */
#define MOES_NV_OWNER_MAGIC     0x4D4F4553u

/* Build 30 used 0x70 for the owner as well as the reset-skip flag. Keep this
 * name only so an installed b30 record can be recognised and migrated; new
 * writes use MOES_NV_ITEM_OWNER (0x73). */
#define MOES_NV_ITEM_OWNER_B30  MOES_NV_ITEM_SKIP_RST

/* The exact range an SWire erase was proven to clear the wedge over. */
#define MOES_NV_REGION_START    0xD8000
#define MOES_NV_REGION_END      0xE8000

/* NV_MODULE_APP is module 6, so its two 4 KiB sectors start at
 * NV_BASE_ADDRESS + 0x1000 * 2 * 6. Read directly because the pre-stack pass
 * runs before nv_init() and cannot use nv_flashReadNew(). */
#define MOES_NV_APP_START          (MOES_NV_REGION_START + 0x1000 * 2 * 6)
#define MOES_NV_APP_SECTOR_SIZE    0x1000
#define MOES_NV_APP_SECTOR_COUNT   2

/* Telink's APP module reserves 512 bytes for the sector header and 63 eight-
 * byte index records. Payloads start at +0x200. These sizes are part of the
 * on-flash ABI and are verified by the host test against synthetic records
 * shaped exactly like the captured custom NV. */
#define MOES_NV_MODULE_INFO_SIZE   0x200
#define MOES_NV_INDEX_COUNT        ((MOES_NV_MODULE_INFO_SIZE - 4) / 8)

#define MOES_NV_SECTOR_VALID_CRC   0x7A7A
#define MOES_NV_ITEM_VALID_SINGLE  0x7A
#define MOES_NV_ITEM_INVALID       0x50
#define MOES_NV_HEADER_VALID_CRC   0x7A

typedef struct {
	u16 usedFlag;
	u8 idName;
	u8 opSect;
} moes_nv_sector_t;

typedef struct {
	u32 offset;
	u16 size;
	u8 itemId;
	u8 usedState;
} moes_nv_index_t;

typedef struct {
	u32 checkSum;
	u16 size;
	u8 itemId;
	u8 used;
} moes_nv_header_t;

typedef char moes_nv_sector_size_must_be_4[(sizeof(moes_nv_sector_t) == 4) ? 1 : -1];
typedef char moes_nv_index_size_must_be_8[(sizeof(moes_nv_index_t) == 8) ? 1 : -1];
typedef char moes_nv_header_size_must_be_8[(sizeof(moes_nv_header_t) == 8) ? 1 : -1];

typedef enum {
	MOES_NV_PROOF_NONE = 0,
	MOES_NV_PROOF_LEGACY,
	MOES_NV_PROOF_CURRENT,
} moes_nv_proof_t;

static bool s_ownerWriteNeeded = FALSE;

static bool moes_nvSectorValid(u8 sectorNo)
{
	moes_nv_sector_t sector;
	u8 crcBytes[2];
	u8 storedCrc;
	u32 crc;

	flash_read(MOES_NV_APP_START + (u32)sectorNo * MOES_NV_APP_SECTOR_SIZE,
	           sizeof(sector), (u8 *)&sector);

	if(sector.usedFlag != MOES_NV_SECTOR_VALID_CRC ||
	   sector.idName != NV_MODULE_APP ||
	   (sector.opSect & 0x03) != sectorNo){
		return FALSE;
	}

	storedCrc = sector.opSect >> 2;
	crcBytes[0] = sector.idName;
	crcBytes[1] = sector.opSect & 0x03;
	crc = xcrc32(crcBytes, sizeof(crcBytes), 0xFFFFFFFF);
	return (u8)(crc & 0x3F) == storedCrc;
}

/* Validate one indexed CRC-format item without trusting the SDK's in-RAM NV
 * state. usedState is excluded from Telink's index CRC, so b30's now-invalid
 * owner record remains safely recognisable after the colliding reset-skip
 * write changed only that byte from 0x7A to 0x50. */
static bool moes_nvRecordRead(u32 sectorStart, const moes_nv_index_t *index,
					  u8 itemId, u16 payloadLen, bool allowInvalidIndex,
					  u8 *payload)
{
	moes_nv_header_t header;
	u8 meta[4];
	u32 crc;

	if(index->itemId != itemId || index->size != sizeof(header) + payloadLen){
		return FALSE;
	}
	if(index->usedState != MOES_NV_ITEM_VALID_SINGLE &&
	   !(allowInvalidIndex && index->usedState == MOES_NV_ITEM_INVALID)){
		return FALSE;
	}
	if(index->offset < sectorStart + MOES_NV_MODULE_INFO_SIZE ||
	   (index->offset & 0x03) != 0 ||
	   index->offset > sectorStart + MOES_NV_APP_SECTOR_SIZE - index->size){
		return FALSE;
	}

	flash_read(index->offset, sizeof(header), (u8 *)&header);
	if(header.size != payloadLen || header.itemId != itemId ||
	   header.used != MOES_NV_HEADER_VALID_CRC){
		return FALSE;
	}

	/* Upper half: CRC of the first seven index bytes (usedState excluded). */
	crc = xcrc32((const u8 *)index, sizeof(*index) - 1, 0xFFFFFFFF);
	if((u16)(crc & 0xFFFF) != (u16)(header.checkSum >> 16)){
		return FALSE;
	}

	flash_read(index->offset + sizeof(header), payloadLen, payload);
	crc = xcrc32(payload, payloadLen, 0xFFFFFFFF);
	meta[0] = (u8)(payloadLen & 0xFF);
	meta[1] = (u8)(payloadLen >> 8);
	meta[2] = itemId;
	meta[3] = header.used;
	crc = xcrc32(meta, sizeof(meta), crc);
	return (u16)(crc & 0xFFFF) == (u16)(header.checkSum & 0xFFFF);
}

/*********************************************************************
 * @fn      moes_nvOwnershipProof
 *
 * @brief   Find a current owner marker or a CRC-backed record that only our
 *          pre-marker firmware wrote. The captured virgin fixture has a blank
 *          APP module; its post-conversion capture has a valid 0x72 record.
 */
static moes_nv_proof_t moes_nvOwnershipProof(void)
{
	moes_nv_proof_t proof = MOES_NV_PROOF_NONE;
	u8 payload[4];

	for(u8 sectorNo = 0; sectorNo < MOES_NV_APP_SECTOR_COUNT; sectorNo++){
		u32 sectorStart = MOES_NV_APP_START + (u32)sectorNo * MOES_NV_APP_SECTOR_SIZE;

		if(!moes_nvSectorValid(sectorNo)){
			continue;
		}

		for(u16 i = 0; i < MOES_NV_INDEX_COUNT; i++){
			moes_nv_index_t index;
			u32 owner;

			flash_read(sectorStart + sizeof(moes_nv_sector_t) +
			           (u32)i * sizeof(index), sizeof(index), (u8 *)&index);

			if(moes_nvRecordRead(sectorStart, &index, MOES_NV_ITEM_OWNER,
			                     sizeof(owner), FALSE, payload)){
				memcpy(&owner, payload, sizeof(owner));
				if(owner == MOES_NV_OWNER_MAGIC){
					return MOES_NV_PROOF_CURRENT;
				}
			}

			/* b30's owner may now be invalid in the index because 0x70 also
			 * belonged to the reset-skip flag. Its record CRC still proves that
			 * this exact four-byte value was deliberately written. */
			if(moes_nvRecordRead(sectorStart, &index, MOES_NV_ITEM_OWNER_B30,
			                     sizeof(owner), TRUE, payload)){
				memcpy(&owner, payload, sizeof(owner));
				if(owner == MOES_NV_OWNER_MAGIC){
					proof = MOES_NV_PROOF_LEGACY;
				}
			}

			/* Builds 19-30 write these one-byte rescue records. A matching id
			 * alone is not enough: moes_nvRecordRead also requires the active
			 * sector, bounded offset, matching header, index CRC and payload CRC. */
			if(moes_nvRecordRead(sectorStart, &index, MOES_NV_ITEM_BOOT_PROBATION,
			                     1, FALSE, payload) &&
			   payload[0] <= MOES_RESCUE_FAIL_THRESHOLD){
				proof = MOES_NV_PROOF_LEGACY;
			}
			if(moes_nvRecordRead(sectorStart, &index, MOES_NV_ITEM_BOOT_YOUNG,
			                     1, FALSE, payload) && payload[0] <= 1){
				proof = MOES_NV_PROOF_LEGACY;
			}
		}
	}

	return proof;
}

void moes_nvHealPreStack(void)
{
	moes_nv_proof_t proof;
	u32 addr;

	s_ownerWriteNeeded = FALSE;
	proof = moes_nvOwnershipProof();

	if(proof == MOES_NV_PROOF_CURRENT){
		/* Current owned NV: validate the marker, then do nothing. */
		return;
	}
	if(proof == MOES_NV_PROOF_LEGACY){
		/* Build 19-30 custom NV. Preserve it and add the collision-free owner
		 * marker after nv_init() has made the normal API available. */
		s_ownerWriteNeeded = TRUE;
		return;
	}

	/* Foreign NV - a conversion from stock, or a factory-new module. Erase
	 * before nv_init() ever looks at it, so the NV layer comes up against blank
	 * flash and its state cannot disagree with the flash. drv_flash feeds the
	 * watchdog around each erase, so 16 sectors cannot trip it. */
	for(addr = MOES_NV_REGION_START; addr < MOES_NV_REGION_END; addr += 0x1000){
		flash_erase(addr);
	}

	s_ownerWriteNeeded = TRUE;
}

void moes_nvHealPostStack(void)
{
	u32 owner = MOES_NV_OWNER_MAGIC;

	if(!s_ownerWriteNeeded){
		return;
	}

	/* Safe now: nv_init() has run against either the unchanged legacy-custom
	 * region or the freshly erased conversion region, so its in-RAM index and
	 * flash agree. This is exactly what build 28 got wrong by writing before
	 * that was true. */
	nv_flashWriteNew(1, NV_MODULE_APP, MOES_NV_ITEM_OWNER,
	                 sizeof(owner), (u8 *)&owner);
	s_ownerWriteNeeded = FALSE;
}

#endif  /* MOES_TS0505B */
#endif  /* __PROJECT_TL_DIMMABLE_LIGHT__ */
