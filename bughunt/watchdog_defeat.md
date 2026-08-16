# watchdog_defeat — did the MAC/CSMA timer path clobber the TLSR8258 watchdog?

**Branch:** `moes-ts0505b`, build 06 (`da0921b` + `b06109c`), `MODULE_WATCHDOG_ENABLE 1`, 600 ms.
**Question this doc answers:** can the MAC CSMA path (`drv_hwTmr_set(3,...)` / `timer_set_mode`)
disable or repurpose the watchdog timer that `drv_wd_*` depends on?
**Method:** source read of the SDK watchdog/timer drivers plus `tc32-elf-objdump` of the linked
`build/light/light_TS0505B` ELF. Read-only; no firmware change.

## Verdict: NO — at the register level, the MAC/CSMA timer activity cannot defeat the watchdog

The watchdog and the MAC CSMA backoff use **two different hardware timer blocks with disjoint
register sets**:

| function | hardware block | registers touched |
|---|---|---|
| watchdog (`drv_wd_*`) | **Timer2 in WD mode** | `reg_tmr_ctrl` `0x620`, `reg_tmr_sta` `0x623`, `reg_tmr2_tick` `0x638` |
| MAC CSMA backoff (`drv_hwTmr_set(3,...)`) | **system timer (STIM)** | `reg_system_tick_irq` `0x744`, `reg_irq_mask` `0x640` bit 20, (init only) `reg_system_tick_mode` `0x74c` bit 1 |

There is **no overlap**. `drv_hwTmr_set(3,...)` never calls `timer_set_mode`, and `timer_set_mode` /
`timer_start` / `timer_stop` (the only functions that would touch `reg_tmr_ctrl`/`reg_tmr_sta` for
Timer0/1/2) are **unreachable dead code** in this linked firmware. The premise that the CSMA path
"drives `timer_set_mode` against `reg_tmr_ctrl`/`reg_tmr_sta`" is refuted by the disassembly.

---

## 1. What the watchdog actually uses

`drv_wd_*` is a thin wrapper over `platform/chip_8258/watchdog.h`:

- `drv_wd_setInterval(600)` -> `wd_set_interval_ms(600)` (`watchdog.h:38`)
- `drv_wd_start()`            -> `wd_start()` (`watchdog.h:52`)
- `drv_wd_clear()`            -> `wd_clear()` (`watchdog.h:70`)

The TLSR8258 has **no separate WDT peripheral**. The watchdog *is* **Timer2** repurposed into
watchdog mode. Confirmed by `proj/drivers/drv_timer.h:48`:

```c
#define TIMER_IDX_2   2 //!< Timer2, for Watch dog.
#define TIMER_IDX_3   3 //!< SYS Timer, for MAC-CSMA.
```

The watchdog registers are defined in `platform/chip_8258/register.h:730-763`:

```c
#define reg_tmr_ctrl   REG_ADDR32(0x620)
    FLD_TMR2_EN      = BIT(6),        // Timer2 counter enable
    FLD_TMR2_MODE    = BIT_RNG(7,8),
    FLD_TMR_WD_CAPT  = BIT_RNG(9,22), // watchdog reload/capture compare
    FLD_TMR_WD_EN    = BIT(23),       // watchdog *mode* enable
    FLD_TMR2_STA     = BIT(26),
    FLD_CLR_WD       = BIT(27),
#define reg_tmr_sta    REG_ADDR8(0x623)
    FLD_TMR_STA_WD   = BIT(3),        // write 1 => clear WD counter
#define reg_tmr2_tick  REG_ADDR32(0x638)
```

`wd_start()` is a read-modify-write on `reg_tmr_ctrl` only:

```c
BM_SET(reg_tmr_ctrl, FLD_TMR2_EN);    // bit 6  -> counter runs
BM_SET(reg_tmr_ctrl, FLD_TMR_WD_EN);  // bit 23 -> WD mode reset path
```

`wd_clear()` pokes **one** register with a single byte store:

```c
reg_tmr_sta = FLD_TMR_STA_WD;         // write 0x08 to 0x623
```

Disassembly of the linked ELF matches source exactly:

```
1d01c <drv_wd_setInterval>:  ...  [0x638]=0 ; reg_tmr_ctrl &= 0xff8001ff ; reg_tmr_ctrl |= (capt<<9)
1d060 <drv_wd_start>:        [0x620] |= 0x40  (bit6) ; [0x620] |= 0x00800000 (bit23)
1d07c <drv_wd_clear>:        [0x623] = 0x08
```

