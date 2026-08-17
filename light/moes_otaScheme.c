/********************************************************************************************************
 * @file    moes_otaScheme.c
 *
 * @brief   See moes_otaScheme.h. Ported with minimal edits from
 *          romasku/tuya-zigbee-switch (src/telink/ota_reformating/),
 *          itself built on the Telink SDK ram-code flash primitives.
 *
 * @date    2026
 *******************************************************************************************************/

#include "../common/comm_cfg.h"

/* MOES: the whole file is dead code unless MOES_NOBOOT_MIGRATION is defined,
 * and it must stay that way until somebody deliberately turns it on.
 *
 * Both entry points here write 0x00000000 over the KNLT flag at 0x0 + 8 -
 * i.e. into the *stock Tuya bootloader*, the one thing that makes OTA
 * installs work (MOES_EDITING_GUIDE S1.3: "Never write that region"). If the
 * image they leave at 0x40000 does not boot, nothing does: the bootloader is
 * gone, the 0x8000 slot is gone, and there is no working wired write path on
 * this silicon. That can strand the deployed fleet, not "one bad update".
 *
 * They were previously compiled unconditionally - moes_otaBankInstall() only
 * needed MOES_TS0505B && BOOT_LOADER_MODE, which is exactly our build - so
 * they sat in the image as _attribute_ram_code_sec_ functions consuming the
 * RAM-code budget the stock bootloader copies, one accidental -D away from
 * being called. Guard the file with the same symbol that guards the call
 * sites (main.c, zb_appCb.c) so the pairing cannot drift.
 *
 * See FALLBACK_DESIGN.md S6 for why the dual-bank migration is not
 * recommended at all. */
#if defined(MOES_NOBOOT_MIGRATION)

#include "tl_common.h"
#include "moes_otaScheme.h"

#include "chip_8258/flash.h"

#define BOOTLOADER_MODE_MAIN_ADDR    0x8000
#define SMALL_OTA_FLASH_ADDR         0x20000
#define IMAGE_SIZE_OFFSET            0x18
#define FLASH_TLNK_FLAG_OFFSET       8
#define TL_START_UP_FLAG_WHOLE       0x544c4e4b
#define MAX_FIRMWARE_SIZE            0x40000

/* Ramcode versions of flash read/write/memcmp (tiny - keep them in the
 * ram section so the migration runs even while the flash window that
 * backs executing code is being shuffled). */
int _attribute_ram_code_sec_ ram_memcmp(const void *m1, const void *m2, unsigned int n){
	const unsigned char *s1 = (const unsigned char *)m1;
	const unsigned char *s2 = (const unsigned char *)m2;
	while(n--){
		if(*s1 != *s2){
			return *s1 - *s2;
		}
		s1++; s2++;
	}
	return 0;
}

/* Definitions of private functions from the SDK's flash.c: */
_attribute_ram_code_sec_noinline_ void flash_mspi_read_ram(unsigned char cmd, unsigned long addr,
					unsigned char addr_en, unsigned char dummy_cnt,
					unsigned char *data, unsigned long data_len);
_attribute_ram_code_sec_noinline_ unsigned char flash_mspi_write_ram(unsigned char cmd, unsigned long addr,
					unsigned char addr_en, unsigned char *data,
					unsigned long data_len);

static void _attribute_ram_code_sec_ ram_flash_read_page(unsigned long addr, unsigned long len, unsigned char *buf){
	flash_mspi_read_ram(FLASH_READ_CMD, addr, 1, 0, buf, len);
}

static void _attribute_ram_code_sec_ ram_flash_write_page(unsigned long addr, unsigned long len, unsigned char *buf){
	unsigned int ns = PAGE_SIZE - (addr & (PAGE_SIZE - 1));
	int nw = 0;
	do {
		nw = len > ns ? ns : len;
		if(flash_mspi_write_ram(FLASH_WRITE_CMD, addr, 1, buf, nw) == 0){
			break;
		}
		ns = PAGE_SIZE;
		addr += nw;
		buf += nw;
		len -= nw;
	} while(len > 0);
}

static void _attribute_ram_code_sec_ ram_flash_erase_sector(unsigned long addr){
	flash_mspi_write_ram(FLASH_SECT_ERASE_CMD, addr, 1, NULL, 0);
}

static bool _attribute_ram_code_sec_ is_valid_image_at_address(u32 addr){
	u32 flashInfo = 0;
	ram_flash_read_page(addr + FLASH_TLNK_FLAG_OFFSET, 4, (u8 *)&flashInfo);
	return (flashInfo == TL_START_UP_FLAG_WHOLE);
}

static u32 _attribute_ram_code_sec_ get_firmware_size(u32 addr){
	u32 size = 0;
	ram_flash_read_page(addr + IMAGE_SIZE_OFFSET, 4, (u8 *)&size);
	return size;
}

