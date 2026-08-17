# Editing this firmware without bricking 46 ceiling lights

Read this before changing anything under `light/`, `device_config/`,
`common/`, or the vendored SDK in `build/tl_zigbee_sdk/`.

You are editing firmware for **46 mains-wired downlights installed in
ceilings**. They are updated **only** over the air. There is no working
wired recovery: SWire writes are broken on this silicon (§6), so a light
that stops accepting OTA has to be physically removed from the ceiling and
opened up. Assume every mistake is a ladder.

The dangerous mistakes here are **not** the obvious ones. A wrong colour
curve just looks bad and you fix it next update. The mistakes that hurt are
the ones that silently destroy the *ability to update*, or destroy
*per-device identity*, and nothing warns you — the build succeeds, the
image is byte-valid, and the damage only appears after it is deployed.

---

## 0. The rule that actually cost us a light

**Nothing in `user_init()` may touch NV or schedule a Zigbee timer before
`stack_init()` returns.**

`nv_init()` is not called anywhere in the SDK sources — it lives inside the
prebuilt `libzb_router.a` and runs from `zb_init()`, which `stack_init()`
calls. Anything earlier is operating on an uninitialized NV subsystem.

We enabled `factoryRst_init()` (for the 3-power-cycle pairing gesture) and
placed it *before* `stack_init()`. It restores a power-cycle counter from
NV, writes it back, and schedules a `TL_ZB_TIMER`. Reading uninitialized NV
returned a garbage count `>= 3`, so about two seconds into **every** boot
`factoryRst_handler()` called `zb_factoryReset()` — the firmware reset
itself, forever, roughly every 11 seconds.

The device still booted, joined and interviewed correctly in between, which
made it look healthy at first glance. But an OTA needs ~30 minutes of
continuous uptime, so **the light could not be fixed over the air** and had
to come out of the ceiling.

Two defences are now in place; keep both:

1. `factoryRst_init()` is called after `stack_init()` / `user_app_init()`.
2. `factory_reset.c` clamps the restored counter — a value above the
   threshold is never legitimate, so garbage can no longer trigger a reset.

**Generalise the lesson:** the fatal class of bug here is anything that
makes the device unable to stay up long enough to receive an OTA. Boot
loops, watchdog resets, exception-handler resets, and anything that calls
`zb_factoryReset()` or `SYSTEM_RESET()` on a timer are all in that class.
Treat them as more dangerous than any wrong colour or broken effect.

### 0.1 This board has no button, and the key scanner must never run

`HAVE_NET_BUTTON 0` in `device_config/light_ts0505b.h` is now **load-bearing**.
It gates `app_key_handler()` in `light/app_ui.c` and its call site in
`app_task()` (`light/tuyaLight.c`). Do not remove either guard, and do not set
the macro to 1.

Until 2026-08-15 the macro was defined in four board headers and **read
nowhere**, so the scanner ran on every board including this one. Here is what
that does on a board with no buttons:

`KB_SCAN_PINS {GPIO_PC0, GPIO_PD4}` is declared without the matching
`PC0_INPUT_ENABLE 1` / `PULL_WAKEUP_SRC_PC0 PM_PIN_PULLUP_10K` that every other
board file in this tree pairs with it. The `gpio_default.h` fallbacks then
leave both pads with the input buffer disabled and no pull, so
`gpio_read_all()` returns 0 for them — and `kb_key_pressed()` treats LOW as
*pressed* (`KB_LINE_HIGH_VALID` is 0). The scanner therefore sees VK_SW1 held
from the second poll after boot, and five seconds later
`buttonKeepPressed(VK_SW1)` calls **`zb_factoryReset()`**. Reboot. Repeat,
every ~5–7 seconds, forever.

This was a second, independent cause of exactly the failure that cost a
fixture, still live in the tree after the v1.2 fix. If you ever want the
buttons back, you must add the pin configuration **and** set the macro —
both, never one.

