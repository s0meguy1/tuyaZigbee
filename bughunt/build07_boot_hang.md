# build07_boot_hang — why the 17:20 reset left the bench light silent with s_failCnt=0 and an armed CSMA slot

**Scope:** paper analysis only. No code changed, nothing flashed, no git mutations, no ssh, no MQTT/z2m.
Read-only git (`git show 87a4e2d`), the build-07 ELF listing `build/light/light_TS0505B.lst`,
vendored SDK sources under `build/tl_zigbee_sdk/`, and the existing bughunt records. Claims that
could not be fully proven are marked INFERRED.

---

## Verdict up front

The contradiction the parent handed me — *"stack ran a CSMA attempt but `moes_rescueBootCheck()`
never ran"* — **cannot be produced by any single image's normal boot flow.** In both build 06 and
build 07 the only CSMA path is the rejoin/steer scan, and that path is reached *after*
`moes_rescueBootCheck()` (which is the very next statement after `stack_init()` returns). There is
no code that can arm the MAC TX-timeout slot before the rescue check runs.

The evidence instead points to a much simpler reconciliation, and it also answers the parent's open
question "which build is actually in the app slot now":

> **The SRAM slot observation the parent cites — address `0x84748c` holding callback `0x21bf1` — is
> the *build-06* MAC TX-timeout pair. Build 07's `mac_csmaStart` stores `0x21cb9` into `0x847494`
> (verified from the build-07 ELF disassembly below). Therefore the code that last armed that slot
> was build 06's `mac_csmaStart`, i.e. the app slot is still running BUILD 06. The OTA install of
> build 07 did NOT happen at the 17:20 reset.**

The "`s_failCnt=0` / `s_armed=0`" readings are then an **address-map mismatch**: build-07 symbol
addresses were read against a build-06 SRAM image. At the build-06 addresses, `s_failCnt` is `>=1`
(it always is after a boot that reached the rejoin), which is exactly what a boot that re-armed the
CSMA slot must show. The device did **not** hang "before the rescue check" — it re-entered the known
build-06 rejoin-scan wedge (`bughunt/mac_scan_wedge.md`), this time wedging *during the boot rejoin*
(before any announce/join) rather than 70–95 s after a successful join.

Ranked candidates and the per-candidate stack-capture confirmation key are in §6 and §8.

---

## 1. The decisive address fact (which build is running)

Build 07 `mac_csmaStart` disassembly, `build/light/light_TS0505B.lst:53269-53342`:

| item | build 06 (from `mac_scan_wedge.md` §1) | build 07 (from ELF `.lst`) |
|---|---|---|
| `mac_csmaStart` | `0x21d4c` | `0x21e14` |
| `mac_waitTxIrqCb` | `0x21bf0` (slot callback `0x21bf1`) | `0x21cb8` (slot callback `0x21cb9`) |
| MAC TX-timeout slot base | `0x84748c` | `0x847494` |
| `g_zbInfo` (macMaxCSMABackoffs base) | `0x847118` | `0x847120` |
| `s_failCnt` | `0x842471` | `0x842479` |
| `s_rescue` | `0x842478` | `0x842480` |
| `s_armed` / `s_silence` | (does not exist in b06) | `0x842470` / `0x842471` |
| `g_zbNwkCtx` (joined bit at `+0x2d`, mask `0x04`) | `0x847498` | `0x8474a0` |

The build-07 `.lst` shows the exact literals build 07 writes on a CSMA TX:

```
53269  mac_csmaStart:
...
53311  tloadr r3, [pc, #64]  ; (21eb4)  -> r3 = 0x00021cb9
53312  tstorer r3, [r4, #0]           ; slot->cb   = 0x21cb9
53319  tstorer r0, [r4, #4]           ; slot->deadline
53337  .word 0x00847494               ; r4 = slot base 0x847494
53338  .word 0x00021cb9               ; callback literal
53341  .word 0x00847120               ; macMaxCSMABackoffs base = g_zbInfo
```

So:

- Build 06 `mac_csmaStart` writes `0x21bf1` to `0x84748c`.
- Build 07 `mac_csmaStart` writes `0x21cb9` to `0x847494`.

