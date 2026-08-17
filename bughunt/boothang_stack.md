# boothang_stack — 18:02 boot hang is a NEW APS/NWK link-status wedge, and s_failCnt=0 is moes_rescueClear(), not "the check never ran"

> ## Superseded certainty notice (2026-08-17)
> The flasher did not record a halted PC/SP. This document's stack reconstruction
> is therefore inference, not proof of a new APS/NWK wedge site. The 17:41
> capture also reset before SRAM acquisition; later preserve-state reads only
> show post-reset state. See `VERIFICATION_STATUS.md` for the current boundary.

**Branch:** `moes-ts0505b`, running image = BUILD 06 (`6464-0395-11063003`, stripped payload 201956 B).
**Evidence:** `dump/bench_2026-08-16_boothang/sram_pass1.bin` (65536 B @ 0x840000), `sram_pass2.bin`,
build-06 raw disassembly (`tc32-elf-objdump -b binary --adjust-vma=0x8000`), build-08 ELF symbols
(`build/light/light_TS0505B`) with the verified **+0xc8 text shift**, and the prior wedge
(`dump/bench_2026-08-15/hang_capture/sram_1.bin` + `bughunt/mac_scan_wedge.md`). No code changed, no
flashing, no git mutations. Pi log scp read-only; the flasher logs do **not** record a halted PC.

---

## Verdict up front

1. **The boot did NOT hang before `moes_rescueBootCheck()`.** The stack contains the return address
   `0x1f618` = `main` calling `ev_main` (the main loop). Reaching `ev_main` requires `user_init()` to
   have returned, and `moes_rescueBootCheck()` runs at `tuyaLight.c:410` inside `user_init` before it
   returns. Build-06 machine code for `moes_rescueBootCheck` (`0x9fe8`) sets `s_failCnt >= 1` on every
   path (verified below). So the check ran and then `s_failCnt` was **cleared back to 0**.

2. **`s_failCnt@0x842471 = 0x00` is the end state of `moes_rescueClear()`, reached through the
   20-minute rescue stable timer.** `s_minsLeft@0x842470 = 0` and `s_stableTimer@0x842474 = NULL` are
   the exact post-completion signature of `moes_rescueStableTimerCb` clearing the counter (machine code
   below). The device considered itself joined (joined byte `0x8474c5 = 0xc4`, bit2 set), so the timer
   counted down and cleared rather than parking at `s_minsLeft = 20`.

3. **The hang is a NEW site, not the `mac_scan_wedge`.** The prior wedge's stack is the MAC
   beacon-request/scan chain (`mac_csmaStart+0x76`, `mac_trxTask+0x292`, `tl_zbMacTx`,
   `tl_zbMacScanRunning`, `ev_timer_update` …). The 18:02 stack is the **NWK link-status / APS
   security / ZCL-report data path** (`tl_zbNwkLinkStatusCmdHandler`, `tl_zbMacMcpsDataIndicationHandler`,
   `ss_nwkSecureFrame`, `ss_apsConfirmKeyCmdHandle`, `apsTxEventPost`, `aps_interPanDataIndCb`,
   `aps_nwk_addr_req_cb`, `zb_bindingTblSearched`, `reportAttr`). No scan timer is armed
   (`0x847478 = 0` vs `0xc46e74` in the prior wedge); the device is processing traffic, not scanning.

4. **The announce path is NOT implicated; the ZCL-report path is.** `0x3605a` is *not*
   `zb_zdoSendDevAnnance` (`0x3605c`) — it is the return address into `zb_bindingTblSearched`
   (`0x3604c`), the binding-table lookup used by `reportAttr`. `0x118f8`/`0x11df4` are `reportAttr` /
   `reportAttrTimerStart` (ZCL reporting), **not** flash/NV/SPI drivers.

---

## 1. Decisive SRAM bytes (file offset = addr − 0x840000)

