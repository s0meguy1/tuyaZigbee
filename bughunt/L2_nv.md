# L2 — NV / flash-erase during the configure pass

**Workstream:** LEAD L2 (hang class + recovery robustness)
**Scope:** `build/tl_zigbee_sdk/proj/drivers/drv_nv.c`,
`build/tl_zigbee_sdk/platform/chip_8258/flash.c`, plus the flash wrappers and
the MSPI/IRQ primitives they depend on
(`proj/drivers/drv_flash.c`, `platform/chip_8258/spi_i.h`,
`platform/chip_8258/irq.h`, `proj/drivers/drv_hw.c`,
`apps/common/main.c`, `proj/drivers/drv_nv.h`).

**Verdict up front:** the NV write path is *not* clean. There is an
**unbounded poll** (`mspi_wait()`, `spi_i.h:37`) that runs **with global
IRQs disabled** on every flash read/write/erase. If the MSPI/flash hardware
ever fails to complete a byte transfer, the CPU spins in that loop forever,
the radio ISR is never serviced again, PWM latches, and — because
`MODULE_WATCHDOG_ENABLE 0` — nothing ever resets it. This is a
hang-class candidate, not a reset-class one. The NV *software* state machine
itself (two-phase sector commit, torn-write recovery, `forceChgSec` retry)
has bounded loops and I could not find an error branch in it that leaves the
next access spinning. The dangerous unbounded wait is in the flash/MSPI
layer, one level below NV.

**Committed fixes:** none. The unbounded `mspi_wait()` is a vendored-SDK
primitive used for every SPI flash byte; bounding it without a defined
timeout/reset policy risks silent data corruption, and adding a watchdog
clear there is dead code while the watchdog is off. Those are debatable
changes, so per the brief they stay findings. The actual mitigation is the
Part C watchdog change (design-only, not this workstream).

---

## 1. The call chain that runs in the death window

```
z2m configure reporting / bind / key write
  zcl_reportingTab_save()            zigbee/zcl/zcl_nv.c:61-72  (single=1)
  aps binding-table save             libzb_router.a (prebuilt)
  nv_flashWriteNew()                 drv_nv.c:782
    nv_flashWriteNewHandler()        drv_nv.c:568
      nv_sector_read()               drv_nv.c:204   (recovery/promotion)
      flash_erase()                  drv_flash.c:184
      nv_write_item()                drv_nv.c:259   (flash copies, page writes)
      flash_writeWithCheck()         drv_flash.c:146
      flash_read()/flash_write()     drv_flash.c:134/180
        flash_erase_sector()         flash.c:193
        flash_write_page()           flash.c:237
        flash_read_page()            flash.c:214
          flash_mspi_write_ram()     flash.c:144   <-- irq_disable + flash_wait_done
          flash_mspi_read_ram()      flash.c:106   <-- irq_disable + mspi_wait loop
            mspi_wait()              spi_i.h:37    <-- UNBOUNDED poll
```

The three evidence-relevant NV writes in the death window are all 4 KB
sector erases (`FLASH_SECTOR_SIZE 4096`, `drv_nv.h:400`): ZCL reporting
(`NV_MODULE_ZCL`), APS bind table (`NV_MODULE_APS`), and possibly the
keypair module (`NV_MODULE_KEYPAIR`, 4 sectors = 16 KB per rotation,
`drv_nv.h:420`).

---

## 2. (a) Unbounded polls on a flash/hardware status bit

### Finding L2-a · `mspi_wait()` is an unbounded poll, and it runs with IRQs off

**File:** `build/tl_zigbee_sdk/platform/chip_8258/spi_i.h:36-39`

```c
_attribute_ram_code_sec_ static inline void mspi_wait(void){
    while(reg_mspi_ctrl & FLD_MSPI_BUSY)
        ;
}
```

- `FLD_MSPI_BUSY` is `BIT(4)` of `reg_mspi_ctrl` (`register.h:131`).
- There is **no iteration cap and no timeout**. If the MSPI engine never
  clears the busy bit (flash stops clocking back, CS/clock fault, or a prior
  incomplete command leaves the controller wedged), this loop never exits.
