# Audit findings — Moes TS0505B custom firmware, pre-OTA review

**Date:** 2026-08-15
**Scope:** branch `moes-ts0505b` in `work/tuyaZigbee`, at `323af86` (v1.2, the
build staged after the 2026-08-14 post-mortem).
**Nothing was flashed.** No device was written to over the air or over wires.
The production zigbee2mqtt host was not modified.

Severity is ranked by the risk model in `AI_AUDIT_BRIEF.md` §1 — *can it cost
us OTA access?* — not by how wrong the code looks.

| | meaning |
|---|---|
| **CRITICAL** | reproducibly stops the light staying up long enough to take an OTA. Fixture out of the ceiling. |
| **HIGH** | can do that, but needs a coincidence (an NV byte, a later firmware change) to fire. |
| **MEDIUM** | breaks an invariant that protects OTA, or is a latent version of a CRITICAL. |
| **LOW** | wrong, network-reachable or user-visible, but cannot cost OTA. |

---

## The headline

**There was a second, still-live cause of the exact failure that cost a
fixture on 2026-08-14, and the v1.2 fix does not touch it.** Flashing v1.2 as
staged would very likely have put the next light into a reset loop too — a
different loop, from a different file, with a similar period.

That is finding **A-1**. Everything else is smaller.

---

# Part A — findings

## A-1 · CRITICAL · A phantom button press factory-resets the light every ~5 s

**Where:** `device_config/light_ts0505b.h:130` (`KB_SCAN_PINS`),
`light/app_ui.c:128` (`app_key_handler`), `light/app_ui.c:81`
(`buttonKeepPressed` → `zb_factoryReset()`), called from
`light/tuyaLight.c:312` (`app_task`).

**What breaks.** The light factory-resets and reboots roughly every 5–7
seconds, forever, from the first boot after conversion. An OTA needs ~30
minutes of continuous uptime, so it can never be repaired remotely.

**How it is triggered.** By nothing. It fires on a healthy light, on the first
boot, with no network traffic and no user action.

**Mechanism.**

1. `device_config/light_ts0505b.h` declares `KB_SCAN_PINS {GPIO_PC0, GPIO_PD4}`
   but — uniquely among the board configs in this tree — does *not* pair them
   with `PC0_INPUT_ENABLE 1` / `PULL_WAKEUP_SRC_PC0 PM_PIN_PULLUP_10K` and the
   `PD4` equivalents. `light_ts0501b.h`, `board_tuya_W.h`, `switch_zbws01a.h`
   and `iaszone_zg102zl.h` all define them; this file does not.
2. The `gpio_default.h` fallbacks therefore apply: `PC0_INPUT_ENABLE 0`,
   `PULL_WAKEUP_SRC_PC0 0`, `PC0_OUTPUT_ENABLE 0`. The pad is isolated with the
   input buffer powered down, so `gpio_read_all()` reports **0** for it.
3. `kb_key_pressed()` (`proj/drivers/drv_keyboard.c`) treats a scan pin
   reading LOW as *pressed*, because `KB_LINE_HIGH_VALID` defaults to 0.
4. `key_debounce_filter()` needs two consecutive scans to report a change, so
   on the **second** `app_task()` poll after boot `keyScan_keyPressedCB()`
   fires with `keycode[0] == VK_SW1`, sets `gLightCtx.keyPressedTime` and
   `gLightCtx.state = APP_FACTORY_NEW_SET_CHECK`.
5. The key never "releases", so the state never clears. Five seconds later
   `clock_time_exceed(keyPressedTime, 5*1000*1000)` is true and
   `buttonKeepPressed(VK_SW1)` calls **`zb_factoryReset()`**.
6. Reboot. Repeat.

`HAVE_NET_BUTTON 0` in `light_ts0505b.h:119` looks like it prevents exactly
this. It does not: **the macro was defined in four board headers and read
nowhere in the tree.** `grep -rn HAVE_NET_BUTTON` returned only the four
definitions.

**Note.** `app_key_handler()` is called from `app_task()` *unconditionally* —
it sits above the `if(BDB_STATE_GET() == BDB_STATE_IDLE)` guard, so it runs
from the very first poll, before the stack has settled.