| field | address | value | meaning |
|---|---|---|---|
| s_failCnt | `0x842471` | `0x00` | cleared (see §4) |
| s_minsLeft | `0x842470` | `0x00` | stable timer completed / never restarted |
| s_stableTimer | `0x842474` | `0x00000000` | timer done (callback NULLs it) |
| s_rescue | `0x842478` | `0x00` | not latched |
| joined byte | `0x8474c5` | `0xc4` | bit2 set = joined-context restored |
| MAC TX-timeout slot cb | `0x84748c` | `0x00021bf1` | build-06 `mac_waitTxIrqCb` |
| slot deadline | `0x847490` | `0xe22a0638` | changed vs prior `0xdd1105e0` (fresh CSMA) |
| slot armed flag | `0x847494` | `0x00` | timeout already fired (cb+deadline are stale leftovers) |
| scan timer task id | `0x847478` | `0x00000000` | **no active scan** (prior wedge: `0xc46e74`) |
| scan/current chan | `0x8421e4` | `0x0b` (11) | operating channel, not frozen-at-24 |
| radio state | `0x842550` | `0x01` | RX |

The MAC slot is the normal **post-timeout** shape: `mac_csmaStart` wrote cb+deadline and set `+8=1`;
`mac_waitTxIrqCb` later cleared `+8`, leaving cb/deadline as stale history. So "a CSMA ran and then its
TX-done timeout fired" — not "armed and waiting".

---

## 2. Symbolized stack (build-06 → build-08 = +0xc8 for all SDK/prebuilt code)

Real saved return addresses were distinguished from pointer/locals by checking that `V-4` is a `tjl`.
Only 5 of the 13 candidates are real return addresses; the rest are function pointers/locals.

### 2a. Confirmed return-address chain (oldest → newest)

| SRAM addr | value (b06) | returns into | call site (b06) |
|---|---|---|---|
| `0x84fff4` | `0x1f618` | `main` (after `tjl ev_main`) | `0x1f614` → `ev_main 0x1ecc0` |
| `0x84ffe4` | `0x1f032` | `ev_poll_process` (after `tjl 0x1f038`) | `0x1f02e` → small ev helper |
| `0x84ff74` | `0x3605a` | `zb_bindingTblSearched` (after `tjl 0x30644`) | `0x36056` → `aps_bindingTblMatched` |
| `0x84ff4c` | `0x257f0` | `tl_zbMacMcpsDataIndicationHandler` (after `tjl 0x22cf8`) | `0x257ec` → `tl_zbNwkLinkStatusCmdHandler` |
| `0x84ff00` | `0x22d2e` | `tl_zbNwkLinkStatusCmdHandler` (after `tjl 0x1f6a8`) | `0x22d2a` → `zb_buf_free` |

Callee identity of the last three (via `+0xc8` shift → build-08 symbols):
`0x30644 → aps_bindingTblMatched`, `0x22cf8 → tl_zbNwkLinkStatusCmdHandler`, `0x1f6a8 → zb_buf_free`.

### 2b. Non-return code values in the deep frame (function pointers / locals)

| SRAM addr | value (b06) | build-06 → build-08 name |
|---|---|---|
| `0x84fdf8`, `0x84fe14` | `0x2eeee` (×2) | inside `apsTxEventPost` |
| `0x84fe40` | `0x2edf2` | inside `aps_interPanDataIndCb` |
| `0x84fe98` | `0x2f038` | inside `aps_nwk_addr_req_cb` |
| `0x84febc` | `0x2c644` | literal pool of `ss_apsConfirmKeyCmdHandle` |
| `0x84fee4` | `0x2c8d0` | inside `ss_nwkSecureFrame` |
| `0x84ff78` | `0x118f8` | inside `reportAttr` |
| `0x84ffc0` | `0x11df4` | inside `reportAttrTimerStart` |

Deepest 552-byte used-stack extent: first non-`0xff` word `0x84fdd4`, top `0x84fffc`; below is the
unwritten `0xff` stack region. The deep region (`0x84fdd4`–`0x84fee4`) is a large local struct/array
holding APS code pointers + `0x00c4xxxx` stack pointers — i.e. the frame of the code currently running
in the APS layer.