- Every caller of `mspi_wait()` in the flash path is inside a
  `flash_mspi_read_ram()` / `flash_mspi_write_ram()` critical section that
  has already done `irq_disable()` (`flash.c:108` and `flash.c:146`). So the
  hang happens **with global interrupts off**.

**Causal chain to the observed death:**

1. Configure pass writes NV → `nv_flashWriteNewHandler` rotates a sector →
   `flash_erase`/`flash_writeWithCheck` → `flash_mspi_write_ram`.
2. `flash_mspi_write_ram` executes `irq_disable()` (`flash.c:146`), then
   drives the MSPI. Some transfer does not complete.
3. CPU enters `while(reg_mspi_ctrl & FLD_MSPI_BUSY);` and never leaves.
4. `irq_restore()` at `flash.c:174` is never reached → IRQs stay off forever.
5. Radio ISR (RF) never runs again → no MAC ACK, no RX service →
   `MAC_NO_ACK`/`MAC_CHANNEL_ACCESS_FAILURE` (evidence 5).
6. PWM is hardware-latched at the last written duty (full warm-orange),
   independent of the CPU (evidence 6).
7. No exception, no `SYSTEM_RESET`, no reboot; `MODULE_WATCHDOG_ENABLE 0`
   means no hardware reset either (evidence 4).
8. A power cycle clears the hardware wedge; the same configure-pass NV write
   re-triggers it, so the hang **survives a power cycle** (evidence 1).

### Finding L2-b · `flash_wait_done()` is bounded but enormous, and IRQs are off for all of it

**File:** `build/tl_zigbee_sdk/platform/chip_8258/flash.c:82-94`

```c
_attribute_ram_code_sec_noinline_ static void flash_wait_done(void)
{
    sleep_us(100);
    flash_send_cmd(FLASH_READ_STATUS_CMD_LOWBYTE);
    int i;
    for(i = 0; i < 10000000; ++i){
        if(!flash_is_busy()){
            break;
        }
    }
    mspi_high();
}
```

- This is a **bounded** poll (10,000,000 iterations), so it is not the
  unbounded-poll answer — but it is still a hang risk in practice, because it
  runs inside `flash_mspi_write_ram()`'s IRQ-off critical section and the
  bound is huge (see §4).
- Each iteration is one `flash_is_busy()` → `mspi_read()` → `mspi_wait()`
  (`flash.c:47-49`, `spi_i.h:91-95`). Note the *inner* `mspi_wait()` is the
  same unbounded poll as L2-a, so `flash_wait_done()` is only "bounded" when
  the MSPI controller itself is healthy.

### Finding L2-c · `internalFlashSizeCheck()` has a deliberate `while(1)`

**File:** `build/tl_zigbee_sdk/proj/drivers/drv_hw.c:82-112` (the
`while(1);` is at line 92).

This is a boot-path hang if flash size/MID does not match. It cannot explain
this incident — it runs inside `drv_platform_init()`, before the device joins
and answers configure (evidence 2 refutes it as the trigger). Recorded for
completeness: it is the same hang-class shape (no reset), but it is *not* in
the configure window.

---

## 3. (b) `drv_disable_irq()` / `irq_disable()` restore audit

The live flash path uses `irq_disable()`/`irq_restore()` directly, not the
`drv_*` wrappers:

| function | disable | restore | balanced? |
|---|---|---|---|
| `flash_mspi_read_ram()` | `flash.c:108` | `flash.c:131` | yes, unconditional |
| `flash_mspi_write_ram()` | `flash.c:146` | `flash.c:174` | yes, unconditional — **unless `flash_wait_done()`/`mspi_wait()` never returns (L2-a/L2-b)** |

`drv_flash.c` also has `cfs_flash_write_page()` / `cfs_flash_read_page()`
using `drv_disable_irq()` / `drv_restore_irq()` (`drv_flash.c:198/211` and
`211/229`), but they are compiled only under `CFS_ENABLE`, which is **not
defined** in this tree (`grep CFS_ENABLE` returns nothing). They are balanced
anyway (no early return).