**Fixed** — commit `ea7795b`. `HAVE_NET_BUTTON` now defaults to 1 in
`common/comm_cfg.h` (so every other board is unchanged) and gates both the
`app_task()` call site and the body of `app_key_handler()`.

Deliberately **not** fixed by adding `PC0_INPUT_ENABLE`/pull-ups: nothing is
known to be connected to PC0/PD4 on this board, and enabling a 10 K pull-up
onto an unknown net is the riskier of the two options. Leaving the pads
isolated and never scanning them is strictly safer. If someone later
establishes what those nets are and wants the buttons back, add the pin
configuration *and* set `HAVE_NET_BUTTON 1` — both, never one.

---

## A-2 · HIGH · The v1.2 factory-reset clamp is off by one

**Where:** `common/factory_reset.c:111` (was `>`, now `>=`).

**What breaks.** A power-cycle counter restored from NV as exactly
`FACTORY_RESET_POWER_CNT_THRESHOLD` (3) survives the clamp, is incremented to
4, and trips `factoryRst_timerCb()`'s `>= 3` test two seconds into the boot →
`zb_factoryReset()` → reboot → the value is written back to 0, so it is not
usually a permanent loop, but it is an unexplained factory reset.

**How it is triggered.** Any NV byte that reads back as exactly 3 — one value
in 256 — or a power cut in the ~2 s window between `factoryRst_init()` writing
the count and `factoryRst_timerCb()` clearing it.

**Why `>=` is correct.** The largest value a legitimate power cycle can leave
in NV is `THRESHOLD - 1`, because the cycle that *reaches* `THRESHOLD` sets
`factoryRst_exist` and immediately writes 0 back. A restored value of exactly
`THRESHOLD` therefore cannot come from a user gesture.

This is the hardening added in v1.2 as one of the two corrective actions from
the post-mortem. It works; it was just one character too permissive.

**Fixed** — commit `d0285ce`.

---

## A-3 · HIGH (latent) · Restored reporting entries are never revalidated

**Where:** `zigbee/zcl/zcl_reporting.c:400`, `:437`, `:489` (vendored SDK);
entry point `light/tuyaLight.c` `report_handler()` → `reportNoMinLimit()`.

**What breaks.** `zcl_reportingTabInit()` restores the whole reporting table
from NV without validating anything. The only live validation is in
`zcl_configureReporting()`, on the *inbound command* path. Three places then
dereference `zcl_findAttribute()` on a restored entry and each calls
`ZB_EXCEPTION_POST(SYS_EXCEPTTION_ZB_ZCL_ENTRY)` if the attribute is missing.
That reaches the handler registered by `sys_exceptHandlerRegister()`, which
calls `SYSTEM_RESET()`.

`reportNoMinLimit()` runs from `app_task()` on *every idle poll*, so the reset
lands within milliseconds of the stack going idle and repeats identically on
the next boot. **A firmware update that merely removes a reportable attribute
would boot-loop every light that had reporting configured on it.**

**How it is triggered.** Not reachable today, and the negative result is worth
recording:

* Our own earlier images have the same attribute set.
* The stock Tuya NV cannot be restored into our structures — see the
  "checked and clean" section below for the dump evidence.

It becomes reachable the moment a future image drops a reportable attribute,
and **v1.2 has just added three of them**, on cluster `0xEF00`
(`light/zcl_tuyaMfg.c:29-31`, all `ACCESS_CONTROL_REPORTABLE`).

**Fixed** — commit `9bc7aef`. `tuyaLight_reportingTabSanitize()` runs at the
end of `user_app_init()`, after every cluster is registered, drops any entry
`zcl_findAttribute()` cannot resolve, and persists the cleaned table.

---

## A-4 · MEDIUM · `light_fresh()` recursion is bounded only by statement order

**Where:** `light/tuyaLightCtrl.c:314` (`light_fresh`),
`light/light_effects.c` (`lightFx_start`), `light/zcl_colorCtrlCb.c`
(`tuyaLight_colorInit`).

**What breaks.** The cycle the brief suspected is real:

```
light_fresh()
 └─ lightFx_active() → lightFx_start(MOES_EF_STEADY, …)
     └─ light_adjust()
         └─ tuyaLight_colorInit()
             └─ light_applyUpdate()  ×2
                 └─ light_fresh()          ← back to the top
```