**Readout:** a ZCL attribute report is being built/sent (`reportAttrTimerStart`/`reportAttr` →
`zb_bindingTblSearched` → `aps_bindingTblMatched`), and concurrently a MAC data indication delivered a
NWK link-status command (`tl_zbMacMcpsDataIndicationHandler` → `tl_zbNwkLinkStatusCmdHandler` →
`zb_buf_free`), with the active frame parked in the APS security/data layer
(`ss_nwkSecureFrame`/`ss_apsConfirmKeyCmdHandle`/`apsTxEventPost`).

---

## 3. The contradiction resolved

The parent's triangle:

- (a) `s_failCnt = 0` ⇒ "`moes_rescueBootCheck` never ran";
- (b) a fresh CSMA ran (slot deadline changed post-reset);
- (c) source order says CSMA (`bdb_init`, `:463`) runs after the check (`:410`).

All three are true **at capture time**, but (a) is being misread. Build-06 machine code
(`0x9fe8`, disassembled from the stripped payload):

- NV-read-fail path → `s_failCnt = 1` (`0xa008`–`0xa012`), then `moes_probationWrite(1)`.
- NV-ok, `cnt <= 6`, `cnt != 6` → `s_failCnt = cnt` (`0xa032`) then `s_failCnt = cnt + 1`
  (`0xa00c`–`0xa012`).
- NV-ok, `cnt == 6` → `s_failCnt = 6`, `s_rescue = 1`, early return (`0xa028`–`0xa02e`).
- NV-ok, `cnt > 6` → clamp `s_failCnt = 6`, `s_rescue = 1` (`0xa022`–`0xa02e`).

Every path leaves `s_failCnt >= 1`. The literal at `0xa03c` is `0x00842471` = `0x842471`, so this is
unambiguously build-06's `s_failCnt`. Therefore `s_failCnt = 0` at capture time cannot mean "the check
never ran" once the stack also proves the boot reached `ev_main` (which is *after* `user_init` returns).

The only writer of `s_failCnt = 0` is `moes_rescueClear` (`0xa050`, literal `0xa068 = 0x842471`):
it stores `0` then `moes_probationWrite(0)`. Its two call sites are
`moes_rescueStableTimerCb` (`0xa06c`) and `factoryRst_handler` (`factory_reset.c:87`).

`moes_rescueStableTimerCb` machine code (`0xa06c`):

- not joined → `s_minsLeft = 20`, return 0 (`0xa08e`);
- joined, `s_minsLeft > 0` → decrement; when it reaches 0 → `moes_rescueClear()`,
  `s_stableTimer = NULL`, return −1 (`0xa096`–`0xa0a4`).

The captured `s_minsLeft = 0` **and** `s_stableTimer = NULL` **and** `s_failCnt = 0` is exactly the
completed-timer end state. If the timer had never started (`s_failCnt == 0` at join), `s_failCnt` could
not have been 0 at join (the check sets `>= 1` first and no clear path runs before join). So the timer
started, the device was joined for 20 minutes, and `moes_rescueClear()` zeroed the counter. The joined
byte `0x8474c5 bit2` confirms the NWK-level join; the stable timer's `zb_isDeviceJoinedNwk()` test is
satisfied even if the ZDO device-announce never reached z2m — which explains "silent/no-announce"
simultaneously with a cleared `s_failCnt`.

The fresh CSMA (b) is then ordinary: the boot ran `bdb_init → bdb_routerStart → rejoin → beacon
request → mac_csmaStart` after the check, exactly as (c) says. No ordering contradiction exists.

---

## 4. Where the CPU is hung

No halted-PC register value exists in the local or Pi flasher logs (`phase3_*.log`, `run_*.log` only
record read commands; `rst_regs` is marked probably-unreadable). The deepest stack frame
(`0x84fdd4`–`0x84fee4`) is therefore the best available pointer: it is the live frame of code in the
**APS layer** (`apsTxEventPost`/`aps_interPanDataIndCb`/`aps_nwk_addr_req_cb` pointers), entered from
`tl_zbNwkLinkStatusCmdHandler → zb_buf_free`. The confirmed chain above it is the NWK link-status →
MAC data-indication → ZCL report/binding → ev_poll → main path.

