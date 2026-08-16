# Build 06 watchdog design (DESIGN ONLY — nothing shipped)

**Scope:** `FALLBACK_DESIGN.md` §5.2, turned into an exact, reviewable change
set for build 06. **No file is modified by this document.** The diffs below
are presented for approval; they are not committed.

**Bottom line:** the 600 ms interval is correct, and it is *only* correct
because the SDK already services the watchdog at the top of every high-level
flash operation. The one place it does not is `ota_newImageValid()`'s CRC
loop, which is also the most dangerous place in the device's life. Fix that
loop and enable the watchdog in the same change; nothing else in the compiled
tree needs a new `drv_wd_clear()` site.

---

## 1. `ota_newImageValid()` CRC loop

**File/line:** `build/tl_zigbee_sdk/zigbee/ota/ota.c:138` (function),
`:161-172` (the loop). Caller chain:

```
main()                                   apps/common/main.c:85
  ev_main()
    -> OTA_EVT_COMPLETE handler
       tuyaLight_otaProcessMsgHandler()  light/zb_appCb.c:286
         ota_mcuReboot()                 ota.c:182
           ota_newImageValid()           ota.c:189   (BOOT_LOADER_MODE)
```

### 1.1 It CRCs the whole image in one call, with no watchdog service

Confirmed by reading the function. It:

1. reads the first 256 bytes (`ota.c:142`) and takes `fw_size` from the OTA
   header at `+0x18` (`ota.c:143`);
2. `totalLen = fw_size - 4` (`ota.c:146`) — the trailing 4 bytes are the CRC,
   excluded from the CRC input;
3. reads the stored CRC from `new_image_addr + fw_size - 4` (`ota.c:152`);
4. validates the `KNLT`/flag field (`ota.c:154-157`);
5. loops over `totalLen` in 256-byte chunks (`ota.c:161-172`), calling
   `flash_read()` and `xcrc32()` per chunk, substituting `TL_IMAGE_START_FLAG`
   at `FLASH_TLNK_FLAG_OFFSET` (8) in the first chunk.

There is **no `drv_wd_clear()` anywhere in `ota.c`** (grep-verified; the only
watchdog service in the SDK is in `drv_flash.c` and `main.c`). The loop does
not return to the main loop, so the two `drv_wd_clear()` calls in
`main()` (`apps/common/main.c:88,94`) do **not** run during it.

### 1.2 Runtime estimate

For build 04 the image is 201,476 B, so `totalLen = 201,472 B` →
`ceil(201472/256) = 787` iterations, plus the two pre-loop reads.

Per 256-byte iteration:

| component | cost | basis |
|---|---|---|
| `flash_read` 256 B | ~130–250 µs | auto-mode MSPI read; 256 bytes at the flash's SPI clock (see below), plus command/address/`mspi_wait` overhead |
| `xcrc32` 256 B | ~45–55 µs | table-driven CRC in `proj/common/utility.c:83-92`, ~8–10 cycles/byte at 48 MHz |
| loop overhead | small | — |
| **per iteration** | **~180–300 µs** | |

**Total: roughly 160–240 ms** for build 04, with ~200 ms as a fair central
estimate. This is slightly higher than the "100–150 ms" figure in
`FALLBACK_DESIGN.md` §5.2 (that figure under-counted the flash-read cost), but
still under the 600 ms interval *for today's image*.

The decisive point is headroom. The legal maximum image is
`FLASH_OTA_IMAGE_MAX_SIZE = 0x68000 = 425,984 B` (`proj/drivers/drv_nv.h:241`,
with `NV_BASE_ADDRESS 0xD8000`). At that size `totalLen = 425,980 B` →
**1,664 iterations**, i.e. roughly **330–500 ms**. That is already brushing
against the real interval (≈595 ms, see §2), and any jitter, a slower flash
part, or future image growth pushes it over. "Probably survives" is not the
standard for the moment a device commits to a new image.