**It terminates today**, at depth 2, and only because `lightFx_start()` sets
`g_moesFx.effect = MOES_EF_STEADY` *before* it calls `light_adjust()`, so the
inner `lightFx_active()` is FALSE. Swap those two statements — a completely
innocuous-looking edit in a different file — and the recursion is unbounded.
On this part that is a stack overflow → fault → `tuyaLightSysException()` →
`SYSTEM_RESET()` → reset loop.

**Fixed** — commit `d0285ce`. An explicit depth guard in `light_fresh()`, so
the bound is a property of the function rather than of statement order
somewhere else.

---

## A-5 · MEDIUM · The exception handler writes NV before resetting

**Where:** `light/tuyaLight.c:340` (`tuyaLightSysException`).

**What breaks.** Upstream saved on/off, level and colour to NV from the fault
handler. Three problems on this device:

1. `sys_exceptionPost()` (`proj/os/ev.c:33`) calls the handler
   **synchronously, from wherever the fault was detected** — including from
   inside the NV layer itself (`drv_nv.c:97`,
   `nv_itemLengthCheckAdd` → `SYS_EXCEPTTION_NV_CHECK_TABLE_FULL`) and from
   the `ev_buffer` free path. Re-entering `nv_flashWriteNew()` from there can
   leave a sector half-written, which is worse than the fault being reacted to.
2. If the fault repeats every boot, so do the flash writes.
3. It buys nothing — `tuyaLightAttrsChk()` already persists that state one
   second after any change.

**Fixed** — commit `d0285ce`. Reset immediately, write nothing. The reset
itself is what the new probation counter records, on the next boot, from a
context where writing NV is safe.

---

## A-6 · MEDIUM · A network leave reboots without marking the reboot

**Where:** `light/zb_appCb.c` (`tuyaLight_softReset`, scheduled from
`tuyaLight_leaveCnfHandler`).

**What breaks.** `MOES_EDITING_GUIDE.md` §4.5 requires every *deliberate*
reboot to call `moes_resetSkipNextBoot()` first, so the 3-power-cycle gesture
never counts a firmware reboot as a user power cycle. The leave path did not,
so a z2m "remove device" left a stale count of 1 behind and one later power
cycle could trip the gesture a cycle early.

**Fixed** — commit `d0285ce`.

---

## A-7 · MEDIUM · `moes_otaBankInstall()` was compiled into every image

**Where:** `light/moes_otaScheme.c` (was `#if MOES_TS0505B && BOOT_LOADER_MODE`
around the definition, `#if defined(MOES_NOBOOT_MIGRATION)` around the only
call site in `light/zb_appCb.c`).

**What breaks.** Nothing today — the call site is off. But the definition's
guard was *exactly our build*, so the most dangerous routine in the tree sat
in the image, in `.ram_code`, one accidental `-D` away from running. What it
does:

```c
ram_flash_write_page(0x0 + FLASH_TLNK_FLAG_OFFSET, 4, &unused_flag);   /* zeroes KNLT inside the STOCK BOOTLOADER */
ram_flash_write_page(0x8000 + FLASH_TLNK_FLAG_OFFSET, 4, &unused_flag); /* and the running slot */
SYSTEM_RESET();
```

`MOES_EDITING_GUIDE.md` §1.3 says the bootloader region must never be written.
If the image left at `0x40000` does not boot — and it would not, see
`FALLBACK_DESIGN.md` §6 — then *nothing* boots: bootloader gone, `0x8000`
gone, no working wired write path. That can strand the deployed fleet, not just
one fixture.

It also consumed RAM-code budget for no benefit.

**Fixed** — commit `d0285ce`. The whole file is now behind
`MOES_NOBOOT_MIGRATION`, the same symbol as its call sites, so the two cannot
drift apart. Its stale `0x77000` comment is corrected to `0x70000`.

---

## A-8 · MEDIUM · No compile-time guard on the ZCL cluster table

**Where:** `light/stack_cfg.h:53` (`ZCL_CLUSTER_NUM_MAX 11`),
`light/tuyaLightEpCfg.c`.

**What breaks.** `zcl_registerCluster()` returns
`ZCL_STA_INSUFFICIENT_SPACE` past the limit and `zcl_register()` (`zcl.c:134`)
just `return`s. No warning, no log, nothing on the wire. An overflow therefore
presents as a *silently missing cluster* — and OTA is registered late in the
sequence, so it is among the first to be lost.