**Same wedge as `mac_scan_wedge.md`? No.** Prior wedge: `rf_setTrxState → mac_csmaStart → mac_trxTask →
tl_zbMacTx → tl_zbMacMlmeBeaconRequestCmdSend → tl_zbMacScanRunning → ev_timer_update →
tl_zbNwkTaskProc → tl_zbTaskProcedure → main`, frozen at scan channel 24 with an armed scan timer
(`0xc46e74`). 18:02 stack shares none of that; it is data/security processing on a joined device with
**no scan timer** and channel 11 (operating). New wedge site.

**Announce vs driver:** `0x3605a` is the report-path `zb_bindingTblSearched`, not the announce
`zb_zdoSendDevAnnance` (`0x3605c`). `0x118f8`/`0x11df4` are ZCL `reportAttr`/`reportAttrTimerStart`, not
flash/NV/SPI drivers — the driver hypothesis is refuted.

---

## 5. Refuted theories

| theory | why it fails |
|---|---|
| Boot hung inside `stack_init`/`zb_init` before `:410` | stack shows `main → ev_main → ev_poll_process`, which requires `user_init` to return; `zb_init` (`0x1fa04`) calls no CSMA/TX (callees: `tl_zbMacInit`, `tl_zbNwkInit`, `aps_init`, key/attr/af/zdo init) |
| `s_failCnt = 0` = "check never ran" | machine code sets `>= 1` on every path; reaching `ev_main` means it ran |
| Stale stack from the prior `mac_scan_wedge` boot | prior wedge stack is the scan chain, byte-different from this APS chain; the unwritten `0xff` stack region begins just below `0x84fdd4`, and a hardware reset (17:41 RESETB) reloads SP to top |
| Factory-reset gesture cleared `s_failCnt` | requires `factoryRst_powerCnt >= 3` and `factoryRst_handler → zb_factoryReset()`, which would clear the joined flag; joined bit is set |
| `0x118f8`/`0x11df4` = flash/NV/SPI driver hang | they are `reportAttr` / `reportAttrTimerStart` (ZCL) |
| `0x3605a` = announce path | it is `zb_bindingTblSearched` (report binding lookup), 2 bytes before `zb_zdoSendDevAnnance` |
| Slot "armed and waiting" | `+8 = 0`; the TX-done timeout already fired — cb/deadline are stale history |

---

## 6. Inferred / unverified

- That `moes_rescueClear` ran via the 20-min stable timer (rather than some other clear) is **INFERRED**
  from the exact `s_minsLeft=0 / s_stableTimer=NULL / s_failCnt=0` triple plus the set joined flag; the
  current NV probation byte was not read, so it cannot be confirmed from flash here.
- The exact PC is unknown (no register/log value); the "currently in the APS layer" statement is
  **INFERRED** from the deepest frame's contents.
- "The device was joined ~20 min before the clear" is **INFERRED**; it is the only stable-timer path
  that produces the observed triple, but the 17:20 vs 17:41 boot attribution is not settled by this
  capture alone.
- `+0xc8` text shift is verified for the prebuilt SDK/common code (mac_csmaStart, mac_waitTxIrqCb,
  main, tl_zbTaskProcedure, ev_timer_update all `+0xc8`); app-only code (`moes_rescue.c`) shifts
  differently and was instead disassembled directly.

## 7. Follow-up (do not act here)

1. Read the NV probation byte (NV_MODULE_APP item `0x71`): `0` confirms `moes_rescueClear` ran;
   `>= 1` would refute §3 and reopen the "check never ran" question.
2. The actionable defect is the APS/NWK link-status wedge, not the rescue counter. This is a second,
   independent hang class on top of `mac_scan_wedge`; both leave build 06 unrecoverable without OTA.
