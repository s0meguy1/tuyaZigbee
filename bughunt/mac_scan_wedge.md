# mac_scan_wedge — build 06 bench light goes silent ~70–95 s after a successful join

**Branch:** `moes-ts0505b`, build 06 (`da0921b` + `b06109c`), `MODULE_WATCHDOG_ENABLE 1` at 600 ms.
**Evidence:** two SWire SRAM captures 3 min apart (`dump/bench_2026-08-15/hang_capture/sram_1.bin`, `sram_2.bin`),
plus `nv.bin`, `wd_regs.bin`, `rst_regs.bin`. Read-only analysis; nothing committed to firmware.

## Verdict up front

The device is **deterministically wedged in a MAC scan (beacon-request) path, frozen at scan
channel 24**, not in an unbounded CSMA/CCA spin inside `mac_csmaStart` itself. The MAC TX/CSMA
state machine is bounded everywhere we can read it; the wedge is a higher-level **scan that
stops advancing**, with the CPU stuck in the channel-24 beacon-request/CSMA call chain.

The 600 ms watchdog **does not recover it**: the SRAM rescue-state variables prove the device has
**not** been through a watchdog reset loop. `s_failCnt == 1` and `s_rescue == 0` means the chip
booted once and stayed hung — a reset loop would have parked `s_failCnt` at the threshold (6) and
latched rescue mode within seconds. The safety-net hole is therefore real in two layers: the
watchdog does not fire on this wedge, and even if it did, rescue mode uses the same rejoin scan
and would wedge before it can get on-air.

---

## 1. MAC CSMA / trx wait structure — bounded, not a spin

All symbols are from `build/light/light_TS0505B` (ELF, not stripped). MAC CSMA/trx is in the
prebuilt `libzb_router.a`; only `mac_pib.c` and `mac_phy.c` are in source, so the following is
from `tc32-elf-objdump`.

### `mac_csmaStart` (`0x21d4c`)

```
21d4e  drv_disable_irq()            ; IRQs OFF for the CCA+TX critical section
21d54  rf_performCCA()              ; single fixed-duration CCA sample
21d58  tcmp r0, #4 ; tjne 21dc4     ; r0==4 => channel clear => transmit
21d5c  ... rf802154_tx()            ; kick TX (non-blocking)
21d84  drv_restore_irq()
21daa  [0x84748c+0] = 0x21bf1       ; arm mac_waitTxIrqCb (TX-done timeout)
21dae  mac_currentTickGet()
21dbc  [0x84748c+4] = now + 10000*unit
21dc0  [0x84748c+8] = 1
21dc2  return -1
21dc4  ; CCA busy path:
21dc8  r3 = [0x847118+0x3e]         ; macMaxCSMABackoffs (SRAM value = 0x4)
21dcc  tjeq 21d5c                   ; if 0 => transmit anyway (retry)
21dd4  tl_zbTaskPost(0x21fcd, 2)    ; else post mac_trxTask event 2, return -1
```

`rf_performCCA` (`0x958`) is the only loop and it is **fixed-duration**:

```
980   r3 = [0x740] - r2             ; 0x740 = reg_system_tick (hardware counter)
984   tcmp r3, #2048
986   tjls 974                      ; loop while elapsed <= 2048 ticks
```

It samples RSSI and averages over ~2048 system ticks, then returns 4 (clear) or 0 (busy). The
loop bound is a hardware counter (`reg_system_tick`, `register.h:809`), so it terminates even
with IRQs disabled.

### `mac_trxTask` (`0x21fcc`)

Event-2 (busy) handling is the CSMA backoff:

```
2200e  r3 = [r4+6]                  ; attempt counter
22012  r1 = r3+1 ; [r4+6] = r1
22016  tcmp r2( max=0x4 ), r3
22018  tjls 2201e                   ; counter exhausted => channel-access-failure
2201c  tj   22194                   ; else retry CSMA
22194  rf_TrxStateGet/set
221e6  random backoff = rand % (2^NB - 1) * 320us
221b0  drv_hwTmr_set(3, delay, mac_csmaStart, txdesc)   ; ONE-SHOT hw timer 3
```

The backoff retry is bounded by `macMaxCSMABackoffs` (`PIB+0x3e`, SRAM value `0x4`). The hardware
timer callback is `mac_csmaStart` and `drv_hwTmr_irq_process` stops the timer after the callback
returns `< 0` (one-shot). `mac_waitTxIrqCb` (`0x21bf0`) and `mac_ackWaitingTimerCb` (`0x21c24`)
are bounded recovery timers that force the radio back to RX and post the next trx event.

**Conclusion:** there is no unbounded CCA-clear or TX-done wait in any of these functions.

---

## 2. Where it is actually stuck — the scan is frozen at channel 24

Both SRAM captures are identical in every MAC/scan state word:

| field | address | sram_1 | sram_2 |
|---|---|---|---|
| scan timer task id | `0x847478` | `0xc46e74` | `0xc46e74` |
| scan channel index | `0x8421e4` | `0x18` (24) | `0x18` (24) |
| scan current channel | `0x847478+13` | `0x18` | `0x18` |
| scan state | `0x847478+16` | `0x00` | `0x00` |
| radio state | `0x842550` | `0x01` (RX) | `0x01` |
| TX state | `0x8426dc+4` | `0x00` | `0x00` |
| MAC pending active | `0x84748c+8` | `0x00` | `0x00` |

The return-address chain on the stack is also byte-identical; the only SRAM bytes that differ
between captures are ~45 scattered data/bss bytes (bit-5 toggles, i.e. live timer/tick state) and
two stack locals at `0x84fde8`/`0x84fe1c` (a slow counter, `+0x20` in 3 min). The stack chain
(symbolized, deepest → outermost) is:

```
rf_setTrxState+0x48 / rf_rx_irq_handler+0x20
mac_csmaStart+0x76            (0x21dc2)
mac_trxTask+0x292            (0x2225e)
(tl_zbSwitchOffRx region)     (0x222f2)
tl_zbMacTx+0xc4              (0x223ec)
tl_zbMacMlmeBeaconRequestCmdSend+0x72 (0x21906)
tl_zbMacScanRunning+0xcc / +0x1 (0x219e4 / 0x21919)
ev_timer_update+0x14          (0x1f3f0)
tl_zbNwkTaskProc+0xa2        (0x232f2)
tl_zbTaskProcedure+0x24      (0x1fc64)
main+0x34                    (0x1f620)
```

Interpretation: the CPU is **stuck inside the channel-24 beacon-request path**; the scan timer
(`tl_zbMacScanRunning` scheduled via `ev_timer_taskPost` at `tl_zbMacScanRequestHandler:21ba4`)
never advances past channel 24. The radio is idle-RX with no pending TX, so this is not a
radio-stuck-in-TX state; it is the scan state machine / timer path that is wedged.

---

## 3. Would the 600 ms watchdog catch it? No — evidence says it did not

Firmware side (all unconditional in the built ELF):

- `main()` (`apps/common/main.c:64-67`) calls `drv_wd_setInterval(600)` then `drv_wd_start()`.
- `drv_wd_clear()` is `apps/common/main.c:88` and `:94`, i.e. only between `ev_main()` and
  `tl_zbTaskProcedure()` and after it. Disassembly confirms the only `drv_wd_clear` call sites are
  the two in `main` plus the flash driver (`drv_flash.c:136/155/186`) and the OTA CRC loop
  (`ota.c:164`). **No IRQ handler calls it.**

Rescue-state variables in SRAM are decisive:

| variable | address | value |
|---|---|---|
| `s_failCnt` | `0x842471` | `0x01` |
| `s_rescue` | `0x842478` | `0x00` |
| `s_minsLeft` | `0x842470` | `0x00` |
| `s_stableTimer` | `0x842474` | `0x00000000` |

`moes_rescueBootCheck()` (`light/moes_rescue.c:38`) runs after `stack_init()` on every boot and
sets `s_failCnt = nv_cnt + 1`. `s_failCnt == 1` means the NV probation counter read as **0** at
this boot. If the 600 ms watchdog had reset the chip even once, the next boot would read ≥1 and
`moes_rescueBootCheck` would park `s_failCnt` at ≥2; after 25+ minutes of a 600 ms loop it would
be `6` and `s_rescue` would be `TRUE`. It is not. **The watchdog did not reset the device.**

Why it does not fire on this wedge is a hardware/silicon question rather than a firmware-logic
question: the watchdog is enabled in firmware, is not serviced from any IRQ, and the main loop's
`drv_wd_clear()` at `main.c:88` is not reached once the scan stops advancing. The most probable
explanation is that the TLSR8258 watchdog (Timer2 in WD mode) is being defeated by the same
timer-register activity the CSMA path performs (`drv_hwTmr_set`/`timer_set_mode` touch
`reg_tmr_ctrl`/`reg_tmr_sta`), or the watchdog counter is not running on this bench part. What the
SRAM proves is the **outcome**: a permanent hang, not a 600 ms reset loop.

`z2m` sees zero traffic (no rejoins) exactly because the CPU never gets a frame out after the
first wedge; CCA/CSMA never successfully transmits, and there is no reset that would produce a
fresh rejoin attempt.

---

## 4. What fires 60–95 s after join, and what triggers the new scan

App timers scheduled after a successful fresh join+configure:

| timer | source | period | fires in window? |
|---|---|---|---|
| heartbeat (`heartInterval`) | `zb_appCb.c:199` | 1000 ms | no-op (`DEBUG_HEART 0`) |
| OTA periodic query | `zb_appCb.c:221` | `OTA_PERIODIC_QUERY_INTERVAL` = 5 min | **no** (300 s) |
| rescue stable timer | `zb_appCb.c:211` → `moes_rescue.c:147` | 60 s | **yes (60 s)** |
| attribute-store debounce | `tuyaLight.c:276` | 1 s | continuous, only writes when dirty |
| find-and-bind | `zb_appCb.c:227` | 1 s | fires at ~1 s |
| colour/level/identify | various | 100 ms / 1 s | only on commands |