`drv_platform_init()` disables IRQs at `drv_hw.c:207` and deliberately does
not restore; the matching `drv_enable_irq()` is in `main()` at
`apps/common/main.c:62`, after `user_init()` returns. That is the intended
boot sequence, not a leak — but see the watchdog note in §6.

**Conclusion for (b):** there is no *missing* restore on an explicit branch.
The only way IRQs are left off is the L2-a/L2-b path: the restore statement
exists but is unreachable because the intervening `mspi_wait()`/bounded-but-
huge `flash_wait_done()` never returns. That is the hang.

---

## 4. (c) Worst-case IRQ-off duration per operation vs a 600 ms watchdog

System clock: **48 MHz** (`common/comm_cfg.h:180`), system tick **16 MHz**
(`timer.h:51`). Flash MSPI byte time is negligible next to the flash's
internal write-in-progress (WIP) time, so WIP dominates.

| operation | IRQ-off span | basis |
|---|---|---|
| flash read (small, 4–48 B) | ~1–10 µs | command + address + `data_len` bytes, each `mspi_wait()` |
| flash read (4 KB worst, e.g. full index scan) | ~2–4 ms | 4096 bytes of MSPI reads |
| flash page program (≤256 B) | ~0.7–5 ms | WIP after `mspi_high()` |
| flash 4 KB sector erase | **~20–80 ms** | WIP; typical ~20–40 ms, datasheet max ~60–80 ms on this flash class |
| keypair rotation (4 × 4 KB erase) | **not one span**; 4 spans of ~20–80 ms with IRQs restored between each `flash_erase()` call | `drv_nv.c:629-634` |
| full `nv_flashWriteNewHandler` sector rotation | several tens–hundreds of ms *total*, but never one continuous IRQ-off span longer than a single erase | `drv_nv.c:624-764` |
| `flash_wait_done()` if WIP is stuck | **~10–30 s** (bounded) | 10,000,000 iterations × ~1–3 µs/iteration, IRQs off throughout |
| `mspi_wait()` if `FLD_MSPI_BUSY` sticks | **infinite** (unbounded) | `spi_i.h:37` |

**Against a proposed 600 ms interval:**

- Normal worst-case single erase (~80 ms) is comfortably under 600 ms. The
  keypair rotation's four erases are separated by IRQ-restore points, so even
  the largest normal NV operation does not trip a 600 ms watchdog.
- The `flash_wait_done()` WIP-stuck case (~10–30 s) **exceeds** 600 ms by
  ~16–50×. That is actually the *desired* behaviour once the watchdog exists:
  it would reset a wedged erase instead of hanging forever.
- The `mspi_wait()` unbounded case exceeds 600 ms and would also be caught by
  the watchdog — but today the watchdog is **off**, so it is a permanent hang.

---

## 5. Radio/MAC during an erase, and NV error-branch state

### What happens to the radio if an erase runs while the ISR needs servicing

`flash_mspi_write_ram()` holds `reg_irq_en = 0` for the whole erase + WIP
wait (`flash.c:146-174`). The RF ISR cannot run for that span:

- A **normal** erase (~20–80 ms) makes the MAC miss any RX/TX event in that
  window. The Zigbee stack's MAC retry/ACK timers are serviced on the main
  loop and recover once IRQs are restored; this is a transient blip, not the
  incident signature.
- A **wedged** erase (`mspi_wait`/`flash_wait_done` stuck) leaves the RF ISR
  permanently unserviced. Frames stop being ACKed, the RX state machine stops
  advancing, and z2m sees continuous `MAC_NO_ACK`/`MAC_CHANNEL_ACCESS_FAILURE`
  (evidence 5). The brief's "a hung CPU with the radio in RX would still
  auto-ACK in hardware" assumption does **not** hold when the CPU is stuck
  with `reg_irq_en = 0`: even if the RF front-end auto-ACKs in some hardware
  mode, the MAC software that runs in the ISR is frozen, so the stack cannot
  complete the receive path or schedule an ACK.

