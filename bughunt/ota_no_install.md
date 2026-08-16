# OTA staged at 0x70000, rebooted, bootloader did not install (build 07 → bench unit)

**Verdict up front:** the stock Tuya bootloader's install decision is three checks
and nothing else — `KNLT` magic at `0x70008`, `fw_size @0x70018 <= 0x68000`, and a
Telink `xcrc32` over the whole image minus its trailing 4 bytes. There is **no
version comparison, no image-type check, no alignment check** in the bootloader.
Our build 07 artefact passes all three *on paper* (CRC verified locally). The
failure is therefore **not** "the bootloader rejected build 07 for a version/size
reason". The strongest, evidence-backed explanation is that the install-trigger
flag byte at `0x70008` was `0xff` (not `K`) at the moment of the reboot, so the
bootloader skipped the OTA block entirely; the `0x4b` now seen there was written
*after* that boot, by a flag-write that did not lead to a bootloader run. Two
mechanisms remain in play and are ranked in §4; the parallel hardware agent's
majority-vote CRC of the staged image disambiguates them.

---

## 1. Bootloader accept criteria (exact)

Source of truth: `work/tuyaZigbee/bootloader/bootloader.c` (project copy),
byte-for-byte logic-identical in the install path to the vendored
`build/tl_zigbee_sdk/apps/bootLoader/bootloader.c` (diffed: only `printf`s,
`FLASH_PROTECT_ENABLE` blocks, LED toggles differ). The stock TS0505B bootloader
is this same Telink SDK boot-loader app, Tuya-built, with NV base `0xD8000`
(that is what places the OTA bank at `0x70000`; see §1.4). "Stock == this
source" is **INFERRED** (high confidence) — the stock binary was not fully
re-traced instruction-by-instruction, but the flash layout and behaviour match.

`bootloader_with_ota_check(APP_RUNNING_ADDR, APP_NEW_IMAGE_ADDR)` runs on every
reset. With `APP_RUNNING_ADDR = 0x8000`, `APP_NEW_IMAGE_ADDR = 0x70000`:

1. **Trigger gate — `KNLT` at staging `+8`.**
   `is_valid_fw_bootloader(0x70000)` reads 4 bytes at `0x70008` and requires
   them to equal `0x544c4e4b` (`"KNLT"`). `bootloader.c:122-127`.
   Disassembly (reference build `build/bootloader/bootloader_ZBWS01A.lst`):
   `is_valid_fw_bootloader` @ `0x2690` does `flash_read(addr+8, 4, &flag)`
   (`2698: tadds r0,#8`, `269a: r1=4`, `269e: tjl flash_read`) and compares
   against literal `0xabb3b1b5` @ `0x26b0` — the two's complement of
   `0x544c4e4b`. If this gate is false the whole OTA block is skipped.

2. **Size gate.** `fw_size = *(u32*)(0x70000 + 0x18)`; must be
   `<= FLASH_OTA_IMAGE_MAX_SIZE = 0x68000` (425984 B). `bootloader.c:142-144`.
   Reference disasm: read `+0x18` at `275a`, compare at `2762-2766`.

3. **CRC gate.** `totalLen = fw_size - 4`. `crcVal = *(u32*)(0x70000 +
   fw_size - 4)`. Compute `curCRC = xcrc32(bytes[0x70000 .. 0x70000+totalLen),
   init=0xffffffff)` and require `curCRC == crcVal`. `bootloader.c:145-166`.
   The bootloader CRC loop does **not** substitute the flag byte at `+8` (the
   app-side `ota_newImageValid` does; the bootloader does not). Reference
   disasm: CRC loop at `27a8-280a`, no `+8` patch inside.

`xcrc32` = reflected CRC-32, poly `0xEDB88320`, init passed in (here
`0xffffffff`), **no final XOR**. `proj/common/utility.c:83-92`.