### 0.2 Rescue mode

`light/moes_rescue.{c,h}` counts consecutive boots that never reached a stable
state. The first six such boots park the counter at the threshold
(`MOES_RESCUE_FAIL_THRESHOLD`, 6); the seventh latches rescue mode and brings
the light up doing nothing but joining the network and servicing the OTA
cluster. **Do not remove it, and do not add work to the rescue path.** It is the only thing standing between an application bug and a
ladder. Full design, failure modes and test plan in `FALLBACK_DESIGN.md`;
`tools/rescue_hosttest/` exercises the state machine on a PC (`make check`).

The hooks are small and easy to delete by accident. They are:

| file | hook |
|---|---|
| `light/tuyaLight.c` | `moes_rescueBootCheck()` right after `stack_init()`; skips `factoryRst_init()`, `light_adjust()`, `factoryRst_handler()`, `tuyaLightAttrsChk()` |
| `light/tuyaLightCtrl.c` | `light_fresh()` and `light_blink_start()` return immediately |
| `light/light_effects.c` | `lightFx_start()` refuses anything but `MOES_EF_STEADY` |
| `light/zcl_tuyaMfg.c` | the `0xEF00` command handler rejects everything |
| `light/zb_appCb.c` | starts the stable clock on join; shortens the OTA poll |
| `common/factory_reset.c` | `moes_rescueClear()` when the 3-power-cycle gesture completes |

Two invariants to preserve if you touch it:

* the probation write must stay **after `stack_init()`** (§0), and
* a boot that *latches* rescue mode must **not** write NV — that is what bounds
  flash wear in a permanent loop to a total of six writes.

## 1. The four invariants. Break one and the fleet is unrecoverable.

### 1.1 NV must never reach 0xF8000

`-DMOES_NV_BASE_ADDRESS=0xD8000` in `light/CMakeLists.txt`.

The Telink SDK's default for bootloader mode is `0xE6000`, which puts the
keypair NV module at `0xF4000–0xFC000`, directly **on top of**:

* `0x0F8000` — Tuya's board-config JSON (per model)
* `0x0FB000` — Tuya's identity block, containing this light's **IEEE
  address**, which is unique per unit and exists nowhere else

Storing link keys — which happens the first time the light joins a network —
would erase it. The light loses its MAC address permanently. You cannot
restore it: it was never backed up for 45 of the 46 units.

**If you change `NV_BASE_ADDRESS`, or the flash-capacity macros, or
`BOOT_LOADER_MODE`, recompute the NV span by hand and prove it ends at or
below `0xF8000`.** The span is
`NV_BASE + 0x1000*14` … `+ 0x1000*4*2` (keypair is 4 sectors, double
buffered). There is no compile-time check. Add one if you touch this.

### 1.2 The OTA staging bank must stay at 0x70000

This is not a free choice. The **stock Tuya bootloader** — which we keep and
rely on — reads the staged image from `0x70000`. The SDK computes the bank
as `APP + (NV_BASE - APP)/2`, so `NV_BASE = 0xD8000` yields exactly
`0x70000`. That is why that specific value is used; it is not arbitrary.