**Current usage is 10 of 11** (counted and confirmed, see below). **One slot
spare.** Adding one more cluster is silent, permanent OTA loss across the
fleet.

**Fixed** — commit `d0285ce`. A negative-array-size static assert in
`tuyaLightEpCfg.c` (the tc32 toolchain predates C11, so `_Static_assert` is
not available). Verified by temporarily setting the limit to 9 and confirming
the build fails with *"size of array 'moes_zclClusterTableMustFit' is
negative"*.

---

## A-9 · LOW · Truncating bounds check on network-reachable input

**Where:** `light/zcl_tuyaMfg.c:77`, was
`if((u16)(6 + vlen) > len) return ZCL_STA_MALFORMED_COMMAND;`

**What breaks.** `vlen` is attacker-controlled (bytes 4–5 of the datapoint
frame). `6 + 0xFFFF` promotes to `int` 65541, and the `(u16)` cast truncates
it to **5**, which passes `5 > len` for every `len >= 5`. The loop at
`zcl_tuyaMfg.c:83` then reads up to 4 bytes past the ASDU.

Low severity because the read is bounded at 4 bytes, lands in an `ev_buffer`
pool allocation (no fault, no meaningful disclosure), and the value is
range-checked before use. But it is unauthenticated network input on the one
cluster we added.

**Not** an underflow risk from the other direction: `zcl.c:789/800` validates
`asduLen >= 5` / `>= 3` before computing `dataLen`, so `dataLen` cannot wrap.

**Fixed** — commit `d0285ce`. Compare in `u32`.

---

## A-10 · LOW · `MOES_EF_TWINKLE` overflows the level argument

**Where:** `light/light_effects.c` (`fxRender`, `MOES_EF_TWINKLE`).

`(v * (64 + spark)) >> 8` with `v` up to 254 and `spark` up to 255 reaches
**316**, which wrapped to 60 in `hsvToRGB()`'s `u8 level` parameter — so the
brightest sparkles rendered as the darkest frames. Confirmed as the brief
suspected. **Fixed** (clamped to 254) — commit `d0285ce`.

---

## A-11 · LOW · `MOES_EF_COLOR_STEP` applies the modulo after the cast

**Where:** `light/light_effects.c` (`fxRender`, `MOES_EF_COLOR_STEP`).

`h = (u8)(...) % 255` — the cast binds tighter than `%`, so the sum (up to
488) was truncated to 8 bits *before* the reduction and the phase offset
folded the hue wheel back on itself. `MOES_EF_CHASE` three cases earlier gets
this right; this one did not. **Fixed** — commit `d0285ce`.

---

## A-12 · LOW · `moes_flashGetIeee()` rejects uppercase hex

**Where:** `light/moes_flashcfg.c` (`moes_flashGetIeee`).

The parser accepted `0-9a-f` only. An uppercase digit fails, and the patched
`generateIEEEAddr()` (`zigbee/mac/mac_pib.c:141`) falls through to the binary
block at `0x0FF000` — which on this board holds no valid address. The light
would come up with a different IEEE: a brand-new device in zigbee2mqtt, dead
history, broken automations.

Not a brick (the light still interviews, still matches the fingerprint, still
gets its converter and therefore still gets OTA), but exactly the kind of
thing you do not want to discover across deployed fixtures. One sampled unit
has been dumped and stores it lowercase; the remaining fleet has not been
checked.

**Fixed** — commit `d0285ce`. Accept `A-F` as well.

**Note on the fallback path:** it is safe. `generateIEEEAddr()`'s
`ZB_EXCEPTION_POST(SYS_EXCEPTTION_COMMON_PARAM_ERROR)` at `mac_pib.c:178` is
only reachable when `CFG_MAC_ADDRESS` reads as invalid *and* a freshly written
random address reads back invalid. On the dumped unit `0x0FF000` holds
`d1 fa e7 38 e1 e4 ed ec`, which is not invalid, so that branch is not taken.

---

## A-13 · LOW · Colour temperature was not implemented in this build

**Where:** `light/zcl_colorCtrlCb.c` throughout.