**Explicitly absent from the bootloader:** any read of the version field at
`+2`, any image-type/manufacturer check, any slot-selection scheme, any
alignment/rounding requirement. The "must be newer" logic lives entirely in the
ZCL OTA cluster (server offers only when `fileVer != curFileVer`; client accepts
`!=`; both pass `0x11073003` vs `0x11063003`).

**What it does on accept** (`bootloader.c:170-205`): erase 4 KiB sectors at
`0x8000..`, copy from `0x70000` in 256 B chunks with readback verify, then
clear the staging flag (write `0x00` to `0x70008`) and erase the staging
sectors.

**Clean-up-on-fail (important):** in the SDK source the flag-clear and
staging-erase (`bootloader.c:197-205`) sit *outside* `if(isNewImageValid)`, so
once the `KNLT` gate passes they run even on CRC fail or oversize. Reference
disasm confirms: oversize falls through to the clear/erase block at `2768`
(`2766 tjls 27a8` → not-taken path), and CRC-mismatch jumps to the same clear/
erase (`2826 tjeq 2768`). **INFERRED for the stock binary specifically** — see
§4 caveat.

### 1.4 Where the constants come from

`common/comm_cfg.h`: `BOOT_LOADER_MODE 1`, `APP_IMAGE_ADDR 0x8000`.
`proj/drivers/drv_nv.h` (with `-DMOES_NV_BASE_ADDRESS=0xD8000`, the value this
project back-solved from Tuya's own NV records):

- `FLASH_TLNK_FLAG_OFFSET = 8`
- `FLASH_OTA_IMAGE_MAX_SIZE = (NV_BASE_ADDRESS - FLASH_ADDR_OF_APP_FW)/2
  = (0xD8000 - 0x8000)/2 = 0x68000`
- `FLASH_ADDR_OF_OTA_IMAGE = FLASH_ADDR_OF_APP_FW + max = 0x8000 + 0x68000
  = 0x70000`

This is why the staged image must land at `0x70000` and may be up to 425,984 B.

---

## 2. What our OTA path leaves behind

All addresses/symbols are from the build-07 listing
`build/light/light_TS0505B.lst` and the vendored `zigbee/ota/ota.c`.

Download (`ota_imageDataProcess`):
- Erases staging sectors, writes in ≤48 B blocks (`ota.c:1456-1461`,
  `:1064-1067`).
- When the first block spans `+8`, it checks the incoming byte is `0x4b` and
  then writes `0xff` in its place (`ota.c:1056-1063`). So **during and
  immediately after download, flash `0x70008` is `0xff`** — the "boot flag" is
  deliberately disabled.
- Validates the trailing CRC as it goes, with the `+8` byte forced back to
  `0x4b` for the CRC input (`ota.c:1074-1090`). This compensates for the `0xff`
  that sits in flash.

Completion:
- `tuyaLight_otaProcessMsgHandler` on `OTA_EVT_COMPLETE` + `ZCL_STA_SUCCESS`
  calls `ota_mcuReboot()` (`light/zb_appCb.c:302-324`, call at `:320`).
- `ota_mcuReboot()` (BOOT_LOADER_MODE branch, `ota.c:185-233`): validates
  `0x70000` with `ota_newImageValid()`, then
  `flash_writeWithCheck(0x70008, 1, &0x4b)` to set the flag, then
  `SYSTEM_RESET()`.
- Compiled form confirmed: `ota_mcuReboot` @ `0x1ad40` in the .lst —
  `newAddr = 0x70000` built as `0xe0<<11` (`1ad4c: tmovs r0,#224`,
  `1ad4e: tshftls r0,r0,#11`); `flash_writeWithCheck(0x70008,1,&0x4b)` at
  `1ad5c-1ad62` (literal `0x70008` @ `1ad74`); then reset
  `write_reg8(0x6f,0x20)` (`1ad6a-1ad6e`, literal `0x0080006f` @ `1ad78`).
- `ota_newImageValid` (`ota.c:138-183`) itself reads `fw_size@+0x18`, checks
  `<= FLASH_OTA_IMAGE_MAX_SIZE`, checks `(*(u32*)(buf+8) & 0xffffff00) ==
  0x544c4e00`, and CRCs with `+8` forced to `0x4b`.

**`flash_writeWithCheck` semantics** (`proj/drivers/drv_flash.c:146-177`):
it calls `flash_write_page()` first, *then* reads back and `memcmp`s, returning
`TRUE` only if readback matches. A write can therefore **land in flash even
when the function returns `FALSE`**; `ota_mcuReboot` gates `SYSTEM_RESET()` on
that `TRUE`, so a write-that-lands-but-fails-verify leaves the flag set with
**no reset** (`ota.c:200-231`).

---

## 3. The linchpin observation

Because the bootloader clears `0x70008` and erases the staging bank whenever the
`KNLT` gate passes (even on CRC fail), the observed post-mortem state —

- staging bank intact (build 07 image present),
- flag `0x70008 = 0x4b` (`"KNLT"`),
- app slot `0x8000` still build 06 —

is **inconsistent with the bootloader ever having run with the flag set**. If
it had, the staging bank would now be erased and the flag cleared, regardless of
the CRC result.

Therefore, at the `~14:29:30` reboot the flag at `0x70008` was `0xff`, the
bootloader correctly skipped the install, and the `0x4b` seen now was written
*after* that boot by a flag-write that did not produce a subsequent bootloader
run.

This is reliable despite the SWire read artefacts (see note): the `KNLT` bytes
`4b 4e 4c 54` all have bit 6 already set, so the "bit 6 reads high" artefact the
hardware agent is characterising does not alter them; the `0xff`-vs-`0x4b`
distinction at `+8` is trustworthy. The header bytes at `+1` and `+0xE` are *not*
trustworthy in single reads — `0x80`/`0x88` in the built artefact read back as
`0xc0`/`0xc8`, which is the read artefact, not real flash content.

---

## 4. Hypotheses, ranked

**(b) Install-trigger flag was `0xff` at boot time — most consistent.**
The flag gate failed at the reboot because `ota_mcuReboot()` had not yet run
(its `0x4b` write is the only writer of the flag in our build; the download
only writes `0xff`, the bootloader writes `0x00`, and `moes_otaScheme.c` is
dead code under `MOES_NOBOOT_MIGRATION`). The `0x4b` now present was written
after the last boot and its reset was skipped. Two sub-mechanisms, in order:

1. `flash_writeWithCheck(0x70008,1,&0x4b)` wrote the byte but returned `FALSE`
   (write-then-verify), so `ota_mcuReboot()` skipped `SYSTEM_RESET()`. Directly
   matches "flag set + staging intact + no install".
2. The running build 06 wedged / rebooted before the OTA end-sequence reached
   `ota_mcuReboot()` (the 60 s Upgrade-End countdown is a long exposure to the
   known join-but-silent wedge), so the flag was `0xff` at boot; a later
   NV-recovered OTA-resume path invoked `ota_mcuReboot()` and hit (1).

**(a) Staged CRC/bytes invalid — second, needs the stock-bootloader caveat.**
If the *stock* bootloader does **not** clean up on CRC fail (i.e. differs from
the SDK source on that detail), then "flag set at boot + CRC fail" would leave
exactly the observed state without needing a reset-skip. This is being verified
by the parallel hardware agent (majority-vote read of `0x70000..`, then CRC vs
the `.zigbee` payload). **Do not duplicate; dependency noted.** Our built
artefact's own CRC is valid locally: `xcrc32(light_TS0505B.bin[0:202144]) ==
0x18faf087` == the stored trailing word, and `fw_size = 202148` == file size.

**(e) Reboot before end-sequence completed — real, but a trigger for (b), not a
separate root cause.** The `14:27` download-complete → `14:29:30` reboot gap is
roughly the `Upgrade-End` handshake + 60 s countdown; if build 06 wedged in that
window, the flag never got written before the reboot.

**(c) Bootloader requires strictly-newer version — REFUTED.** The bootloader
reads no version field; `+2` is only ever read by the ZCL OTA cluster, whose
comparison is `!=`, and `0x11073003 != 0x11063003` passes.

**(d) Size rounding / alignment — REFUTED.** `202148 <= 425984`; the bootloader
copies arbitrary lengths in 256 B chunks and erases 4 KiB sectors, with no
alignment precondition beyond the CRC covering exactly `fw_size-4` bytes.

---

## 5. Minimal-patch sketch (firmware-side, if the flag/reset race is confirmed)

Target file `build/tl_zigbee_sdk/zigbee/ota/ota.c`, `ota_mcuReboot()`
(BOOT_LOADER_MODE branch). Do **not** touch the bootloader or flash layout.

```c
// current (fragile):
if(flash_writeWithCheck(newAddr + FLASH_TLNK_FLAG_OFFSET, 1, &flashInfo) == TRUE){
    reboot = 1;
}
...
if(reboot){ SYSTEM_RESET(); }

// hardened:
flash_write(newAddr + FLASH_TLNK_FLAG_OFFSET, 1, &flashInfo); // set KNLT 'K'
SYSTEM_RESET();                                               // always reboot
```

Rationale: the flag byte write is the *only* install trigger the stock
bootloader honours, and the current code makes the subsequent reset conditional
on a read-back verify that can fail after the write has already landed. Always
resetting after the write removes the "flag set, staging intact, no install"
failure mode. Optionally keep a single read-back to log/debug, but never gate
the reset on it.

Secondary hardening (larger window, worth scheduling separately): trigger the
install earlier in `ota_upgrade()` / the OTA callback — e.g. call
`ota_mcuReboot()` on `DOWNLOAD_COMPLETE` (or `OTA_EVT_IMAGE_DONE`) instead of
waiting out the 60 s Upgrade-End countdown, so the wedge has less time to
pre-empt the flag write. Keep the Upgrade-End Req/Rsp for protocol compliance,
but do not make the install depend on it.

---

## 6. What is verified vs inferred

**Verified (local, this agent):**
- Bootloader accept criteria (§1) from project+SDK bootloader source and the
  reference `bootloader_ZBWS01A.lst` disassembly.
- Constants `0x70000` / `0x68000` / `FLAG=8` from `drv_nv.h` + `comm_cfg.h`.
- Our build 07 `.zigbee` payload == `light_TS0505B.bin`; `fw_size=202148`;
  trailing CRC `0x18faf087` matches `xcrc32(payload[0:202144])`.
- Our `ota_mcuReboot` compiled form writes `0x4b` to `0x70008` then resets
  (`write_reg8(0x6f,0x20)`); `flash_writeWithCheck` is write-then-verify.
- Bootloader has no version/image-type/alignment check.

**Inferred (needs the noted confirmation):**
- "Stock TS0505B bootloader == this SDK source", including the
  clear-flag-and-erase-staging-on-CRC-fail behaviour. Byte-level confirmation of
  the stock binary's clean-up-on-fail path would definitively close the
  (a)-vs-(b) choice.
- The exact micro-cause of "flag written but no subsequent bootloader run"
  (write-then-verify `FALSE` after landing is the best fit, not yet observed
  directly on hardware).

**Dependency:** the staged-image byte-level CRC verification is with the
parallel hardware agent (majority-vote over 3 passes to defeat the bit5-random /
bit6-deterministic read artefacts). If that CRC comes back **valid**, hypothesis
(a) is dead and the flag/reset race (§4b, §5) is the cause. If it comes back
**invalid**, then either the staged bytes are genuinely corrupted or the stock
bootloader does not clean up on CRC-fail — the latter would need the stock
binary's clean-up path checked.