### Does any error branch leave flash/NV such that the *next* access spins?

No. I traced every loop in `drv_nv.c` and the two-phase commit:

- `nv_index_read_op()` (`drv_nv.c:146-180`) — bounded by `idxTotalNum`.
- sector-rotation copy loop (`drv_nv.c:650-708`) — bounded by `idxTotalNum`.
- `nv_write_item()` copy loop (`drv_nv.c:317-335`) — bounded by `toalLen`.
- `nv_nwkFrameCountSearch()` — bounded.
- `nv_flashWriteNew()` retries `forceChgSec` **once**, not indefinitely
  (`drv_nv.c:787-793`).
- Torn-write recovery (`nv_sector_read()`, `drv_nv.c:204-256`) is sound:
  * power loss before the new sector's `READY` header is written → new sector
    reads `0xFFFF` and is ignored; old `VALID` sector wins;
  * power loss after `READY` + valid CRC but before old `INVALID` → on next
    boot `nv_sector_read()` promotes the `READY` sector (`drv_nv.c:219-233`);
  * power loss between old `INVALID` and new `VALID` → promotion completes;
  * both-old-`VALID`-and-new-`READY` coexist → the loop picks the old `VALID`
    first and the new `READY` stays orphaned (a wasted sector, not a hang,
    and it is erased on the next rotation).
- Error returns from `nv_write_item` during the copy loop either stop the
  copy or return to the caller, leaving the new sector headerless (`0xFF`) so
  the next `nv_sector_read()` still finds the old sector (`drv_nv.c:694-703`).

The only unbounded wait is `mspi_wait()` in the flash/MSPI hardware layer. It
is not a function of NV state, so no NV error branch can *create* a
spin-forever state; it can only *reach* the one already latent in the flash
driver.

---

## 6. Evidence mapping (the 7 points)

| # | Evidence | L2 explains? |
|---|---|---|
| 1 | Death between 10:21:53 configure success and 10:23:03 ZDO timeout | **Yes.** Configure is exactly when `zcl_reportingTab_save()` / bind saves / keypair writes trigger sector erases. |
| 2 | ZDO worked, then stopped | **Yes.** A wedge mid-NV-write freezes the stack after a successful configure response. |
| 3 | No `device_announce` ever, incl. the joining boot | **No.** The announce should precede configure (~10:19–10:21); a flash hang at ~10:22 cannot retroactively erase it. This needs the independent first-join/no-announce answer (separate workstream). |
| 4 | No reboots, steady light 25+ min | **Yes.** Busy-wait is not a fault; no exception, no `SYSTEM_RESET`, watchdog off. |
| 5 | Continuous `MAC_NO_ACK`, occasional `MAC_CHANNEL_ACCESS_FAILURE` | **Yes.** IRQs off in `mspi_wait`/`flash_wait_done` → RF ISR never serviced. |
| 6 | Steady full **orange** (warm channel at ~full duty) | **Consistent, but only if a colour write preceded the hang.** L2 does not produce the colour; the configure pass itself can write colour temperature, or L1's colour path ran first. The hang then latches whatever PWM was last written. |
| 7 | Colour-temperature code path live | **Neutral.** L2 does not require `5d932a6`; it requires only the NV writes that any configure pass performs. This is the main reason L1 and L2 are separable A/B hypotheses. |

**Ranking within L2:** L2-a (`mspi_wait` unbounded, IRQs off) is the only
mechanism that turns a flash fault into a *permanent* hang matching points
1/2/4/5. L2-b is a bounded-but-very-long secondary path (~10–30 s radio
silence, then recovery — too short to explain 25+ min, so it is not the
standalone culprit). L2-c is boot-path only and refuted by point 2.

---

## 7. What the coming SWire SRAM dump would show, per culprit

### If L2-a (`mspi_wait` unbounded) is the culprit

