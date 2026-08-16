# HANG_FINDINGS — why build 04 bricked fixture #3, and what the dump must decide

**Date:** 2026-08-15
**Incident:** `0xa4c138…d282`, build 04 (commit `b485043`, contains the
colour-temperature change `5d932a6`), flashed OTA at 09:25, joined and passed a
full z2m configure pass at 10:21:53, then hung between 10:22 and 10:23. Radio
dead (`MAC_NO_ACK` continuous), PWM latched steady full **orange**, no reboots,
hang survived a power cycle.
**Master brief:** `AI_BUGHUNT_BRIEF.md`. **Incident log:**
`INCIDENT_2026-08-15.md`.

This document merges six parallel workstreams:

| source | scope | status |
|---|---|---|
| `bughunt/L1_color.md` | colour change `5d932a6` | no hard hang; one real timer defect; **bridge to L2** |
| `bughunt/L2_nv.md` | NV / flash-erase path | **LEAD THEORY (L2-a)** |
| `bughunt/no_announce.md` | why no `device_announce` | **proven monitoring blind spot**, not a hang |
| `bughunt/lightshow.md` | light-show robustness | **1 fix committed** (`739ad92`/`472636e`) |
| `bughunt/watchdog_design.md` | build 06 watchdog design | **design only, not applied** |
| `OTA_SPEED_ANALYSIS.md` | 54-minute OTA | **committed** (`7443e64`) |

---

## 0. Verdict up front

**The lead theory is L2-a: an unbounded poll in `mspi_wait()` that runs with
global interrupts disabled on every flash operation.** The trigger for this
particular device is the colour change: the first-ever warm colour write made
`lightAttrsChanged` dirty, which one second later performed the first-ever
colour NV save on a fresh-conversion unit, which drove a 4 KB sector erase
straight into the unbounded flash/MSPI busy-wait. That single chain explains
six of the seven evidence points, and the seventh — the missing announce — has
its own independent, now-proven explanation.

**The honest answer to "could the next image do this again?" is yes.** Nothing
has shipped that removes the unbounded `mspi_wait()` poll or enables the
hardware watchdog. Until the watchdog + `ota_newImageValid()` CRC servicing
(`bughunt/watchdog_design.md`) — and, ideally, a bound on `mspi_wait()` itself —
land together, any image that performs a flash operation can still hang exactly
this way. Build 05 removes the colour *trigger* but does **not** remove the
flash *mechanism*; configure-pass reporting/bind/keypair writes alone can still
reach the same wedge. This is stated plainly at the end (§6).

---

## 1. The seven evidence points (reference)

1. Death window: 10:21:53 (full configure pass answered) → 10:23:03 (ZDO
   active-endpoints request timed out).
2. ZDO answered a node-descriptor earlier, so ZDO worked, then stopped.
3. No `device_announce` ever — including the boot that joined and was configured.
4. No reboots: steady output for 30+ s; 25+ minutes of full output means the
   probation counter (needs 6 boots) never advanced.
5. Continuous `MAC_NO_ACK` (0xe9), occasional `MAC_CHANNEL_ACCESS_FAILURE`
   (0xe1) — the radio is not being serviced.
6. Output = steady full **orange** (warm-white channel at ~full duty) — a warm
   PWM write happened *after* boot (boot path ends OFF), and it latched.
7. The colour-temperature path (`5d932a6`) was live — the only behavioural
   change in build 04 and the only code that had never run on hardware before.

---

## 2. The lead theory, as one causal chain

### L2-a · `mspi_wait()` unbounded poll with IRQs off — **LEAD**

**File/line:** `build/tl_zigbee_sdk/platform/chip_8258/spi_i.h:36-39`

```c
_attribute_ram_code_sec_ static inline void mspi_wait(void){
    while(reg_mspi_ctrl & FLD_MSPI_BUSY)
        ;
}
```