The parent observed `0x84748c` → `0x21bf1`. **That is unambiguously the build-06 pair.** In a
build-07 image `0x84748c` is 8 bytes *below* the real slot; after build-07 crt0 zeroes `.bss` it
would read `0` (nothing in build 07 writes that address), and the real slot at `0x847494` would hold
`0x21cb9` if a CSMA had run. The observed value cannot come from build 07.

**Conclusion: the running code is build 06. The bootloader did not consume the PRIMED staging bank
and install build 07 at the 17:20 reset (or the install was not committed).** The reason the install
did not happen is a separate question (bootloader flag semantics / reset vector / CRC), and it is
worth one line in follow-up — but it is outside the boot-path regression scope; the SRAM simply says
build 06 is still executing.

*(INFERRED: the "deadline bytes changed vs pre-reset capture" observation is self-consistent here —
the pre-reset captures `dump/bench_2026-08-15/hang_capture/sram_{1,2}.bin` are build 06, and the
17:41 read is build 06 again, so both sides of the comparison are the same build-06 slot at
`0x84748c`. A changed deadline means build-06 `mac_csmaStart` re-armed it after the reset.)*

---

## 2. Exact boot order (from source + build-07 disassembly)

`apps/common/main.c` (`build/tl_zigbee_sdk/apps/common/main.c:43-96`):

```
main():
  drv_platform_init()
  os_init(isRetention)          -> os_reset() -> ev_buf_init / ev_timer_init / zb_sched_init / tl_zbBufferInit
  user_init(isRetention)        // all app+stack init happens here, synchronously
  drv_enable_irq()
  drv_wd_setInterval(600); drv_wd_start()
  while(1){
      ev_main();                // runs ev_timer_process -> ev_timer_executeCB
      drv_wd_clear();           // main.c:88
      tl_zbTaskProcedure();     // runs TL_SCHEDULE_TASK queue (incl. bdb_task)
      drv_wd_clear();           // main.c:94
  }
```

`user_init` order, `light/tuyaLight.c:383-464`:

| line | call | notes |
|---|---|---|
| 387 | `led_init()` | |
| 389 | `hwLight_init()` | loads Tuya JSON/MAC fallback data (flash reads, not NV-module) |
| 392 | `lightFx_init()` | memset only |
| **396** | **`stack_init()`** | `tuyaLight.c:141-148` = `zb_init()` + `zb_zdoCbRegister()` |
| **410** | **`moes_rescueBootCheck()`** | first NV-module user; sets `s_failCnt` |
| 414 | `user_app_init()` | zcl init/endpoint/attrs/`zcl_reportingTabInit()` (NV)/register/ota/gp/wwah/sanitize |
| 418 | `factoryRst_init()` | skipped if rescue active; NV read+write, schedules 2 s `TL_ZB_TIMER` |
| 423 | `sys_exceptHandlerRegister()` | |
| 434/437 | `moes_outSet()` or `light_adjust()` | |
| 449 | `ev_on_poll(EV_POLL_IDLE, app_task)` | |
| 452 | `bdb_preInstallCodeLoad()` | |
| 459 | `bdb_defaultReportingCfg()` | |
| **463** | **`bdb_init(...)`** | schedules BDB init; on-network path calls `bdb_routerStart()` synchronously |

`stack_init` = `zb_init()` + `zb_zdoCbRegister()` (`light/tuyaLight.c:141-148`).

`zb_init` is in the prebuilt `libzb_router.a`; its body is visible in the build-07 disassembly
(`light_TS0505B.lst:48963`):

```
zb_init():
  nv_facrotyNewRstFlagCheck()   ; may -> nv_resetToFactoryNew()
  zb_info_load()                 ; NV load (network creds)
  tl_zbMacInit(1|0)
  tl_zbNwkInit(1|0)
  aps_init()
  zb_nwkKeySet()                 ; (on-network path)
  ss_zdoUseKey(0)
  tl_bdbAttrInit()
  af_init()
  zdo_init()
  return
```