The SDK default `0xE6000` yields `0x77000`. An image built that way installs
correctly *once* (the stock firmware stages it where the stock bootloader
looks) and then **can never be updated again**, because our firmware would
stage future images at `0x77000` where the bootloader never looks. This is
the exact bug that made the upstream project's firmware a one-way trip
(doctor64/tuyaZigbee#23).

Changing `NV_BASE_ADDRESS` silently changes the OTA bank. They are coupled.

### 1.3 Never write to, or erase, 0x0F8000 / 0x0FB000 / the bootloader

* `0x000000–0x008000` — stock Tuya bootloader. **Keep it.** It is what makes
  OTA installs work. We deliberately do not replace it.
* `0x0F8000` — board config.
* `0x0FB000` — identity: Tuya product id, a device secret, and the ASCII
  EUI-64 the radio actually uses.

`moes_flashcfg.c` reads `0x0FB000` and nothing writes above `0xEE000`. Keep
it that way. If you add any `flash_write`/`flash_erase` with a computed
address, bound-check it.

### 1.4 The device identity strings must match exactly

`device_config/light_ts0505b.h`:

```c
#define ZCL_BASIC_MODEL_ID  {7,'T','S','0','5','0','5','B'}
#define ZCL_BASIC_MFG_NAME  {16,'_','T','Z','3','2','1','0','_','b','8','j','d','o','s','x','o'}
```

These are **length-prefixed Zigbee strings** — the first byte must equal the
number of characters that follow. This bit us: the prefix said 17 for a
16-character name, which appends a garbage byte, breaks zigbee2mqtt's
fingerprint match on `manufacturerName`, and makes the light show up as an
*unsupported device* — which means **no external converter and therefore no
OTA**. A light that cannot be offered an OTA cannot be fixed remotely.

Count the characters. Then count them again.

Keeping these strings identical to stock is also what preserves the z2m
device entry, friendly name and Home Assistant entities across conversion.
Change them and every light re-appears as a new device.

---

## 2. The identity chain (why the light keeps its MAC)

The radio's IEEE does **not** come from the conventional Telink location.

* `0x0FF000` — the usual Telink MAC block. On these boards it holds a value
  that matches no valid format. **It is not the live address.**
* `0x0FB000 + 0x58` — an ASCII EUI-64, e.g. `a4c138…eccd`. **This is
  what the radio uses.** Proven: a converted light joined with exactly this
  value.

`build/tl_zigbee_sdk/zigbee/mac/mac_pib.c` is patched (guarded by
`MOES_TS0505B`) to read it via `moes_flashGetIeee()`. **If you re-download
or update the vendored SDK, this patch is lost**, every light gets a
different randomly-derived MAC, and all 46 appear as brand-new devices in
zigbee2mqtt with dead history and broken automations.

Vendored SDK patches that must survive an SDK update:

| File | Patch | Lose it and… |
|---|---|---|
| `zigbee/mac/mac_pib.c` | read IEEE from Tuya block | every light changes MAC |
| `proj/drivers/drv_nv.h` | honour `MOES_NV_BASE_ADDRESS` | NV eats the IEEE, OTA bank moves |
| `apps/common/main.c` | `MOES_NOBOOT_MIGRATION` hook | harmless today (feature is off) |
| `zigbee/ota/ota.c` | `drv_wd_clear()` inside `ota_newImageValid()` CRC loop | the OTA validation CRC loop can watchdog-reset the device mid-commit-path |
| `zigbee/ota/ota.c` | `ota_mcuReboot()` writes the `0x4b` staging flag with `flash_write()` and resets unconditionally (never gated on a verify result) | the ours->ours install silently never commits — updates download but never install |
| `zigbee/ota/ota.c` | `ota_mcuReboot()` also writes the 12-byte install descriptor `{0x70001, 0x70000, 1}` at `0xF7000` — word1 is the constant `0x00070000`, **not** the image size (build 11; the build-09 `size` encoding downloads fine but the bootloader's guard `word0 == byte8 + word1` rejects it and the install never commits — bughunt/install_sm.md §10) | ours->ours OTA downloads but the Tuya bootloader's install state machine never commits — updates never install |
| `platform/chip_8258/spi_i.h` | bound `mspi_wait()` with `MSPI_WAIT_MAX_ITER 20000u` | the unbounded IRQ-off flash busy-wait hang returns |

**Applied as of build 06** — both are live in the working-tree vendored SDK and
are recorded in the table above:

* `zigbee/ota/ota.c` — `drv_wd_clear()` inside `ota_newImageValid()`'s CRC
  loop (`ota.c:161-172`) shipped **in the same change** as
  `MODULE_WATCHDOG_ENABLE 1` (`light/app_cfg.h:76`), honouring the sequencing
  rule. The 600 ms justification remains in `bughunt/watchdog_design.md`.
* `platform/chip_8258/spi_i.h` — `mspi_wait()` is bounded with
  `MSPI_WAIT_MAX_ITER 20000u` (~2.5–3 ms at 48 MHz), closing the L2-a
  unbounded IRQ-off busy-wait (`HANG_FINDINGS.md` §2).

`build/*` is gitignored, so these working-tree patches are not in git history;
the table above is the durable record — re-apply them after any SDK update.

---

## 3. Things that are safe to change

These are the parts you probably came here to edit:

* **`light/light_effects.c`** — the light-show renderers. Add effects, change
  curves, tune timing. Bounded by `MOES_EF_MAX`; keep the enum and the
  converter's `EFFECTS` array in the same order.
* **Colour maths** in `light/tuyaLightCtrl.c`: `hsvToRGB`,
  `temperatureToCW`, the gamma in `moes_outSet`, the `gmw*` white-balance
  trim. Worst case the light looks wrong and you push another update.
* **`device_config/light_ts0505b.h`** pin map, PWM frequency, effect tick —
  but see §4.1 before touching pins.
* **z2m converters** in `converters/`. Not firmware at all; instant to
  revert.

---

## 4. Things to be careful with

### 4.1 The PWM duty scale

`pwmSetDuty()` divides by `ZCL_LEVEL_ATTR_MAX_LEVEL * PWM_FULL_DUTYCYCLE`
(254 × 100 = 25400). Therefore `moes_duty()` must return
`value * PWM_FULL_DUTYCYCLE` for a 0–255 input. An earlier `/2` "to fit in
u16" capped every channel at **50 % brightness** — 25500 fits in a u16 just
fine. If you change either side of this, recompute the full-scale duty and
check it equals `PMW_MAX_TICK`.

### 4.2 The pin map is not obvious

The Tuya JSON names **module pin numbers**, not GPIOs: `r_pin:4` means ZT3L
module pin 4, which is `PB4`/`PWM4`. The mapping in `moes_pinTable[]` is
cross-confirmed two ways (the factory JSON on a real unit, and a traced
schematic of the same board). Each channel's PWM channel number is fixed by
silicon — you cannot assign an arbitrary PWM channel to an arbitrary pin.

### 4.3 Do not resurrect runtime parsing of the factory JSON

It was implemented, and it wrote 17 of 18 fields to the wrong struct offsets
(all five pin fields collided). It is deleted on purpose. The fleet is one
hardware revision; the pin map is compiled in and verified. A parser bug on
the boot path produces nonsense pins on a device you cannot reach.

### 4.4 The Tuya 0xEF00 cluster has two easy traps

* the frame's `seq` is **u16**, not u8 — get this wrong and every field is
  read one byte early
* zigbee-herdsman defines cluster `0xEF00` with **no manufacturer code**, so
  it must be registered with `MANUFACTURER_CODE_NONE`. Register it as
  manufacturer-specific and `zcl.c` rejects every command silently.

### 4.5 Reboots and the factory-reset counter

Three power cycles factory-reset the light (stock behaviour, `rstnum:3`).
Any *deliberate* reboot must call `moes_resetSkipNextBoot()` first, and the
skip path must also clear the counter — otherwise a stale count survives and
one later power cycle trips an unexpected reset.

### 4.6 Image size and RAM code

Two hard ceilings:

* image ≤ `0x68000` (416 KB). Currently ~197 KB.
* RAM code must not exceed what the **stock bootloader** copies. Measured from
  the `+0x0C` header field × 16: **ours declares 1792 B, the stock app at
  `0x8000` declares 3584 B**, so that is the proven headroom. (An earlier
  revision of this guide said 5888 / 7680; those numbers do not match the
  built image or the dump. The conclusion is unchanged.) Adding a lot of
  `_attribute_ram_code_sec_` functions eats it — this is one reason
  `moes_otaScheme.c` is now compiled only under `MOES_NOBOOT_MIGRATION`.

---

### 4.7 Two more compile-time traps worth knowing

**The ZCL cluster table has one slot left.** `ZCL_CLUSTER_NUM_MAX` is 11 and we
use 10 (8 application clusters + green power + OTA). `zcl_registerCluster()`
returns `ZCL_STA_INSUFFICIENT_SPACE` past the limit and `zcl_register()` gives
up *silently* — so an overflow shows up as a missing cluster, and OTA is
registered late enough to be one of the first lost. There is now a static
assert in `light/tuyaLightEpCfg.c`; if you add a cluster, raise the limit in
`light/stack_cfg.h` in the same commit.

**Never remove a reportable attribute without thinking.** The reporting table
is restored from NV verbatim and the SDK posts
`SYS_EXCEPTTION_ZB_ZCL_ENTRY` — which our exception handler turns into
`SYSTEM_RESET()` — if a restored entry names an attribute the running image
does not have. `reportNoMinLimit()` runs on every idle poll, so that is an
instant, permanent boot loop on every light that had reporting configured for
it. `tuyaLight_reportingTabSanitize()` in `light/tuyaLight.c` now drops such
entries at startup; keep it, and keep it running *after* every
`zcl_register()`/`ota_init()`/`gp_init()`.

## 5. Mandatory pre-flash checklist

Never flash anything to a ceiling light until all of these pass:

```bash
cmake . -B build -DDEVICE_VARIANT=TS0505B
cmake --build build --target light_TS0505B.zigbee -j8
```

1. **Zero new compiler warnings.** Implicit declarations are real bugs here.
2. **Header valid**: `KNLT` at +8, `5d 02` at +6, size field == file size,
   CRC32 over `[0:-4]` == trailing 4 bytes.
3. **Layout**: confirm the preprocessor really got the flag —
   `NV_BASE_ADDRESS` must expand to `0xD8000` **for the TS0505B target**
   (`compile_commands.json` has entries for other targets; pick the one
   containing `BUILD_TS0505B`).
4. **Identity strings**: prefix bytes 16 and 7 in the built binary.
5. **Bump `APP_BUILD`** in `common/version.h`, or the image is not offered
   as an update.
6. `cd tools/rescue_hosttest && make check` — 22 scenarios, all passing.
7. Test on **one** light, following `OTA_TEST_PLAN.md`. Watch the
   `device_announce` cadence, not the interview: **more than one announce in
   15 minutes means stop.** Confirm it rejoins with the **same IEEE** and
   still accepts a subsequent OTA *before* touching another light.

A stalled or failed download is safe — the light keeps running what it had.
The only irreversible moment is a *complete, CRC-valid* image booting.

---

## 6. Why there is no wired safety net

SWire **reads** work perfectly (`SWS_DIV=110`, `-b 460800`) and are the way
to back up a unit. SWire **writes are fundamentally broken** on this part:
byte values `0x80–0xBF` corrupt deterministically, in both flash and SRAM,
independent of divider, chunk size and cell encoding. A UART-framed `'1'`
cell is 8/10 bits low, leaving a 2-bit gap that the chip's decoder swallows,
so a `'0'` following `'1'`s decodes as `'1'`. This is why pvvx abandoned
COM-port SWire writing. Do not spend a day rediscovering it.

The stock bootloader also speaks a UART flashing protocol on module pins
15/16 (115200, `0x55`/`0xAA` framing, crc8) — implemented in
`uart_flash/uart_flash.py` — but the module never answered on those pins.
Unresolved.

**So: OTA is the only way in, and keeping OTA working is the highest
priority in this codebase.** Everything in §1 exists to protect it.