static void _attribute_ram_code_sec_ move_flash_data(u32 from, u32 to, u32 size){
	u32 bufCache[256 / 4];
	u8 *buf = (u8 *)bufCache;
	u8  verifyBuf[256];

	for(u32 i = 0; i < size; i += 256){
		if((i & 0xfff) == 0){
			ram_flash_erase_sector(to + i);
		}

		ram_flash_read_page(from + i, 256, buf);

		ram_flash_write_page(to + i, 256, buf);

		ram_flash_read_page(to + i, 256, verifyBuf);
		if(ram_memcmp(verifyBuf, buf, 256)){
			SYSTEM_RESET();  /* verification failed - retry from a clean slate */
		}
	}
}

void _attribute_ram_code_sec_ ensure_correct_ota_scheme(void){
	u32 current_addr = 0x0;
	u32 copy_to_addr = 0x0;
	bool uses_bootloader = is_valid_image_at_address(BOOTLOADER_MODE_MAIN_ADDR);

	if(uses_bootloader){
		current_addr = BOOTLOADER_MODE_MAIN_ADDR;
		copy_to_addr = FLASH_ADDR_OF_OTA_IMAGE;
	}else{
		if(is_valid_image_at_address(SMALL_OTA_FLASH_ADDR)){
			current_addr = SMALL_OTA_FLASH_ADDR;
			copy_to_addr = FLASH_ADDR_OF_OTA_IMAGE;
		}else{
			return;  /* booted from 0x0 or 0x40000 - already correct */
		}
	}

	u32 firmware_size = get_firmware_size(current_addr);
	if(firmware_size == 0 || firmware_size > MAX_FIRMWARE_SIZE){
		SYSTEM_RESET();
	}

	/* order matters for power safety: the new copy is fully written and
	 * verified first, then the magic at 0x0 goes, then our old slot. Any
	 * power cut leaves either the old path or the new one bootable. */
	move_flash_data(current_addr, copy_to_addr, firmware_size);

	u32 unused_flag = 0x0;
	if(uses_bootloader){
		ram_flash_write_page(0x0 + FLASH_TLNK_FLAG_OFFSET, 4, (u8 *)&unused_flag);
		ram_flash_write_page(BOOTLOADER_MODE_MAIN_ADDR + FLASH_TLNK_FLAG_OFFSET, 4, (u8 *)&unused_flag);
	}else{
		ram_flash_write_page(SMALL_OTA_FLASH_ADDR + FLASH_TLNK_FLAG_OFFSET, 4, (u8 *)&unused_flag);
	}
	SYSTEM_RESET();
}

/* ---- boot-mode conversion image: atomic install over our own OTA ----
 *
 * The conversion image runs at 0x8000 under the stock Tuya bootloader.
 * Its OTA client just finished writing a new image to the SDK staging
 * bank (FLASH_ADDR_OF_OTA_IMAGE, 0x70000 on this build - the comment used
 * to say 0x77000, which is the SDK default this project deliberately does
 * not use; see MOES_EDITING_GUIDE S1.2). Installing it by copying
 * over the running image would open a brick window - instead copy it to
 * the 0x40000 hardware bank (safe: neither end is executing), then
 * invalidate the bootloader at 0x0 and this slot at 0x8000 and reboot.
 * The hardware bank scan boots the new image, and every later OTA is an
 * atomic flag flip. If the staged image is invalid, do nothing and stay
 * on the running firmware.
 */
#if MOES_TS0505B && BOOT_LOADER_MODE
/* prototypes instead of including ota.h (its header chain does not like
 * this file's minimal include set) */
extern bool ota_newImageValid(u32 new_image_addr);

void _attribute_ram_code_sec_ moes_otaBankInstall(void){
	if(!ota_newImageValid(FLASH_ADDR_OF_OTA_IMAGE)){
		return;   /* nothing valid staged - keep running */
	}

	u32 fw_size = get_firmware_size(FLASH_ADDR_OF_OTA_IMAGE);
	if(fw_size == 0 || fw_size > MAX_FIRMWARE_SIZE){
		return;
	}

	move_flash_data(FLASH_ADDR_OF_OTA_IMAGE, 0x40000, fw_size);

	u32 unused_flag = 0x0;
	ram_flash_write_page(0x0 + FLASH_TLNK_FLAG_OFFSET, 4, (u8 *)&unused_flag);
	ram_flash_write_page(BOOTLOADER_MODE_MAIN_ADDR + FLASH_TLNK_FLAG_OFFSET, 4, (u8 *)&unused_flag);
	SYSTEM_RESET();   /* boots the new image from 0x40000 */
}
#endif

#endif  /* MOES_NOBOOT_MIGRATION */