**No TX, no scan, no CSMA anywhere in `zb_init`.** The MAC init (`tl_zbMacInit`) initializes the
radio; it does not transmit. Therefore `stack_init()` cannot arm the MAC TX-timeout slot.

`bdb_init`, `build/tl_zigbee_sdk/zigbee/bdb/bdb.c:1591-1661`:

- `factoryNew = zb_isDeviceFactoryNew()`
- `nodeIsOnANetwork = factoryNew ? 0 : 1` (line 1636)
- on-network: `bdb_routerStart()` **synchronously** (line 1645)
- factory-new: `bdb_factoryNewDevCfg()` then `TL_SCHEDULE_TASK(bdb_task, BDB_EVT_INIT_DONE)` (line 1654)

`bdb_routerStart` is an 8-byte thunk → prebuilt `zb_routerStart` (`light_TS0505B.lst:92526`). That
is the path that starts the rejoin (nwk rejoin → beacon-request scan → `tl_zbMacTx` →
`mac_csmaStart`). INFERRED for the tail inside the prebuilt lib, but the entry is synchronous and
unambiguously *after* `tuyaLight.c:410`.

`zbdemo_bdbInitCb` is **not** called from `bdb_init`. It is called from `bdb_task`
(`bdb.c:1072-1083`), which is posted via `TL_SCHEDULE_TASK` and executed by `tl_zbTaskProcedure()`
in the main loop — i.e. after `user_init` has returned. Same for `zbdemo_bdbCommissioningCb`
(`bdb.c:1135-1137`, `1268-1269`, etc.).

**Ordering consequence:** the first possible CSMA is strictly after
`stack_init()` returns **and** after `moes_rescueBootCheck()` (line 410) **and** after
`bdb_init()` (line 463). `s_failCnt` is therefore always `>=1` by the time any CSMA can happen.

---

## 3. Build-06 → build-07 delta, hunk by hunk

`git -C work/tuyaZigbee show 87a4e2d` (build 07). Files that actually run on the device vs. docs/host
test:

| file | change | runs on device? | can execute before/during `stack_init()`? |
|---|---|---|---|
| `common/version.h` | `APP_BUILD 0x06 → 0x07` | image byte only | no (no runtime branch on it) |
| `light/moes_liveness.c` | new, 120 lines | yes | **no** — all entry points called only from BDB callbacks (see §4) |
| `light/moes_liveness.h` | new, 120 lines | header only | no |
| `light/zb_appCb.c` | `+14`: include + 2 hook calls | yes | **no** — both call sites are inside `zbdemo_bdbInitCb` / `zbdemo_bdbCommissioningCb`, which run in `bdb_task` (task loop) |
| `MOES_EDITING_GUIDE.md`, `HANG_FINDINGS.md`, `OTA_TEST_PLAN.md` | docs | no | — |
| `tools/rescue_hosttest/*` | host test | no | — |

Conclusion: **nothing in the build-06→07 delta can execute before `stack_init()` returns.** The
delta cannot hang the boot before `moes_rescueBootCheck()` and cannot re-order the rescue check
relative to the first CSMA.

---

## 4. Build-07 hooks vs. MOES_EDITING_GUIDE §0 — REFUTED as the cause

§0 (`MOES_EDITING_GUIDE.md:20-51`) forbids NV access and `TL_ZB_TIMER` scheduling *before
`stack_init()` returns*.

- `moes_livenessBootedOnNetwork()` (`light/moes_liveness.c:88-92`): sets `s_armed`, then
  `moes_livenessEnsureTimer()` → `TL_ZB_TIMER_SCHEDULE(...)`. Called from
  `zbdemo_bdbInitCb` (`light/zb_appCb.c:155`), only on the `joinedNetwork==1` branch.
- `moes_livenessJoined()` (`moes_liveness.c:97-101`): sets `s_armed`, zeroes `s_silence`,
  `EnsureTimer()`. Called from `zbdemo_bdbCommissioningCb` SUCCESS (`zb_appCb.c:214`).
- `moes_livenessStackActivity()` (`moes_liveness.c:107-110`): RAM write only. Called at the top of
  every commissioning callback (`zb_appCb.c:205`).