`wd_set_interval_ms` computes `capt = 600*1000*system_clk_mHz >> 18`; `clock_init` sets
`system_clk_mHz` to 12/16/24/32/48 (`0x36a04` store to `0x8477be`), so the capture value is a real,
finite timeout (~0.6 s at 48 MHz).

## 2. Every timer/control register the MAC CSMA path writes

All symbols below are from `build/light/light_TS0505B` (not stripped). The prebuilt `libzb_router.a`
MAC code is linked into the same ELF, so a whole-ELF literal scan covers it.

### 2.1 `rf_performCCA` (`0x958`)

- Reads `reg_system_tick` (`0x740`) as the loop bound, and RSSI at `0x800441`.
- **No write** to any timer/control register.

### 2.2 `mac_csmaStart` (`0x21d4c`)

- Writes only SRAM MAC state: `0x842551`, `0x8426dc`, `0x84748c`, `0x847118`, plus RF trx
  registers via `rf802154_tx` (`0x800xxx`).
- **No write** to `0x620`/`0x623`/`0x638`.

### 2.3 `mac_trxTask` (`0x21fcc`) CSMA backoff -> `drv_hwTmr_set`

The backoff at `0x221b0` is unambiguous:

```
221ae  r1 = #200                  ; 200 us
221b0  r0 = #3                    ; TIMER_IDX_3 = SYS timer
221b2  r2 = mac_csmaStart         ; callback
221b4  r3 = txdesc
221b6  tjl  1eafc <drv_hwTmr_set>
```

`drv_hwTmr_set` (`0x1eafc`) for `tmrIdx == 3` takes the STIM path, not the Timer0/1/2 path. Its
only hardware writes are:

```
1ebc6  r3 = [0x740]  (current system tick)
1ebca  r3 += capture
1ebce  [0x744] = r3                      ; reg_system_tick_irq
1ebd4  r1 = [0x640]
1ebda  r1 |= 0x00100000                  ; FLD_IRQ_SYSTEM_TIMER (bit 20)
1ebdc  [0x640] = r1                      ; reg_irq_mask
```

`drv_hwTmr_init(3,0)` (called once from `drv_platform_init` at `0x1cf28`) writes:

```
1eac6  [0x74c] |= 0x02                   ; reg_system_tick_mode |= FLD_SYSTEM_TICK_IRQ_EN (bit 1)
```

`drv_hwTmr_irq_process(3)` -> `hwTimerStop(3)` -> `hwTimerSet(3)` write only `reg_irq_mask`
(`0x640`, clear/set bit 20) and `reg_system_tick_irq` (`0x744`).

### 2.4 `timer_set_mode` / `timer_start` / `timer_stop` are dead in this firmware

These are the only functions in the ELF that write `reg_tmr_ctrl` (`0x620`) besides the watchdog
itself, but they are never reached:

- `timer_set_mode` (`0x378cc`) writes `reg_tmr_sta` (`0x623`) = `0x04` (TMR2 status clear) and
  read-modify-writes `reg_tmr_ctrl` (`0x620`) mode bits. Its **only** caller is
  `drv_hwTmr_init`'s `idx < 3` branch (`0x1eada`), and `drv_hwTmr_init` is called exactly once with
  `idx = 3` (`0x1cf24`), so the `idx < 3` branch never executes.
- `timer_start` (`0x3785c`) / `timer_stop` (`0x37894`) are called only from the `idx < 3` branches
  of `drv_hwTmr_set`/`hwTimerStop`; `drv_hwTmr_set` has exactly one call site and it passes `idx=3`.

Whole-ELF `.word` literal scan (timer/IRQ block) confirms the only references to the watchdog's
three registers are the watchdog functions themselves:

```
0x620 (reg_tmr_ctrl):   3 refs -> drv_wd_start, timer_start, timer_stop   (last two dead)
0x623 (reg_tmr_sta):    2 refs -> drv_wd_clear, timer_set_mode            (last dead)
0x638 (reg_tmr2_tick):  1 ref  -> drv_wd_setInterval
```

The MAC library never references `0x620`, `0x623`, or `0x638` anywhere.

### 2.5 Cross-check table

| MAC/CSMA write | address | watchdog register | overlap? |
|---|---|---|---|
| `reg_system_tick_irq` = capture | `0x744` | `0x620`/`0x623`/`0x638` | no |
| `reg_irq_mask` \|= bit 20 | `0x640` | `0x620`/`0x623`/`0x638` | no |
| `reg_system_tick_mode` \|= bit 1 (init) | `0x74c` | `0x620`/`0x623`/`0x638` | no |
| `reg_system_tick` read (CCA bound) | `0x740` | — | no |