There is no iteration cap and no timeout. If the MSPI/flash controller never
clears `FLD_MSPI_BUSY`, the CPU spins here forever. Every caller of
`mspi_wait()` in the flash path is already inside a critical section that has
executed `irq_disable()`:

* `flash_mspi_read_ram()`  — `flash.c:108` disables IRQs, restores at `:131`
* `flash_mspi_write_ram()` — `flash.c:146` disables IRQs, restores at `:174`

The restore statements are unconditional, but they are **unreachable** if the
intervening `mspi_wait()` never returns. So the hang happens with global IRQs
off.

### The exact chain that explains the orange, the radio death, and the no-reboots

```
HA/z2m restores state (or a colour-temperature command arrives)
  → CCT handler (zcl_colorCtrlCb.c, build 04) runs light_applyUpdate_16
  → light_fresh() sets gLightCtx.lightAttrsChanged = TRUE     (tuyaLightCtrl.c:368)
  → PWM write is warm → light renders full orange             [evidence 6]
  → 1 s later tuyaLightAttrsStoreTimerCb calls
    zcl_colorCtrlAttr_save()                                  (tuyaLight.c:261-269)
  → first-ever colour NV write on this fresh-conversion unit
  → nv_flashWriteNew() → nv_flashWriteNewHandler() → flash_erase / flash_writeWithCheck
  → flash_mspi_write_ram(): irq_disable()                     (flash.c:146)
  → mspi_wait() never returns                                 (spi_i.h:37)
  → irq_restore() at flash.c:174 never runs → IRQs stay off forever
  → RF ISR never serviced → MAC_NO_ACK / CHANNEL_ACCESS_FAILURE  [evidence 5]
  → ZDO/ZCL stack frozen after having answered configure      [evidence 1, 2]
  → no exception, no SYSTEM_RESET, watchdog off               [evidence 4]
  → power cycle clears the wedge, but the same warm write + NV save
    re-triggers it on the next boot → hang survives power cycle
```

This is the only candidate that turns a flash fault into a **permanent**
radio-dead, PWM-latched, no-reboot hang. It explains points 1, 2, 4, and 5
directly. It explains points 6 and 7 *through the colour trigger*: the colour
change (`5d932a6`) is what made the warm state-restore write change
`colorTemperatureMireds` and mark the attributes dirty, producing both the
orange PWM write and the first colour NV save in the same window. Point 3
(no announce) is independent and is explained in §3.

Why this reconciles the two "separable" leads L1 and L2: **L2-a is the
mechanism; the colour change is the trigger.** L2-a alone does not produce the
colour; the colour change alone does not hang. Together they explain all seven
points.