> Note on the MSPI clock: the flash SPI clock is not a compile-time constant in
> the audited sources; it is configured by the prebuilt clock/boot path. The
> estimate above deliberately brackets a slow flash interface. Even at a very
> pessimistic effective 8 MHz, a 256-byte read is ~256 µs, and the max-size
> image lands near the interval. The conclusion — fix the loop — does not
> depend on the exact clock.

### 1.3 Where `drv_wd_clear()` belongs

Once per loop iteration, before the `flash_read`:

```c
while(totalLen > 0){
    wLen = (totalLen > 256) ? 256 : totalLen;
#if (MODULE_WATCHDOG_ENABLE)
    drv_wd_clear();
#endif
    flash_read(sAddr, wLen, buf);
    ...
}
```

One clear per 256-byte chunk keeps the maximum unserviced window to
~one flash read + one CRC ≈ well under 1 ms, and the cost is a single register
write per 256 bytes (negligible). The guard keeps the patch inert when the
watchdog is off, so the existing build 05 / known-good behaviour is unchanged
until `MODULE_WATCHDOG_ENABLE 1` lands with it.

`drv_wd_clear()` is already visible in `ota.c` via
`zb_common.h -> tl_common.h -> drivers/drv_hw.h` (`proj/tl_common.h:50`), so
no new `#include` is needed.

---

## 2. The case for 600 ms

### 2.1 Wiring and actual interval

- `MODULE_WATCHDOG_ENABLE` is `0` at `light/app_cfg.h:76`.
- When enabled, `main()` (`apps/common/main.c:64-67`) starts the watchdog
  **after `user_init()` returns and after `drv_enable_irq()`**:

```c
#if (MODULE_WATCHDOG_ENABLE)
    drv_wd_setInterval(600);
    drv_wd_start();
#endif
```

- It is cleared twice per main-loop iteration (`main.c:87-95`): after
  `ev_main()` and after `tl_zbTaskProcedure()`.
- `drv_wd_setInterval` → `wd_set_interval_ms(600)` in
  `platform/chip_8258/watchdog.h:38-45`:

```c
tmp_period_ms = (period_ms*1000*system_clk_mHz>>18);
```

For this target `CLOCK_SYS_CLOCK_HZ 48000000` (`common/comm_cfg.h:180`,
`MCU_CORE_8258`), so `system_clk_mHz = 48`:

```
600 * 1000 * 48 = 28,800,000
28,800,000 >> 18 = 109          (integer truncation)
actual interval = 109 * 2^18 / 48e6  ≈ 595.3 ms
```

So "600 ms" is really **≈595 ms**. The argument below keeps ≥ ~195 ms of
margin against the worst legitimate single flash operation.

### 2.2 The worst legitimate IRQ-off section is one sector erase

The IRQ-off sections in the compiled tree are all inside
`flash_mspi_write_ram()` (`platform/chip_8258/flash.c:144-177`):

```c
unsigned char r = irq_disable();          // :146
...
flash_send_cmd(FLASH_WRITE_ENABLE_CMD);   // :149
flash_send_cmd(cmd);                      // :150
...
flash_send_addr(addr);                    // :153-164
...write data...
mspi_high();                              // :171
flash_wait_done();                        // :172   <-- IRQs still off
irq_restore(r);                           // :174
```

`flash_wait_done()` (`flash.c:82-94`) does `sleep_us(100)`, sends a
read-status command, then polls the busy bit until it clears. The duration is
dominated by the **hardware** erase/program time of the external flash, not by
the poll loop.

The longest such operation is a 4 KB sector erase
(`flash_erase_sector` → `FLASH_SECT_ERASE_CMD 0x20`, `flash.c:193-196`).
The SDK's 1 MB flash MID table (`flash.c:490-506`) names the candidate parts
for a 1 MB device:

| part | MID | 4 KB sector erase (vendor datasheet, typical→max) |
|---|---|---|
| ZB25WD80B | `0x14325E` | ~40 ms → ≤ 100 ms |
| GD25LD80C | `0x1460C8` | ~60 ms → ≤ 300 ms |
| GD25LE80C | `0x011460C8` | ~40 ms → ≤ 100 ms |

(These are the external-flash datasheet values, not constants in the SDK
headers; the SDK headers supply the command set and the 10M-poll software
bound, not the silicon timing. Confirm the specific part on the bench unit via
JEDEC `0x9F` before the first watchdog-enabled flash — see §6.)

Even the most pessimistic 4 KB sector erase above is ≤ 300 ms, and the parts'
maximums sit comfortably below the ≈595 ms interval. A page program is far
smaller (typ ~0.7–3 ms, max ≤ 5 ms for these parts).

### 2.3 Why the interval does not need to cover a whole erase *loop*

The key structural fact: the SDK's **high-level** flash entry points already
service the watchdog immediately before the operation when it is enabled:

- `flash_write()` — `proj/drivers/drv_flash.c:134-137`
- `flash_writeWithCheck()` — `drv_flash.c:154-157`
- `flash_erase()` — `drv_flash.c:184-187`

```c
#if (MODULE_WATCHDOG_ENABLE)
    drv_wd_clear();
#endif
```

Every multi-sector loop in the compiled tree goes through these wrappers one
operation at a time (see §4), so the 595 ms window restarts before each
sector. The interval therefore only has to bound **one** flash transaction,
and one sector erase is the largest transaction.

### 2.4 The 10,000,000-iteration poll bound is not a false-trip argument

`flash_wait_done()` exits the poll loop after at most 10,000,000 iterations
(`flash.c:88`). If the flash were stuck busy, that software bound is seconds
of IRQ-off and a watchdog reset would fire. That is the **desired** behaviour
for a stuck flash — it is a hang, not a legitimate long section — so it does
not argue for a longer interval. It argues that the watchdog should be on.

### 2.5 Why 600 ms specifically is the right number

- It must be **> worst legitimate IRQ-off section**: worst 4 KB sector erase
  ≤ 300 ms (conservatively ≤ 400 ms for any part in the MID table), leaving
  ≥ ~195 ms margin against the ≈595 ms actual interval.
- It must be **short enough that a hang is converted into recoverable resets
  quickly**: a hung device resets ~every 600 ms + boot time. Rescue mode
  latches on boot 7 (6 unstable boots), i.e. well within about a minute, and
  then serves OTA every 10 minutes. Short intervals matter here.
- 600 ms also comfortably bounds every other legitimate section found in §4.
- The value is already hard-coded in `main.c:65`; no interval change is needed.

---

## 3. Why the enable and the CRC fix must be the same change

The watchdog has exactly one job on this device: turn a **hang** (which rescue
mode cannot see, because rescue counts reboots) into a **reset loop** that
rescue mode can count and then escape by OTA.

That chain only works if the OTA path itself survives watchdog supervision.
`ota_newImageValid()` runs at the single most dangerous moment in the device's
life: the image is fully staged at `0x70000`, and `ota_mcuReboot()` is about to
write the one flag byte that arms the bootloader. The sequence is:

```
ota_newImageValid(newAddr)          ota.c:189   <- CRC loop, no service
flash_writeWithCheck(flag at +8)    ota.c:197   <- 1 byte, wrapper clears WD
SYSTEM_RESET()                      ota.c:228
```

If `MODULE_WATCHDOG_ENABLE 1` shipped **without** the CRC-loop fix:

1. Today's 200 KB image (~160–240 ms) would *probably* pass, but the maximum
   legal image (425,984 B) is ~330–500 ms — within jitter of the ≈595 ms
   interval, and over it with any growth.
2. When the loop exceeds the interval, the watchdog resets the device **during
   validation**, before the flag write. No half-install occurs (the CRC loop is
   read-only), but the device reboots into the **old** image with the staged
   image still present.
3. The OTA then retries and hits the same reset, forever. The one mechanism
   that was supposed to deliver the fix cannot complete its commit step.