The only *new* thing in the 60–95 s window from our code is the **rescue stable timer's first
tick at 60 s** (`moes_rescueStableTimerCb`, `moes_rescue.c:111`). It is read-only in the joined
case (`zb_isDeviceJoinedNwk()` + decrement) and does not issue a scan; its only side effect is a
`nv_flashWriteNew` at 20 min via `moes_rescueClear()`.

The scan itself is therefore triggered by the **network layer, not our app**. The stack path
(`tl_zbNwkTaskProc` → scan-request handler → `tl_zbMacScanRunning`) is the nwk-layer rejoin path.
`nwk_rejoinScanCnfHandler` (`0x2715c`) calls `nwk_rejoinReq` again on a failed scan, and
`nwk_rejoinReq` (`0x26ce4`) re-enters the scan — this is the rejoin retry loop. The trigger ~70 s
after join is **parent link-loss detection**: the router stops receiving link status from its
parent (`ZB_NWK_LINK_STATUS_PERIOD_DEFAULT 15` s in `nwk_nib.h:57`, aging in
`tl_zbNwkLinkStatusTimerEvtCb`/`tl_zbNwkNeighborTabAging`), declares `BDB_COMMISSION_STA_PARENT_LOST`
(`bdb.c:1261`), and issues a rejoin scan.

Why the parent appears lost on a bench with strong signal is the remaining unknown; the radio is
idle-RX at capture time, so RX was at least superficially alive. The wedge is the scan state
machine failing to advance past channel 24, not the initial link-loss detection itself.

### Fresh-join announce context

Our `zb_zdoSendDevAnnance()` in `zbdemo_bdbCommissioningCb` (`zb_appCb.c:207`) runs in a BDB task
callback (normal task context, not IRQ). `zb_zdoSendDevAnnance` (`0x3605c`) just calls
`zdo_device_announce_send` and returns; the SDK itself calls it from the GP-proxy command handler
(`gp_proxy.c:340`) in a similar task context. It is a normal buffered TX and is **not** an obvious
unsafe context — it is not the wedge trigger, but it is one more TX in the same first-minute
window.

---

## 5. Rescue-mode consequence — the safety net hole is real

`moes_rescueBootCheck` (`light/moes_rescue.c:38`) runs every boot and only latches rescue mode
when the probation counter reaches `MOES_RESCUE_FAIL_THRESHOLD` (6). The SRAM shows `s_failCnt == 1`
and `s_rescue == 0` after 25+ minutes: **the watchdog never resets the device, so the probation
counter never advances and rescue mode never latches.**

Even if the watchdog did fire and the counter reached 6, rescue mode would still need to join.
It does so through the **same BDB steering/rejoin path** (`zbdemo_bdbInitCb:144/168` →
`bdb_networkSteerStart` / the stack's rejoin), which uses the **same** `tl_zbMacScanRunning`
beacon-request scan that is wedged. A latched rescue light with NV ("joined") boots straight into
the rejoin scan and wedges before it can get on-air.

So both halves of the build-06 safety net fail on this wedge class:

1. the watchdog does not convert the hang into a reboot loop (evidence: `s_failCnt == 1`), and
2. rescue mode cannot get on-air because its join uses the same wedged rejoin scan.

---

## 6. Top suspects / follow-up

- **MAC scan state machine freeze at channel 24** (prebuilt, `tl_zbMacScanRunning` / scan timer
  re-arm). The scan timer task exists (`0xc46e74`) but never advances. This is the direct wedge;
  source is unavailable so the exact defect cannot be localized further without a hardware trace.
- **Watchdog defeat by timer-register activity in the CSMA path — REFUTED.** See
  `bughunt/watchdog_defeat.md` for the register-level check. The CSMA backoff uses
  `drv_hwTmr_set(3,...)` = `TIMER_IDX_3` (system timer), writing only `reg_system_tick_irq`
  (`0x744`) and `reg_irq_mask` (`0x640` bit 20); it never touches the watchdog's Timer2 registers
  (`reg_tmr_ctrl` `0x620`, `reg_tmr_sta` `0x623`, `reg_tmr2_tick` `0x638`). `timer_set_mode` /
  `timer_start` / `timer_stop` (the only other `reg_tmr_ctrl` writers) are unreachable dead code
  in the linked ELF. The non-fire of the 600 ms watchdog is therefore a **logical stall** (main
  loop still returns and feeds `drv_wd_clear()`) or a silicon issue, not a CSMA register clobber.
- **Parent link-loss / rejoin trigger.** Determine why the bench router stops hearing its parent
  ~70 s after a successful configure. Capture z2m-side link-status traffic around the death window
  and compare against the device's `tl_zbNwkLinkStatusTimerEvtCb`/neighbor-aging state.

No firmware change is proposed here; this document is analysis only.