The proof request from the brief ("what happens if an erase runs while the
radio ISR needs servicing — how long are IRQs off, and is there any path where
they are never re-enabled?") is answered: a normal erase is ~20–80 ms of IRQ-off
(transient, recovered by the MAC once IRQs return); a wedged erase is infinite
IRQ-off, because the restore exists but is unreachable. There is **no missing
restore on an explicit branch** — the leak is the unbounded wait before the
restore.

---

## 3. The no-announce question — proven, and it is *not* the hang

**The SDK does not send `device_announce` on a first, factory-new join.** It
announces on rejoin and on address-conflict resolution only.

Decoded from the prebuilt `libzb_router.a` (`zdo_startup_complete` in
`zdo_nwk_manager.o`), the gate is:

```c
if (!g_zbNwkCtx.is_factory_new)          /* rejoin */
    zdo_device_announce_send(buf);
else if (buf->hdr.rejoinStartAgain)
    zdo_device_announce_send(buf);
/* else: skip announce */
g_zbNwkCtx.is_factory_new = 0;           /* cleared AFTER the test */
```

`is_factory_new` is bit 0 of the bitfield byte at `g_zbNwkCtx + 45`
(`nwk_ctx.h:103`); `rejoinStartAgain` is bit 5 of the bitfield byte at
`buf + 195` (`zb_buffer.h` hdr is 4 bytes, `ZB_BUF_SIZE` 192). Exactly three
call sites of `zdo_device_announce_send` exist in the archive, and **none** is
the fresh-join path. Build 04 did a fresh join (empty NV), so it was silent —
exactly as observed. The 2026-08-14 casualty rejoined an existing network and
announced on every cycle.

Consequence: **`tools/announce_watch.py` and the abort table in
`OTA_TEST_PLAN.md` are blind (in fact false-positive) during the conversion
window they were written for.** A healthy first join and a hung first join are
indistinguishable to the watcher — both produce zero announces and exit 0.
Proposed fixes: a 5-line firmware first-join announce in `light/zb_appCb.c`
(`zb_zdoSendDevAnnance()` gated by a factory-new flag), and/or a z2m-side
`--fresh-join` watcher mode counting `device_joined` /
`device_interview(successful)`. Neither is committed; the firmware change is a
deliberate on-air behaviour addition and is documented as a proposal
(`bughunt/no_announce.md` §4).

Note this is a monitoring blind spot, **not** a hang candidate and **not** a
symptom of the hang. It explains evidence point 3 in full on its own.

---

## 4. Full candidate inventory and ranking

### 4.1 Hang-class candidates

| rank | id | file:line | class | explains 7 points? |
|---|---|---|---|---|
| **1** | **L2-a** | `spi_i.h:36-39` (via `flash.c:106-177`) | unbounded poll, IRQs off | **Yes — 6/7 directly + trigger; §3 covers the 7th** |
| 2 | L2-b | `flash.c:82-94` | bounded-but-huge poll (~10–30 s), IRQs off | partial — recovers too soon to explain 25+ min; the wedge that *feeds* L2-a |
| 3 | L1-1 | `zcl_colorCtrlCb.c` (build 04) `:294-310` | perpetual 100 ms colour timer on mode-switch mid-fade | no — CPU/radio stay alive; refutes #5 |
| 4 | L1-2 | `tuyaLightCtrl.c:404-430` | `remainingTime == 0xFFFF` never-decrement | no — by-design continuous move; refutes #5 |
| 5 | L2-c | `drv_hw.c:82-112` (`while(1);` at `:92`) | boot-path hang | no — runs before join; refuted by #2 |
| 6 | LS-1 | `zcl_levelCb.c:219` (**fixed**) | level `move` `rate==0` div-by-zero | would have explained #4/#5 and been consistent with #6, not #3/#7 — **now closed** |

Non-hang findings that were audited and matter for context: L1-3 (`light_fresh`
re-entry guard balances on every path — clean), L1-4 (colour arithmetic has no
hang primitive — clean), LS-2 (stale-state clobber on effect+colour), LS-3
(perpetual `move` timer churn), LS-4 (ZCL RX queue overflow leaks buffers),
LS-5 (weak malformed-WRITE bounds), LS-6 (effect timeline wrap at ~49.7 days).
None explains the incident; details in `bughunt/lightshow.md` and
`bughunt/L1_color.md`.

### 4.2 Why L1-1 and L1-2 are not the incident

The decisive evidence is `MAC_NO_ACK` (point 5). A leaked 100 ms colour timer or
a never-decrementing transition keeps the CPU and radio alive — the main loop
continues, the MAC continues to auto-ACK in hardware, and z2m would not see
continuous `MAC_NO_ACK`. A leaked timer also produces no reset (so it matches
point 4) but it cannot produce points 1, 2, or 5. L1-1 is still a real defect to
fix when `5d932a6` is re-applied; it is deliberately **not** committed because
HEAD is build 05 (colour reverted) and the brief forbids re-applying the change.

### 4.3 Why L2-b is not the standalone culprit

`flash_wait_done()` is bounded at 10,000,000 iterations. If the flash WIP bit
sticks, that is ~10–30 s of radio silence, then the poll exits and the device
recovers. It cannot explain 25+ minutes of dead radio. Its real significance is
twofold: (a) it is the *next* wedge point after `mspi_wait()` returns, and (b)
once the watchdog is on, it is exactly the ~10–30 s IRQ-off span the 600 ms
interval is meant to reset. Note its inner `flash_is_busy()` calls `mspi_read()`
→ `mspi_wait()`, so L2-b and L2-a are nested, not independent.

### 4.4 Why L2-c is not the incident

`internalFlashSizeCheck()`'s `while(1);` runs inside `drv_platform_init()`,
before the device joins. Point 2 (ZDO answered a node-descriptor) proves the
device got past boot, so L2-c cannot be the trigger. Recorded only for the
hang-class inventory.

### 4.5 L3 (prebuilt stack) — the un-auditable remainder

ZDO active-endpoints is handled inside `libzb_router.a`. It answered
node-descriptor first (point 2), which the report notes is *not* inconsistent
with L2-a (the wedge froze it mid-configure, not at boot). L3 remains the only
place a hang could hide that we cannot read from source; the SWire dump is the
only way to see inside it. It is ranked behind L2-a because L2-a is a concrete,
reachable, unbounded wait in audited code, whereas L3 has no identified
mechanism.

---

## 5. What the SWire dump must be compared against

Ground truth procedure: `PI_SWIRE_SETUP.md`, `dump/TLSR825xComFlasher.working.py`,
`SWS_DIV=110`, 460800 baud. Reads work; writes do not.

### 5.1 Dump-comparison checklist

1. **SRAM — the prize.** Stack at the moment of the hang contains return
   addresses. Map them with the ELF from the **exact** build: `git checkout
   b485043 && rebuild`, then addr2line. The exact flashed bytes are preserved in
   `field-artifacts/1141-d3a3-ffffffff-BUILD04-FLASHED-TO-0xa4c138…d282.zigbee`
   — a rebuild is *not* byte-identical (`build_time_str` is baked in), so
   prefer the field artifact for byte comparisons and the rebuild only for
   symbol mapping.
2. **Flash @0x8000** — confirm it matches the field artifact minus its 62-byte
   OTA header (sha256 `01c148be…`). This rules out "wrong image served".
3. **Flash @0xD8000–0xEE000 (our NV)** — look for a half-written sector or a torn
   index in the ZCL/APS modules. This is the direct evidence the hang intersected
   an NV write (supports L2).
4. **Flash @0x70000 (staging bank)** — should be all-`0xFF` after install, or
   hold the old staged image. Informative for the OTA path; not expected to be a
   hang smoking gun.

### 5.2 What each candidate would show

**If L2-a (`mspi_wait`) is the culprit (expected):**
- **SRAM/PC:** program counter inside the `_attribute_ram_code_sec_` `mspi_wait()`
  loop. Return-address chain: `flash_send_cmd`/`flash_send_addr`/
  `flash_mspi_read_ram`/`flash_mspi_write_ram` → `flash_erase`/
  `flash_writeWithCheck`/`flash_write` → `nv_flashWriteNewHandler` or
  `nv_write_item` → `nv_flashWriteNew` → `zcl_reportingTab_save` /
  `zcl_colorCtrlAttr_save` / APS bind save. `reg_irq_en == 0`.
- **Flash @0xD8000–0xEE000:** depends where the wedge hit — a wedge during a
  *read* leaves NV fully consistent (a negative-looking result; the stack is then
  the only evidence), a wedge during an *erase/program* leaves a half-written
  sector: new sector erased `0xFF` with partial item copies, or a `READY` header
  (`0xFAFA`) with valid/invalid CRC, plus the old sector still `0x5A5A`/`0x7A7A`.
- **Flash @0x70000:** spent staging bank, not informative for L2.

**If L2-b (`flash_wait_done` WIP-stuck) were the whole story:**
- PC in `flash_wait_done()`'s `for` loop; `reg_irq_en == 0`; flash status bit 0
  stuck `1`. Alone it would recover in ~10–30 s, so its presence means the CPU
  had not yet reached the unbounded `mspi_wait`, or that L2-a is co-occurring.

**If L1-1 / L1-2 (leaked colour timer) were the culprit (not expected):**
- Living CPU in the main loop: `ev_main` → `ev_timer_process` →
  `ev_timer_executeCB` → `tuyaLight_colorTimerEvtCb`, `colorTimerEvt` non-NULL,
  one of `hueRemainingTime`/`saturationRemainingTime`/`colorTempRemainingTime`
  non-zero while `enhancedColorMode` points at a different mode (L1-1), or
  `colorTempRemainingTime == 0xFFFF` (L1-2). **Radio still in RX** — the dump
  would not show a radio-dead state, which is how you'd know this was not the
  whole story.

**If LS-1 (div-by-zero — now fixed) had been the culprit:**
- PC at the tc32 software divider busy-wait (`div`/`.L2` around `0x800660`),
  return chain `tuyaLight_moveProcess` → `tuyaLight_levelCb` →
  `zcl_level_clientCmdHandler` → `zcl_cmdHandler` → `tl_zbTaskProcedure` → `main`.
  No exception (`T_evtExcept` all zero); PWM warm duty latched. (Cannot be the
  culprit unless the flashed bytes predate `739ad92` — build 04 predates it, so
  this is a *real* possible explanation only if the warm write was a level move
  with `rate==0`; but the colour-change trigger is far more consistent with
  point 7.)

**If L2-c were reached (refuted by timeline):**
- PC at `drv_hw.c:92` `while(1);` — but the device would never have joined,
  contradicting point 2.

**No-announce (point 3) in the dump:**
- Not a hang candidate; if inspected, `g_zbNwkCtx` offset 45 should show
  `joined = 1` (bit 2) and `is_factory_new = 0` (bit 0, already cleared),
  confirming "joined but silent". Nothing distinguishes silent-first-join from
  silent-hung — that is the point of the finding.

---

## 6. Could the next image do this again?

**Yes. Plainly, yes.**

* Build 05 (`a72a2ae`) removes the colour **trigger**, but the flash **mechanism**
  is untouched. The configure pass itself performs `zcl_reportingTab_save()`,
  bind-table saves, and possibly keypair rotation — all sector erases that reach
  the same `mspi_wait()`. A build 05 flashed to a fresh-conversion unit could
  wedge on any of those, with the only difference that it would not first render
  the tell-tale warm orange (it would latch whatever PWM was last written).
* The unbounded `mspi_wait()` has not been bounded. Bounding a hardware busy-wait
  in the vendored SPI primitive needs a defined timeout/reset policy and is
  deliberately left as a finding, not a silent patch.
* `MODULE_WATCHDOG_ENABLE` is still `0`. Rescue mode cannot catch a hang —
  probation counts reboots and a hang produces none. A hang is strictly worse
  than a reset loop on an OTA-only-recovery device.

The fix that turns this failure class from "ladder" into "recoverable" is the
Part C watchdog change, designed in `bughunt/watchdog_design.md` and **not yet
applied**: add `drv_wd_clear()` inside `ota_newImageValid()`'s CRC loop
(`ota.c:161-172`) and set `MODULE_WATCHDOG_ENABLE 1` in the same commit. That is
the missing second half of the safety net. Until it lands, every image that does
a flash operation carries this residual risk. State this loudly: the honest
answer to "could the next image do this again?" is **yes, until the watchdog
and the mspi bound land.**

---

## 7. Committed fixes credited

* `739ad92` — **LS-1 (CRITICAL, hang):** guard level `move`/`move with on/off`
  `rate == 0` against the tc32 hardware divide-by-zero busy-wait in
  `light/zcl_levelCb.c` (`tuyaLight_moveProcess`). A real hang-class bug in the
  same codebase, now closed.
* `472636e` — light-show robustness findings (`bughunt/lightshow.md`), including
  LS-2..LS-6.
* `7443e64` — OTA speed analysis (`OTA_SPEED_ANALYSIS.md`): the 250 ms z2m
  default was in effect; slow windows are bursty round-trip latency, not a raised
  knob; bench stalls match the 5 s `ota_imageBlockRspWait` quiet-expiry-then-abort
  signature; server-side recommendation documented.

Uncommitted working documents (included in the same commit as this file):
`bughunt/L1_color.md`, `bughunt/L2_nv.md`, `bughunt/no_announce.md`,
`bughunt/watchdog_design.md`.

---

## 8. What "done" means for the human reading this

One lead — **L2-a, triggered by the colour change** — explains all seven evidence
points: the warm write produced the orange and the first colour NV save, the NV
save drove a sector erase into `mspi_wait()` with IRQs off, and that single
unbounded poll produced the radio death, the no-reboots, the survival of a power
cycle, and the configure-window timing. The no-announce is separately and
completely explained by the SDK's fresh-join gate, so it is not an obstacle to
the theory. When the SRAM dump arrives, compare the PC and return-address chain
against §5.2; if the PC is not in `mspi_wait()`/`flash_wait_done()`/the flash
path, the dump must decide, and §5.2 says what each alternative outcome would
mean.

---

## 9. Addendum — build 06 closes the hang class by construction

**No SWire dump will arrive.** The bench unit stays silent on the SWS line; the
most likely cause is a bad pin-17 SWS solder joint, and a physical re-seat is
needed before any further SWire read can succeed.

The hang class this document was written to explain is closed in build 06
(`moes-ts0505b`, HEAD `07e157a`), by construction and independent of whether
L2-a was the true mechanism:

* a hang now becomes a watchdog reset (`MODULE_WATCHDOG_ENABLE 1`,
  `light/app_cfg.h:76`) → probation counts the boots → rescue mode latches on
  boot 7 (after six unstable boots park the counter at the threshold), so a hung
  light is recoverable over OTA instead of permanent;
* `mspi_wait()` is bounded (`MSPI_WAIT_MAX_ITER 20000u`), so the L2-a wedge can
  no longer spin forever with global IRQs off — the caller's `irq_restore()`
  now runs on timeout even if `FLD_MSPI_BUSY` sticks.

Sections 0–8 above are the pre-build-06 record and are left intact.

## 10. Addendum — build 06 met the wedge, build 07 answers it

Build 06 did go on the bench unit (2026-08-15/16) and **the hang class came
back in a new costume**: not a CPU hang but a *logical* stall — the nwk-layer
rejoin scan freezes at channel 24 inside the prebuilt MAC lib, ~70–95 s after
every join (`bughunt/mac_scan_wedge.md`). The main loop keeps feeding the
600 ms watchdog, so build 06's net never engaged: `s_failCnt` was still 1
after 25+ wedged minutes.

Build 07 adds the missing layer (`light/moes_liveness.c`): the stack clears
`g_zbNwkCtx.joined` when it declares the parent lost (verified 0 in both hang
SRAM captures), and a frozen scan means no BDB commissioning callback ever
fires again. So "armed (has credentials) + continuously unjoined + zero BDB
events for 60 s" is a precise wedge signature, and the app forces a marked
`SYSTEM_RESET()`. The boot-time scan works (proven twice on the bench unit),
probation counts the boots, and rescue mode latches on boot 7 — the wedge
becomes a ~15-minute self-recovery instead of a brick. A coordinator that is
merely offline keeps raising `REJOIN_FAILURE`, which holds the fuse open, so
an outage causes no resets and no rescue latches. Host test: 22/22, including
the full wedge→reset→rescue→healthy-clear chain.