That is the exact worst outcome the whole fallback design exists to prevent: a
device that resets (so it is *not* a clean hang) but can never accept the OTA
that would fix it. Enabling the watchdog without first making the commit path
watchdog-safe trades a hang for a *different* unrecoverable state.

This is already encoded as a known-good rule — `AI_BUGHUNT_BRIEF.md` §6:
"`MODULE_WATCHDOG_ENABLE 0` in the shipped builds until §5.2's precondition
lands with it." The precondition is the §1.3 patch. They are one commit.

Additional detail: `tuyaLight_otaProcessMsgHandler()` calls
`moes_resetSkipNextBoot()` (the deliberate-reboot mark) *before*
`ota_mcuReboot()` (`light/zb_appCb.c:270-286`). A watchdog reset during the
CRC loop would consume that mark, but that is harmless — the old image keeps
running and the OTA retries; the mark only shields the factory-reset counter,
and the next real install marks itself again.

---

## 4. Every other long legitimate section, and where a clear belongs

The survey covered flash operations, NV rotation/init, the OTA write/erase
loops, and the boot path.

### 4.1 OTA download pre-erase loop — already safe

`build/tl_zigbee_sdk/zigbee/ota/ota.c:1453-1458`:

```c
u16 sectorNumUsed = g_otaCtx.downloadImageSize / FLASH_SECTOR_SIZE + 1;
for(u16 i = 0; i < sectorNumUsed; i++){
    flash_erase(baseAddr + i * FLASH_SECTOR_SIZE);
}
```

For a 200 KB image that is 51 sector erases. Each call is to the **wrapper**
`flash_erase` (`drv_flash.c:184`), which clears the watchdog before the
hardware erase. The total loop is seconds, but the watchdog sees only one
≤300 ms erase at a time. **No new site needed.** (A future edit that switches
this to raw `flash_erase_sector()` must add the clear back.)

### 4.2 OTA block write — already safe

`ota.c:1053-1064`: each image-block response writes ≤48 bytes via
`flash_writeWithCheck` (`drv_flash.c:146`), which clears the watchdog at its
start. Per-block time is a page program, ~ms. **No new site needed.**

### 4.3 NV sector rotation — already safe

`proj/drivers/drv_nv.c`:

- `nv_flashWriteNewHandler()` erase loops at `:585-586` (fresh module) and
  `:628-634` (`forceChgSec` / sector full) call `flash_erase` once per sector.
- The valid-item copy loop `:650-708` calls `nv_write_item(...)` with
  `isFlashCopy=TRUE`, which copies in 48-byte chunks via `flash_writeWithCheck`
  (`drv_nv.c:328`) — each chunk clears the watchdog.
- The non-copy branch writes the item payload in one `flash_write(payloadAddr,
  len, buf)` call (`drv_nv.c:355-356`). The largest item in `NV_MODULE_APP` is
  bounded by the 4 KB sector minus headers (`drv_nv.c:577`,
  `MODULE_INFO_SIZE(id)`), i.e. ≤ ~3.5 KB → ≤ ~14 page programs, each a
  separate IRQ-off transaction inside `flash_write_page`. Even at a pessimistic
  5 ms per page that is ≤ ~75 ms, and `flash_write` clears the watchdog once at
  its start (`drv_flash.c:134-137`). Safe.

**No new site needed.** The one thing worth recording: `flash_write` clears
the watchdog *once* for the whole multi-page call. That is fine for every
current caller (max ~3.5 KB). A future caller that writes a very large buffer
through one `flash_write` call would need to chunk at the call site.

### 4.4 NV reset / factory reset — already safe

`nv_resetModule()` / `nv_resetAll()` (`drv_nv.c:1018-1037`) erase every module
sector via `flash_erase`, so per-sector service. Additionally, the watchdog is
started **after** `user_init()` returns (`main.c:59-67`), and `nv_init()` runs
inside `stack_init()`/`zb_init()` — so the boot-time NV reset path is not even
under watchdog supervision yet. **No new site needed.**