The file is written `#if COLOR_RGB_SUPPORT / #elif COLOR_CCT_SUPPORT` from top
to bottom. `device_config/light_ts0505b.h:38-39` sets **both** to 1, so RGB won
every branch and the CCT half was never compiled:

* `tuyaLight_moveToColorTemperatureProcess()`,
  `moveColorTemperatureProcess()` and `stepColorTemperatureProcess()` did not
  exist in the image and their three cases were absent from the dispatch, so
  every colour-temperature command got `UNSUP_CLUSTER_COMMAND`;
* `tuyaLight_updateColor()` called `hwLight_colorUpdate_HSV2RGB()`
  unconditionally, so the CW/WW channels were unreachable from the normal
  control path;
* `tuyaLight_colorInit()` overwrote `colorCapabilities` with
  `HUE_SATURATION` on every boot — discarding the colour-temperature bit
  `tuyaLightEpCfg.c` publishes, *before* zigbee2mqtt ever reads it — and forced
  `colorMode` to hue/sat, so a downlight came back from a power cut in RGB
  mode ignoring the colour temperature it had just restored from NV.

Meanwhile the simple descriptor advertises `HA_DEV_EXTENDED_COLOR_LIGHT`, the
attribute table publishes `colorTemperatureMireds` with physical min/max, and
`tuyaLightCtrl.c` ships `temperatureToCW()` and
`hwLight_colorUpdate_colorTemperature()` for a five-channel RGBCW stage whose
header comment claims both modes "live side by side".

LOW by the §1 risk model — it cannot cost OTA — but it is the largest
*functional* defect found, and on deployed CCT downlights it is the point of the
fixture.

**Fixed, in its own commit** — `5d932a6`, separate from the safety work so it
can be reverted alone. Verified by preprocessing the translation unit: the
three handlers now appear (definition + dispatch); before, they appeared zero
times. **Not verified on hardware.**

---

## A-14 · INFO · The TS0501B target does not build in this fork

`cmake --build build --target light_TS0501B` fails with 17 errors — and did so
before this audit (checked at `323af86`). `tuyaLightCtrl.c` was rewritten for
the five-channel TS0505B board and `device_config/light_ts0501b.h` does not
define `MOES_PWM_FREQUENCY_DEFAULT` or the RGBCW colour attributes. Not fixed;
recorded so it is not mistaken for new breakage.

---

## A-15 · INFO · `MOES_EDITING_GUIDE.md` §4.6 RAM-code figures are wrong

The guide says "Ours declares 5888 B; the stock app declares 7680 B". The
built image's `+0x0C` field is `0x70` → **1792 B**, and the stock app at
`0x8000` in `dump/zt3l_MERGED.bin` has `0xE0` → **3584 B**. The ratio and the
conclusion (we are inside the proven headroom) hold; the numbers do not.
Corrected in the guide.

---

# Checked and found clean

Negative results, with what was actually verified.

### Flash layout and OTA staging — clean, and now proven for *this* target

Preprocessed `light/tuyaLight.c` with the exact `compile_commands.json`
entry containing `BUILD_TS0505B`:

| symbol | value |
|---|---|
| `NV_BASE_ADDRESS` | `0xD8000` ✓ |
| `FLASH_ADDR_OF_APP_FW` | `0x8000` |
| `FLASH_OTA_IMAGE_MAX_SIZE` | `(0xD8000 - 0x8000)/2` = `0x68000` |
| `FLASH_ADDR_OF_OTA_IMAGE` | `0x8000 + 0x68000` = **`0x70000`** ✓ |
| NV span end (`MODULES_START_ADDR(7)` + 2 × 4 sectors) | `0xEE000`, below `0xF8000` ✓ |
| `BOOT_LOADER_MODE` | 1 |
| `MODULE_WATCHDOG_ENABLE` | 0 |

### The stock Tuya NV cannot poison ours — proven from the dump

This mattered: our firmware and the stock firmware share `NV_BASE 0xD8000`, so
"stock NV restored into our structs" was a plausible route to garbage
attributes on the very first boot after conversion. It is not possible.

Surveying `dump/zt3l_MERGED.bin` for non-`0xFF` content shows the stock NV
sectors at **`0xEA000`, `0xEB000`, `0xEC000`, `0xEE000`** and *nothing*
between `0x05C000` and `0x0EA000`. Decoding the `nv_sect_info_t` headers:

```
0xEA000: 5a 5a 09 00   usedFlag=0x5A5A (VALID)   idName=9
0xEB000: 50 50 09 00   usedFlag=0x5050 (INVALID) idName=9
0xEC000: 5a 5a 0a 00   VALID                     idName=10
0xEE000: 5a 5a 0b 00   VALID                     idName=11
```

`MODULES_START_ADDR(id) = NV_BASE + 0x1000*2*id` puts id 9 at exactly
`0xEA000`, id 10 at `0xEC000`, id 11 at `0xEE000` — confirming `NV_BASE
0xD8000` for the stock firmware, and that its SDK has more NV modules than our
`NV_MAX_MODULS 8`. Our modules use ids 0–7, and `nv_sector_read()`
(`drv_nv.c:204`) accepts a sector only if `s.idName == id`. Every stock sector
fails that test. Our own region `0xD8000`–`0xEA000` is fully erased on stock,
so modules 0–6 come up empty and return `NV_ITEM_NOT_FOUND`.

Consequence: no stock reporting table, no stock ZCL attributes, no stock
scene table can be read back into our structures. **Clean.**

(`0xEE000` is stock module 11 and is outside our NV span — our keypair module
ends at exactly `0xEE000` — so we never touch it either.)

### ZCL cluster table capacity — 10 of 11, OTA registers

Counted, then confirmed by making the static assert fail at 9 and pass at 10:

| # | cluster | endpoint | registered by |
|---|---|---|---|
| 1–8 | basic, identify, groups, scenes, on/off, level, colour, `0xEF00` | 1 | `zcl_register()` |
| 9 | green power | 242 | `gp_init()` |
| 10 | OTA | 1 | `ota_init()` |

WWAH is **not** registered (`ZCL_WWAH_SUPPORT 0`, and `ZCL_WWAH` is confirmed
undefined by preprocessing). Touchlink is advertised in the simple descriptor
but `zcl_touchlink_register()` is **never called anywhere in the tree**, so it
costs no slot. **No overflow. OTA is registered.**

### Reporting table capacity — no crash path

`ZCL_REPORTING_TABLE_NUM 4`. `bdb_defaultReportingCfg()` in `user_init()`
takes one for on/off. If zigbee2mqtt asks for more than the table holds,
`zcl_configureReporting()` returns `ZCL_STA_INSUFFICIENT_SPACE` per record and
the device answers normally — no crash, just fewer reports.

Critically, `zcl_configureReporting()` (`zcl.c:1741`) validates
`zcl_findAttribute()` **before** `zcl_reportCfgInfoEntryUpdate()`, so the
`ZB_EXCEPTION_POST` at `zcl_reporting.c:205` is **not network-reachable**. The
restore path was the gap — see A-3.

### Initialization order — clean apart from what A-1 and A-3 fixed

Walked `user_init()` and `user_app_init()` against
`apps/sampleLight/sampleLight.c`. Ordering matches the canonical sample. The
only pre-`stack_init()` work is `led_init()` and `hwLight_init()`, and
`hwLight_init()` touches only PWM/GPIO registers plus `moes_flashCfgLoad()`,
which is a `memset` and compiled constants — no NV, no timers, no radio.
`moes_flashGetIeee()` is called later, from the stack, and uses raw
`flash_read()` rather than NV. `lightFx_init()` is a `memset`.

### Effect-engine arithmetic — everything else provably safe

* `fxPeriod()` cannot divide by zero: `sp = speed ? speed : 50` and it returns
  `p ? p : 1`.
* `MOES_EF_LIGHTNING`'s `r0 % (p * 3 / 4)` cannot divide by zero:
  `g_moesFx.speed` is a `u8`, so `p = 300000/(2*sp) >= 300000/510 = 588`, and
  `p*3/4 >= 441`.
* Every `fxSine[...]` index is provably `< 64`: the form is
  `(t % p) * 64u / p` with `(t % p) <= p-1`, and the intermediate
  `(t % p) * 64` peaks around 19.2 M for the slowest effect — well inside
  `u32`.