Both callback contexts are `bdb_task` (task loop, after `user_init` returns), which is *after*
`stack_init()` returns. They do not read/write NV on the arm path (only
`moes_livenessSampleCb`'s trip calls `moes_resetSkipNextBoot()` → `nv_flashWriteNew`, and that is in
a task-context timer callback, also after `stack_init()`). So the build-07 hooks **satisfy §0**.

This also matches `bughunt/liveness_no_fire.md` §5, which already concluded the arm path is §0-clean.
The §0-violation leading theory is therefore **refuted on the code**.

---

## 5. Non-build-07 causes, each evaluated against the CSMA evidence

The CSMA slot being armed is the hard constraint: any candidate must explain a CSMA attempt *and* the
boot never announcing/joining.

### 5.1 NV corruption → `nv_init`/`zb_info_load` hang inside `zb_init` — REFUTED (as the full story)

If `nv_init`/`zb_info_load` hung inside `zb_init`, the boot would stop before
`moes_rescueBootCheck()` (`s_failCnt=0`, matching one observation) **and before any CSMA**. It cannot
produce the armed slot. NV-corruption can explain a *silent no-CSMA* boot, but not this state.

### 5.2 Primed staging / bootloader install leaving flash in a bad state — partially REFUTED, redirects the question

If the bootloader *had* installed build 07 and build 07 ran, the slot would be `0x847494→0x21cb9`.
It is `0x84748c→0x21bf1` (build 06). So the app slot is build 06 — either the install did not run,
or it ran and left the app slot unchanged (failed/unrecognized flag). This is a *staging/bootloader*
question, not an app boot-path hang. The staged bytes themselves cannot arm a build-06 CSMA slot.

### 5.3 Factory-reset gesture misfire — REFUTED

`factoryRst_init()` (`common/factory_reset.c:94-136`) counts **every** reset that reaches it (POR,
watchdog, soft — there is no reset-source test), increments `factoryRst_powerCnt`, and schedules the
2 s clear timer. Two reasons it cannot be the cause here:

1. `factoryRst_init()` is at `tuyaLight.c:418`, *after* `moes_rescueBootCheck()` (`:410`). A boot that
   hangs before the rescue check never reaches it, so no count accumulates and no factory-reset timer
   exists.
2. The threshold is `FACTORY_RESET_POWER_CNT_THRESHOLD = 3` (`factory_reset.c:31`, `rstnum:3`); the
   17:20 reset was a single pulse. Even if counted, one count is not a factory reset.

### 5.4 600 ms watchdog boot loop at a pre-check point — REFUTED (incoherent with the CSMA slot)

The parent's proposed loop ("hang at the same pre-check point each iteration → all-zero rescue state
+ re-armed CSMA slot") does not hold together:

- A **pre-check** hang is inside `zb_init`, which does no CSMA (§2). Each iteration would leave the
  slot zeroed, not armed.
- A **post-check** hang (in the rejoin scan) would run `moes_rescueBootCheck()` every iteration and
  increment `s_failCnt`; after 6 iterations (a few seconds at 600 ms) it would latch
  `s_rescue=TRUE` and park `s_failCnt` at `MOES_RESCUE_FAIL_THRESHOLD` (`moes_rescue.c:63-69`). That
  produces `s_failCnt=6 / s_rescue=1`, not `0/0`.

So no watchdog-loop variant yields `s_failCnt=0` **and** an armed CSMA slot.

### 5.5 The coherent story: build 06 re-entered the rejoin-scan wedge during the boot rejoin

This is the top candidate and is fully consistent with all hardware facts:

1. 17:20 reset → bootloader does **not** install build 07 → app slot still build 06.
2. Build 06 crt0 zeroes `.bss` (matches "bss zeroed").
3. `user_init` → `stack_init()` returns → `moes_rescueBootCheck()` runs, `s_failCnt@0x842471 = 1`
   (or `nv+1`), `s_rescue@0x842478 = 0`.
4. `bdb_init` sees credentials (`nodeIsOnANetwork=1`) → `bdb_routerStart` → rejoin beacon-request
   scan → `mac_csmaStart` (build 06, `0x21d4c`) arms the slot `0x84748c ← 0x21bf1` with a fresh
   deadline.