### 4.5 Boot / join path — outside watchdog coverage, by design

`drv_platform_init()`, `stack_init()`, `user_init()`, `user_app_init()`, and
`bdb_init()` all run before `drv_wd_start()` (`main.c:48-67`). That is the
right choice for this device: a long join/steer or NV init on the boot path
cannot false-trip the watchdog, at the cost of leaving pre-`main()` hangs
uncovered. That residual gap is already documented (`FALLBACK_DESIGN.md` §4.1)
and is orthogonal to this change.

### 4.6 `moes_otaScheme.c` migration — NOT compiled, but a future trap

`light/moes_otaScheme.c` is entirely behind `MOES_NOBOOT_MIGRATION` (undefined,
`AUDIT_FINDINGS.md` A-7). Its `move_flash_data()` and install routines use
their **own** `ram_flash_erase_sector()` / `ram_flash_write_page()` copies
(`moes_otaScheme.c:73-116`) with **no** `drv_wd_clear()` and a full bank copy
that would certainly exceed 600 ms. If `MOES_NOBOOT_MIGRATION` is ever enabled,
each per-sector step in those routines must call `drv_wd_clear()` — and the
whole migration still needs the other prerequisites in `FALLBACK_DESIGN.md` §6.

### 4.7 Everything else

The 25 fps effect timer, ZCL command handlers, reporting sanitizer, and
factory-reset timer are CPU-bound and were already bounded elsewhere
(`AUDIT_FINDINGS.md`); none approaches 600 ms of unserviced time. No new
sites.

### Summary of proposed `drv_wd_clear()` sites

| site | action |
|---|---|
| `ota.c` CRC loop (`:161-172`) | **add** — the one required new site |
| `drv_flash.c` `flash_write` / `flash_writeWithCheck` / `flash_erase` | keep as-is — these are what make 600 ms safe |
| `drv_nv.c` erase/copy loops | none — they already route through the wrappers |
| `ota.c` download erase/write loops | none — already route through the wrappers |
| `moes_otaScheme.c` (if ever enabled) | add per-sector clears before enabling |

---

## 5. The exact diff (presented, NOT applied)

Two code files plus one documentation table. Nothing here is committed by this
workstream.

### 5.1 Vendored SDK — service the CRC loop

```diff
--- a/build/tl_zigbee_sdk/zigbee/ota/ota.c
+++ b/build/tl_zigbee_sdk/zigbee/ota/ota.c
@@ -161,6 +161,9 @@ bool ota_newImageValid(u32 new_image_addr){
 
 		while(totalLen > 0){
 			wLen = (totalLen > 256) ? 256 : totalLen;
+#if (MODULE_WATCHDOG_ENABLE)
+			drv_wd_clear();
+#endif
 			flash_read(sAddr, wLen, buf);
 			if(oft == 0){
 				buf[FLASH_TLNK_FLAG_OFFSET] = TL_IMAGE_START_FLAG;
```

### 5.2 Enable the watchdog

```diff
--- a/light/app_cfg.h
+++ b/light/app_cfg.h
@@ -75,7 +75,7 @@
 /* Watch dog module */
-#define MODULE_WATCHDOG_ENABLE						0
+#define MODULE_WATCHDOG_ENABLE						1
```

`main.c` needs no edit — the interval (`600`) and both clear sites are already
wired and active under this macro.

### 5.3 Record the vendored-SDK patch (per `MOES_EDITING_GUIDE.md` §2)

```diff
--- a/MOES_EDITING_GUIDE.md
+++ b/MOES_EDITING_GUIDE.md
@@  vendored SDK patches table
 | `zigbee/mac/mac_pib.c` | read IEEE from Tuya block | every light changes MAC |
 | `proj/drivers/drv_nv.h` | honour `MOES_NV_BASE_ADDRESS` | NV eats the IEEE, OTA bank moves |
 | `apps/common/main.c` | `MOES_NOBOOT_MIGRATION` hook | harmless today (feature is off) |
+| `zigbee/ota/ota.c` | `drv_wd_clear()` in `ota_newImageValid()` CRC loop | OTA commit path can watchdog-reset during validation |
```