## 3. Reset, not IRQ — and MAC reprogramming does not affect it

`wd_start()` enables only `FLD_TMR2_EN` + `FLD_TMR_WD_EN` in `reg_tmr_ctrl`. It does **not** set
`reg_irq_mask` `FLD_IRQ_TMR2_EN` (`0x640` bit 2). On this silicon the watchdog timeout asserts a
**system reset**, not a Timer2 interrupt. The reset path is driven purely by the Timer2 counter
(`reg_tmr2_tick`) reaching the capture value in `reg_tmr_ctrl` bits 9-22; it is independent of the
CPU's interrupt controller and of the current IRQ-mask state.

Therefore, even while the MAC CSMA path reprograms the **system timer** (`0x744`/`0x640`), the
watchdog's Timer2 reset path is untouched and remains armed.

## 4. Conclusion

**NO.** MAC/CSMA timer activity cannot plausibly defeat the enabled watchdog on the TLSR8258. The
CSMA backoff uses `TIMER_IDX_3` (system timer) and writes only `0x744`/`0x640`/`0x74c`; the watchdog
uses Timer2 (`0x620`/`0x623`/`0x638`). There is no register overlap, and the only functions that
could clear `FLD_TMR2_EN` (`timer_stop(2)`) or rewrite `reg_tmr_ctrl` mode bits (`timer_set_mode`)
are unreachable dead code in this firmware.

The empirical fact that `s_failCnt == 1` after 25+ minutes (no watchdog reset) must therefore be
explained by one of:

1. **The wedge is a logical stall, not a hard CPU hang** — `tl_zbTaskProcedure()` still returns
   (at least occasionally) and `main()` still executes `drv_wd_clear()` at `main.c:88`/`:94`, so the
   watchdog is correctly serviced and never fires; the scan state machine is stuck at channel 24 but
   the main loop is alive. This is now the most consistent explanation.
2. **A silicon/hardware watchdog issue on this bench part** — the WD is configured and its registers
   are never clobbered, yet it does not reset. This is a hardware concern, not a firmware-clobber
   concern.

CSMA timer-register activity is **not** a viable defeat mechanism on this part.

### Smallest robust fix sketch

- **First, instrument instead of guessing.** At boot (after `drv_wd_start()`), read back
  `reg_tmr_ctrl` (`0x620`) and assert `(bit6 && bit23)`; sample `reg_tmr2_tick` (`0x638`) to confirm
  it is incrementing. This discriminates "WD not armed" (silicon/config) from "WD armed but main
  loop still feeds it" (logical stall).
- **If main loop is still feeding the WD** (logical stall), a main-loop watchdog cannot catch this
  class by design. Add an application-level progress monitor: in `main()` or the app timer path,
  require the Zigbee scan/join state machine to advance within N seconds; if `tl_zbMacScanRunning`
  (channel index `0x8421e4`) or an equivalent state word stops advancing, force a controlled
  `SYSTEM_RESET`/NV rescue increment instead of relying on the hardware WD.
- **Belt-and-suspenders re-arm** (only if instrument shows bits 6/23 ever clear): call
  `drv_wd_start()` once at the top of `main()`'s `while(1)`, or re-write
  `reg_tmr_ctrl |= FLD_TMR2_EN | FLD_TMR_WD_EN` in a 1 s app tick. But register analysis says
  nothing clears these bits, so this is defensive, not the root fix.
- **Do not** re-arm the watchdog "after MAC init" as a targeted fix: the MAC init/CSMA path never
  touches the watchdog registers, so there is nothing to repair there.

## Quick answer: is there an IRQ-safe WD keepalive hook?

`drv_wd_clear()` / `wd_clear()` is a single 8-bit store `reg_tmr_sta = FLD_TMR_STA_WD` (`0x623`).
It is **register-safe from any context, including a timer IRQ** — it is not a read-modify-write and
has no shared-state race.

However, calling it from the system-timer IRQ (where the CSMA backoff callback runs) would
**defeat the watchdog**: that IRQ keeps firing while the main loop is stuck, so the WD would never
reset a main-loop hang. The SDK does not provide a separate "feed from ISR" API beyond
`drv_wd_clear()`, and the current firmware deliberately avoids it from IRQ context for this reason.
