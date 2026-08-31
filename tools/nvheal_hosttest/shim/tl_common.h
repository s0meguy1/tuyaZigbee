/* Minimum Telink surface needed by the real light/moes_nvheal.c. */
#pragma once

#include <stdint.h>
#include <string.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t  s32;
typedef uint8_t  bool;

#define TRUE  1
#define FALSE 0

typedef enum {
	NV_SUCC = 0,
	NV_ITEM_NOT_FOUND = 3,
	NV_ITEM_LEN_NOT_MATCH = 5,
} nv_sts_t;

#define NV_MODULE_APP 6

void flash_read(u32 addr, u32 len, u8 *buf);
void flash_erase(u32 addr);
u32 xcrc32(const u8 *buf, int len, u32 init);

nv_sts_t nv_flashSingleItemSizeGet(u8 id, u8 itemId, u16 *len);
nv_sts_t nv_flashReadNew(u8 single, u8 id, u8 itemId, u16 len, u8 *buf);
nv_sts_t nv_flashWriteNew(u8 single, u16 id, u8 itemId, u16 len, u8 *buf);