### 5.4 Commit plan (for the human / next agent, not performed here)

One commit on `moes-ts0505b`, e.g.:

> build 06: enable hardware watchdog with OTA CRC-loop service

containing the two code diffs and the guide-table row together. Do **not**
split them: the known-good table (`AI_BUGHUNT_BRIEF.md` §6) explicitly forbids
`MODULE_WATCHDOG_ENABLE 1` without the `ota_newImageValid()` fix in the same
change.

---

## 6. Pre-flight checks before this ever ships

1. Build clean (two known-benign `zcl_colorCtrlCb.c` warnings only).
2. `tools/rescue_hosttest && make check` — 15/15 (the watchdog itself does not
   change the rescue state machine, but the image must still pass it).
3. Confirm the exact flash part on the bench unit via JEDEC `0x9F` and verify
   its 4 KB sector-erase maximum is < 595 ms. The §2.2 table covers the parts
   the SDK knows; the actual board should be recorded in
   `REBUILD_NOTES.md`.
4. Bench soak (`OTA_TEST_PLAN.md`) must include: (a) a full OTA install with
   the watchdog enabled, (b) a deliberately hung build to confirm
   hang → reset → rescue latch → OTA recovery, and (c) 24 h of normal
   operation with no watchdog reset.
5. `APP_BUILD` bump in `common/version.h`.

---

## 7. Evidence / SRAM-dump expectations for the long sections (for the incident file)

None of the sections in this document caused the build 04 hang: build 04 had
`MODULE_WATCHDOG_ENABLE 0`, and `ota_newImageValid()` did not execute at all on
build 04 (build 04 was installed by the **stock** firmware's OTA code; our
`ota_newImageValid()` only runs when *our* firmware installs a *future*
update). They are future-hang/recovery candidates, so the mapping below is for
completeness, not a claim about 2026-08-15.

| candidate (file:line) | causal chain if watchdog were on / future | 7-point evidence (brief §2.1) | what the SRAM dump would show if it were the hang |
|---|---|---|---|
| `ota.c:161-172` CRC loop | unserviced validation exceeds interval → watchdog reset mid-`ota_mcuReboot` → OTA never commits | supports none of the build 04 points (did not run on build 04); explains a *future* "resets but never installs" failure | PC inside `ota_newImageValid`/`xcrc32`/`flash_mspi_read_ram`, stack showing `ota_mcuReboot` ← `tuyaLight_otaProcessMsgHandler`; flash `0x70000` fully written, flag byte at `0x70008` still erased |
| `ota.c:1453-1458` pre-erase loop | if a future edit bypassed the `flash_erase` wrapper, a >595 ms unserviced erase loop | n/a future | PC in `flash_mspi_write_ram`/`flash_wait_done`, stack in `ota_queryNextImageRspHandler` path |
| `drv_nv.c:585-586/628-634` sector-rotation erase | same wrapper-bypass scenario | build 04 configure did exercise NV writes (evidence #1), so L2 already owns this; watchdog was off, so no reset expected | PC in `flash_wait_done`; stack `nv_flashWriteNewHandler` ← `nv_flashWriteNew` ← ZCL/APS save; torn sector at `0xD8000-0xEE000` |
| `drv_nv.c:355-356` large item write | only if a future item grew to many KB and `flash_write` were not chunked | n/a future | PC in `flash_write_page` loop; stack in `nv_write_item` |

The honest line: for the 2026-08-15 hang itself, this document does **not**
identify the cause — that is the hang-hunt workstream (`HANG_FINDINGS.md`).
This document's job is to make the *next* hang recoverable, and it does so
only if §5.1 and §5.2 ship together.