5. The scan wedges at channel 24 before the rejoin completes (the exact freeze traced in
   `bughunt/mac_scan_wedge.md` §2) — so `joined` is still 0, and there is no announce and no join.
6. The main loop no longer returns (`mac_scan_wedge.md` §3), and the 600 ms watchdog does not fire
   (silicon defeat), so the device sits silent with `s_failCnt` frozen at its single-boot value.

This is the *same wedge class* the bench unit already exhibited, just entered at the boot-rejoin
stage instead of the post-join parent-loss stage. `mac_scan_wedge.md` §5 even predicted the "boots
straight into the rejoin scan and wedges before it can get on-air" variant.

The only observation that does not fit this story is `s_failCnt=0` / `s_armed=0` — and §1 explains
those as build-07 addresses read against a build-06 image.

---

## 6. Ranked candidates

| # | Candidate | Consistency with observed SRAM | Verdict |
|---|---|---|---|
| **1** | **Build 06 still running; rejoin-scan wedge at boot-rejoin** | slot `0x84748c→0x21bf1` (exact b06 pair), bss zeroed, no announce/join, watchdog no-fire | **Top candidate** |
| 2 | Build 07 running, hung inside `zb_init` before the rescue check | would give `s_failCnt=0`/`s_armed=0`, but slot would be `0` at `0x847494` (no CSMA) | REFUTED by the armed b06 slot |
| 3 | Build 07 running, rejoin scan wedged | slot would be `0x847494→0x21cb9`, `s_failCnt@0x842479>=1` | REFUTED by the b06 slot pair |
| 4 | Build-07 §0 violation (liveness timer before `stack_init`) | hooks run in `bdb_task`, after `stack_init`; also §0 bug would not arm CSMA | REFUTED on code |
| 5 | NV corruption hang in `nv_init`/`zb_info_load` | no CSMA possible | REFUTED by armed slot |
| 6 | Factory-reset gesture | `factoryRst_init` after rescue check; threshold 3 vs 1 reset | REFUTED |
| 7 | 600 ms watchdog loop at pre-check point | pre-check = no CSMA; post-check = `s_failCnt`/`s_rescue` latch | REFUTED |
| 8 | Partial bootloader install (mixed b06/b07 app) | remotely possible, but the clean b06 slot pair argues the MAC code is intact b06 | Unlikely; confirm via flash `0x8000` read |

---

## 7. Reconciliation, stated plainly

There is no ordering contradiction. "CSMA happened but the rescue check never ran" is impossible in a
single coherent image, because `moes_rescueBootCheck()` (`tuyaLight.c:410`) is the first statement
after `stack_init()` (`:396`), and the CSMA path starts at `bdb_init` (`:463`) → `bdb_routerStart`
(`bdb.c:1645`), which is strictly later. The apparent contradiction is an artifact of mixing build-06
and build-07 symbol tables on one SRAM dump:

- the CSMA slot was read at its **build-06** address and value (`0x84748c→0x21bf1`);
- `s_failCnt`/`s_armed` were read at their **build-07** addresses (`0x842479`/`0x842470`), which in a
  build-06 image are `s_rescue`-padding / `s_minsLeft` and read 0.

Read every symbol at its build-06 address and the picture becomes the ordinary build-06 wedge:
`s_failCnt@0x842471 >= 1`, `s_rescue@0x842478 = 0`, CSMA slot armed, `joined=0`, no announce.

---

## 8. Stack-capture confirmation key (for the next full-SRAM read)

The imminent full-SRAM capture should settle candidate #1 vs #2 in one look. Read **both** build
address maps, not one.

**A. Determine the running build (highest-value bytes):**

| check | build 06 | build 07 |
|---|---|---|
| MAC TX-timeout slot base | `0x84748c` → expect `0x21bf1` (armed) | `0x847494` → expect `0x21cb9` (armed) or `0` (never reached CSMA) |
| `macMaxCSMABackoffs` (g_zbInfo `+0x3e`) | `0x847156` → `0x04` | `0x84715e` → `0x04` |
| `g_zbNwkCtx` joined byte (`+0x2d`) | `0x8474c5` bit 2 | `0x8474cd` bit 2 |
| `s_failCnt` | `0x842471` | `0x842479` |
| `s_armed` | (n/a) | `0x842470` |

