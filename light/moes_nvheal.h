/********************************************************************************************************
 * @file    moes_nvheal.h
 *
 * @brief   First-boot NV self-heal, so an OTA conversion needs no programmer.
 *
 * THE BUG THIS EXISTS FOR (measured 2026-08-30, twice on real conversions)
 *
 * A stock->custom OTA transfers and installs perfectly, the firmware boots,
 * reaches stack_init() and posts no exception - and then the MAC layer
 * livelocks: 25 of 25 PC samples pinned at zb_macTimerEventProc+6. No radio, no
 * join, z2m reporting "can not get active endpoints". A true power cycle does
 * NOT clear it. Erasing the NV region over SWire clears it instantly, and the
 * light rejoins keeping its address. Our image inherits whatever the stock Tuya
 * application left at NV_BASE_ADDRESS and the stack wedges on it.
 *
 * TWO EARLIER ATTEMPTS FAILED, BOTH FOR THE SAME REASON
 *
 * Build 28 erased and then wrote its marker straight away. But nv_init() runs
 * inside stack_init(), BEFORE this code, so the NV layer's in-RAM index still
 * described the records the erase had just destroyed. The write landed against
 * stale state and corrupted the sector - a header of repeated 0x03 where erased
 * flash must read 0xFF - and the SDK's own next NV write hung: 12/12 samples in
 * factoryRst_powerCntSave.
 *
 * Build 29 therefore erased, reset, and wrote the marker on the following boot,
 * guarding against a loop with an always-on analog register. That guard does
 * not survive SYSTEM_RESET on this part, so the flag read clear every boot and
 * the device erased and reset forever: NV 100% blank, marker never written,
 * 12/12 samples in flash_wait_done. Do not assume DEEP_ANA_REG* survives a soft
 * reset - it was never demonstrated to, and the build-24 boot-reason
 * breadcrumb rests on the same unproven assumption.
 *
 * THE FIX: CLASSIFY BEFORE THE STACK, AND DO NOT RESET AT ALL
 *
 * The whole difficulty was erasing NV after the NV layer had already read it.
 * moes_nvHealPreStack() therefore runs at the very top of user_init(), before
 * stack_init(), and parses the APP module directly because the NV API is not up
 * yet. With no ownership proof it erases the foreign region there and then;
 * nv_init() subsequently initialises against blank flash, so its state and the
 * flash agree and no reset is needed. moes_nvHealPostStack() writes the marker
 * afterwards through the normal NV API, when that API is coherent.
 *
 * No reset, no retained-register guard, and nothing that can loop: after the
 * erase the marker is written, and every later boot finds it and does nothing.
 *
 * BUILD 31: UPGRADES MUST NOT LOOK LIKE CONVERSIONS
 *
 * Build 30 treated every image older than its new marker as foreign. An OTA
 * from working Build 27 therefore erased valid custom network state and forced
 * an unnecessary rejoin. Build 31 recognises only complete CRC-backed Telink
 * records: current owner item 0x73, Build 19-30 rescue items 0x71/0x72 with
 * valid values, or Build 30's authenticated legacy owner. Legacy custom NV is
 * preserved and migrated to 0x73 after stack_init(); genuine stock captures
 * have the APP module fully erased and still take the required heal path.
 *
 * Build 30 also assigned its four-byte owner and the one-byte reset-skip flag
 * the same APP item id, 0x70. The SDK accepts a stored length divisible by the
 * requested length and then copies the stored length, so the skip read copied
 * four bytes into a one-byte local: a three-byte stack overwrite during boot.
 * Build 31 centralises unique private IDs and requires an exact stored length
 * before every private byte read. Linked disassembly and host tests cover both
 * sides of that contract.
 *
 * IDENTITY IS SAFE. The EUI-64 lives in the factory block at 0xFB000, far
 * outside the NV region. Verified on hardware: after erasing 0xD8000-0xE7FFF
 * the light still read its factory EUI and rejoined under its original address,
 * keeping its Home Assistant entity. The cost is a rejoin, so a conversion
 * needs permit-join open.
 *
 * @date    2026
 *******************************************************************************************************/
#pragma once

#if defined(__cplusplus)
extern "C" {
#endif

/*
 * MUST be the first thing user_init() does, and in particular must run BEFORE
 * stack_init(): the entire point is to erase before nv_init() reads anything.
 */
void moes_nvHealPreStack(void);

/*
 * Call after stack_init(). Writes the ownership marker if the pre-stack pass
 * erased foreign NV or preserved legacy custom NV, using the normal NV API now
 * that it is coherent. A no-op for current owned NV.
 */
void moes_nvHealPostStack(void);

#if defined(__cplusplus)
}
#endif