- **SRAM / PC:** program counter inside the `_attribute_ram_code_sec_`
  `mspi_wait()` loop (`while(reg_mspi_ctrl & FLD_MSPI_BUSY);`). The return
  address chain points back through `flash_send_cmd`/`flash_send_addr`/
  `flash_mspi_read_ram`/`flash_mspi_write_ram` → `flash_erase`/
  `flash_writeWithCheck`/`flash_write` → `nv_flashWriteNewHandler` or
  `nv_write_item` → `nv_flashWriteNew` → `zcl_reportingTab_save` or the APS
  bind save. `reg_irq_en` == 0.
- **Flash @0xD8000–0xEE000:** depends on *where* the wedge hit:
  * wedge during a **read** → NV looks fully consistent (no torn sector), which
    is a negative-looking result — the stack trace is then the only evidence;
  * wedge during an **erase/program** → a half-written sector: new sector
    erased `0xFF` with partial item copies, or a `READY` header (`0xFAFA`) with
    valid/invalid CRC, plus the old sector still `0x5A5A`/`0x7A7A` — the
    two-phase commit caught in flight.
- **Flash @0x70000:** spent staging bank (all `0xFF` or old staged image), not
  informative for L2.

### If L2-b (WIP stuck, bounded) were the whole story

- PC in `flash_wait_done()`'s `for` loop; `reg_irq_en` == 0. Flash WIP bit
  (`status` bit 0) stuck `1`. The device would recover after ~10–30 s, so this
  alone cannot match the 25+ min death — its presence in the dump would
  indicate the erase was in flight but the CPU had *not yet* hit the unbounded
  `mspi_wait` (or that the MSPI was also wedged, i.e. L2-a co-occurring).

### If L2-c were reached (not this incident)

- PC at `drv_hw.c:92` `while(1);`, but the device would never have joined,
  contradicting the timeline.

---

## 8. Findings ranked (this workstream)

1. **L2-a — unbounded `mspi_wait()` with IRQs off (HIGH/hang-class).**
   `spi_i.h:36-39` reached from every flash op via `flash.c:106-177`. The only
   mechanism in the NV/flash path that produces a *permanent* radio-dead,
   PWM-latched, no-reboot hang consistent with points 1/2/4/5. **No code fix
   committed** — bounding a hardware busy-wait in the vendored SPI primitive
   is a policy/robustness change best done together with the Part C watchdog
   (and a defined reset policy), not as an isolated patch.
2. **L2-b — `flash_wait_done()` 10M-iteration bound holds IRQs off for ~10–30 s
   if WIP sticks (MEDIUM/hang-class, transient).** `flash.c:82-94`. Not the
   standalone culprit (recovers too soon), but it is the reason a wedged erase
   will reset once the watchdog lands.
3. **L2-c — `internalFlashSizeCheck()` `while(1)` (LOW, boot-path only).**
   `drv_hw.c:92`. Does not match the timeline; recorded for the hang-class
   inventory.
4. **No NV error-branch spin-forever found (negative result).** All `drv_nv.c`
   loops bounded; two-phase commit and `forceChgSec` recovery sound; orphaned
   `READY` sectors are harmless.

---

## 9. Recovery robustness note (feed into Part C)

`main()` starts the watchdog only *after* `user_init()` returns
(`apps/common/main.c:62-67`), and `user_init()` is where `stack_init()`
(and therefore `nv_init()`) runs. A flash/NV hang during `user_init()` would
still be unrecoverable even with the watchdog enabled. The configure-pass
writes that this incident implicates happen *later*, in the main loop, so the
Part C watchdog **would** catch them — but the watchdog's coverage boundary
must be documented as "post-`user_init()` only". That is consistent with
`FALLBACK_DESIGN.md` §4.1's already-known pre-`stack_init()` gap, widened
slightly: the whole of `user_init()` runs with IRQs off and before the
watchdog starts.

**Recommended (design-only, do not ship here):** when
`MODULE_WATCHDOG_ENABLE` becomes 1, the `flash_wait_done()` WIP-stuck and
`mspi_wait()` unbounded cases are exactly what the 600 ms interval is for.
Do not shrink the 600 ms interval below ~100 ms in any future tuning: a
legitimate 4 KB sector erase already reaches ~80 ms worst-case.