* `zb_random()` is `(u16)drv_u32Rand()` = `(u16)rand()`, a cheap LCG. Calling
  it twice per frame at 25 fps is free. (It returns 16 bits, not 32 —
  `fxNoise()`'s `u32 r` gets zeros in the top half. Cosmetic only.)

### The 25 fps effect timer cannot starve the radio

`TL_ZB_TIMER_SCHEDULE` → `ev_timer_taskPost` → `ev_timer_add`, and callbacks
run from `ev_timer_process()` inside `ev_main()` in the **main loop** — not an
ISR. A frame is one integer HSV conversion, a few software divisions
(`platform/tc32/div_mod.S`) and five PWM register writes: order tens of
microseconds out of a 40 ms budget at 48 MHz. It cannot preempt the radio, and
it cannot meaningfully delay it.

**On the unexplained OTA stalls (2.81 %, then 12.44 %):** as the brief notes,
those happened on *stock* firmware, so nothing in our timer can be the cause.
No testable theory formed from the firmware side. The one asymmetry visible
here is that `z2m` was using 50 B blocks at 250 ms while `zcl_attr_minBlockPeriod`
drives `ota_sendImageBlockReqDelay` on the device — the stall signature (device
stops requesting blocks, no reboot, healthy afterwards) is consistent with a
lost `imageBlockRsp` and an `OTA_MAX_IMAGE_BLOCK_RSP_WAIT_TIME` expiry that
aborts quietly, which would be an RF/retry problem rather than a timing one.
That is consistent with the observation that a power cycle restoring LQI from
167 to 255 got 4× further. Recommend raising
`ota.image_block_response_delay` and retrying rather than looking for a
firmware cause.

### `temperatureToCW()` cannot divide by zero

`colorTempPhysicalMinMireds` / `MaxMireds` are `ACCESS_CONTROL_READ` only in
`lightColorCtrl_attrTbl` — not writable over the air — and are **not**
restored from NV (`zcl_nv_colorCtrl_t` carries hue, saturation,
`colorTemperatureMireds` and `startUpColorTemperatureMireds` only). The
divisor is the compile-time constant `0x01C6 - 0x00FA = 204`.

### Our NV item ids are legal

`MOES_NV_ITEM_SKIP_RST 0x70` and the new `MOES_NV_ITEM_BOOT_PROBATION 0x71`,
both in `NV_MODULE_APP`:

* No collision — the SDK's `nv_item_t` uses `0x01`–`0x2C` plus `0x80`.
* No range check to fail — `nv_flashWriteNew()` rejects only
  `NV_MODULE_NWK_FRAME_COUNT`; `itemId` is unvalidated. Only `0x00`
  (reserved) and `0xFF` (`ITEM_FIELD_IDLE`) have special meaning.
* No pressure on `g_nvItemLenCheckTbl` — that 16-entry table is only fed by
  `nv_itemLengthCheckAdd()`, called from `zcl_nv.c` and `aps_group.c` (and the
  prebuilt library). We do not call it, so we cannot trip
  `SYS_EXCEPTTION_NV_CHECK_TABLE_FULL` at `drv_nv.c:97`.
* The read path handles "item not found" — both call sites check the
  `nv_sts_t` and default explicitly.

### Every reset path, enumerated

| site | trigger | can it repeat? | status |
|---|---|---|---|
| `app_ui.c:84` `zb_factoryReset()` | 5 s "held" VK_SW1 | **yes, every boot** | **A-1, fixed** |
| `factory_reset.c:90` `zb_factoryReset()` | power-cycle count ≥ 3 | yes, if NV garbage | **A-2, clamped** |
| `tuyaLight.c:364` `SYSTEM_RESET()` | any posted exception | yes | A-5 hardened; counted by rescue mode |
| `zb_appCb.c` `ota_mcuReboot()` | OTA download complete | no (needs a valid staged image) | clean |
| `zb_appCb.c` `tuyaLight_softReset()` | network leave confirm | no | **A-6, marked** |
| `moes_otaScheme.c` ×4 `SYSTEM_RESET()` | migration | n/a | **A-7, not compiled** |
| `drv_hw.c:153` `SYSTEM_RESET()` | low voltage | n/a | `VOLTAGE_DETECT_ENABLE 0` |
| `ota.c:228` `SYSTEM_RESET()` | inside `ota_mcuReboot()` | no | clean |

And every `ZB_EXCEPTION_POST()` in compiled SDK sources was reviewed:
`zcl_reporting.c` ×4 (A-3 + the unreachable one), `drv_nv.c:97` (unreachable
for us), `ev_timer.c` ×3 (null/pool-exhaustion, our worst case is a timer that
does not start), `ev_buffer.c` ×2 (double-free guards), `string.c` ×12
(memory-access asserts), `mac_pib.c:178` (see A-12), `ev.c:60`
(`sys_stackStatusCheck`, commented out of `ev_main`), and
`zcl_zll_commissioning.c:830` (touchlink buffer, cluster never registered).

### Build hygiene

`cmake . -B build -DDEVICE_VARIANT=TS0505B && cmake --build build --target
light_TS0505B.zigbee -j8` from a clean object directory produces **exactly the
two known warnings** in `zcl_colorCtrlCb.c` about unused colour-loop statics —
no implicit declarations, no new warnings, before or after these changes. (The
forward declaration added in `9bc7aef` was prompted by exactly this: the first
draft compiled `tuyaLight_reportingTabSanitize()` as an implicit declaration
and the build caught it.)

### Image contract

Re-verified on every build:

| check | result |
|---|---|
| `5d 02` at `+6` | ✓ |
| `KNLT` at `+8` | ✓ |
| size field at `+0x18` == file length | ✓ |
| trailing 4 bytes == `crc32(file[0:-4]) ^ 0xFFFFFFFF` | ✓ (Telink's variant — plain `zlib.crc32` will *not* match; `ota_newImageValid()` also substitutes `TL_IMAGE_START_FLAG` at offset 8 first, which is already `'K'`) |
| size ≤ `0x68000` | ✓ 201 476 B |
| RAM code (`+0x0C` × 16) | 1792 B vs stock app 3584 B ✓ |
| `manufacturerName` length prefix | `0x10` = 16 ✓ |
| `modelId` length prefix | `0x07` = 7 ✓ |
| OTA identity | `6464-0395` (update) — `IMAGE_TYPE = (CHIP_TYPE<<8) \| (0x15\|0x80)` = `0x0395` ✓ |

### Things in §5 confirmed correct and left alone

`MOES_NV_BASE_ADDRESS=0xD8000`; the IEEE read from `0x0FB058`; `0xEF00`
registered `MANUFACTURER_CODE_NONE`; the u16 Tuya `seq`; `moes_duty()` with no
`/2`; the `16` prefix on `ZCL_BASIC_MFG_NAME`; the absent runtime JSON parser;
the stock bootloader at `0x0`–`0x8000`. All re-derived far enough to confirm,
none touched.

One consistency note while there: `moes_pinTable[]` in `moes_flashcfg.c` and
the `LED_*` / `PWM_*_CHANNEL` / `AS_PWM*` triples in `light_ts0505b.h` agree
channel for channel (R→PB4/PWM4, G→PC3/PWM1, B→PD2/PWM3, CW→PB5/PWM5,
WW→PC2/PWM0). Since `g_moesCfg.pin_*` is left 0 on purpose, `moes_chanInit()`
takes the compiled-default path for all five channels and the lookup table is
currently unused.

---

# What is left

**Is there anything still in this firmware that could cost us another
fixture?**

Nothing I found and left unfixed. The honest qualifications:

1. **A-1 was found by reading, not by testing, and so was everything else.**
   The same was true of the review that missed the `factoryRst_init()`
   ordering bug. The bench soak in `OTA_TEST_PLAN.md` is not optional.
2. **`libzb_router.a` is a black box.** Everything inside it — `nv_init()`,
   the NWK/APS state machines, the touchlink stack — was not audited and
   cannot be. Rescue mode is the answer to that: it does not assume the
   library is correct, it assumes only that a device which cannot stay up
   should stop doing optional work.
3. **`moes_pinTable[]` and the compiled pin map are still unverified against
   real LEDs.** No converted light has ever been observed emitting light. If
   the pin map is wrong the fixture stays dark — recoverable over the air, but
   it will look like a brick, and the temptation will be to pull it down.
   Watch the announce cadence, not the light.
4. **One cluster slot of headroom.** The static assert now makes that loud
   rather than silent, but it is still one slot.

`FALLBACK_DESIGN.md` answers the second question — *if the next image is bad
anyway, do we get it back without a ladder?*