**B. If build 06 (candidate #1):** expect the same stack/return chain as
`mac_scan_wedge.md` §2, but PC parked in the boot-rejoin scan:

```
rf_setTrxState+0x48 / rf_rx_irq_handler+0x20
mac_csmaStart+0x76   (build-06 0x21dc2)
mac_trxTask+0x292    (build-06 0x2225e)
tl_zbMacTx+0xc4      (0x223ec)
tl_zbMacMlmeBeaconRequestCmdSend+0x72
tl_zbMacScanRunning+0xcc
ev_timer_update / tl_zbNwkTaskProc / tl_zbTaskProcedure / main
```

with `s_failCnt@0x842471 >= 1`, `s_rescue@0x842478 == 0`, `g_zbInfo@0x847118`, slot
`0x84748c == 0x21bf1` + a fresh deadline, scan channel index `0x8421e4 == 0x18`.

**C. If build 07 actually did install and run (would refute §1):** then `0x84748c` must be `0`, the
real slot is `0x847494`, and:

- `s_failCnt@0x842479 == 0` **and** `s_armed@0x842470 == 0` **and** slot `0x847494 == 0` → hung inside
  `zb_init` (candidate #2); stack dump PC would be inside `zb_init`/`zb_info_load`/`tl_zbMacInit`/
  `tl_zbNwkInit` with **no** `mac_csmaStart` frame.
- `s_failCnt@0x842479 >= 1` **and** slot `0x847494 == 0x21cb9` → build 07 rejoin scan wedged
  (candidate #3), same rejoin-scan chain but with build-07 PC addresses (`mac_csmaStart 0x21e14`,
  `mac_trxTask` shifted accordingly).

**D. Which build is in flash (independent of SRAM):** the other agent's `rf 0x8000 0x100` read plus
the `APP_BUILD` byte location in `common/version.h` (`0x06` vs `0x07`) settles the install question
directly; a CRC of the `0x8000` app image against `light_TS0505B.bin` (build 07) or
`moes-conv-b06.zigbee` (build 06) is the definitive tie-breaker.

---

## 9. Inferred / unverified, stated plainly

- "The bootloader did not install build 07" is **INFERRED** from the SRAM slot evidence (build 06
  code armed the slot). I did not read flash; the mechanism (flag not recognized, reset vector, CRC,
  or install not committed) is not established here.
- The tail of `bdb_routerStart → zb_routerStart → rejoin → beacon-request → CSMA` is inside the
  prebuilt `libzb_router.a`; the claim that this is the *only* CSMA path before the app is joined is
  **INFERRED** from the visible `zb_init` body (no TX) plus the call graph, not from re-disassembling
  the whole prebuilt lib.
- Build-06 symbol addresses are taken from `bughunt/mac_scan_wedge.md` / `bughunt/idle_parent_loss.md`
  (which read the build-06 ELF). They were not independently re-verified against a build-06 ELF here
  (no build-06 ELF is present in `build/`); the build-06/build-07 `+8`-byte `.bss` and
  `+0xc8`-byte `.text` shifts are consistent across every symbol checked.
- The "`s_failCnt=0` / `s_armed=0` are misreads" claim is **INFERRED** from the address-map mismatch
  and the impossibility of a single-build CSMA-before-rescue-check ordering; it should be confirmed
  by the §8 two-map read before any firmware action.

---

## 10. Follow-up (do not act here — this is analysis only)

1. Confirm running build via the §8 two-map SRAM read and the flash `0x8000` read.
2. If build 06 is confirmed, the immediate problem is **not** the boot path — it is that the OTA
   install did not happen. That points back at `bughunt/ota_no_install.md` / the uncommitted build-08
   `ota_mcuReboot` fix, and at the bootloader's staging-flag semantics. The device's silence is the
   known build-06 rejoin-scan wedge, now occurring at boot-rejoin.
3. Do not reset the device again before the full-SRAM capture; another reset would destroy the
   wedge state.
