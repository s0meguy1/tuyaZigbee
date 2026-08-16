# The real stock-bootloader install gate (disassembly of the ACTUAL bootloader)

**Verdict up front:** the stock bootloader at `0x000000..0x006A0C` is **not** the
Telink SDK `bootLoader` app. The previous `ota_no_install.md` premise
("stock == SDK sample, gate = KNLT@0x70008 + size + CRC") is **dead**, confirmed
byte-for-byte. The real gate is a **single-byte `0x4B` check** on the staged
image header read into a **RAM buffer at `0x8472DC`**, plus a size-field read —
and the bootloader contains **no `0x70000`, no `0x68000`, no `0xD8000`, and no
4-byte KNLT constant at all**. It instead computes **`0xF7000`** (`0xF7 << 12`)
as its only flash-address-like constant in the install path. That address is the
most likely missing piece: the stock app writes something there (or nearby) that
our build-06/07 firmware never writes.

Everything below is from `dump/bench_2026-08-15/bench_full_1.bin` bytes
`[0x0 : 0x6A0C]`, cross-checked against `bench_full_2.bin`. All offsets are
flash addresses (bootloader is linked to run at `0x0`).

---

## 1. Read artifacts — how decodes were disambiguated

This rig's SWire reads corrupt bytes by VALUE, not by address:

| true byte | read back as |
|---|---|
| `0x00..0x7F` | unchanged |
| `0x80..0xBF` | `\|0x40` (bit6 forced high) |
| `0xC0..0xDF` | `\|0x20` (bit5 forced high) |
| `0xE0..0xFF` | unchanged |

plus ~0.7% random bit-5 noise (the two dumps differ at 24 bytes in the boot
region, all xor `0x20`). The read **never** emits `0x80..0xBF`. So:

- A read byte in `0x80..0xBF` is **exact** (unambiguous).
- A read byte in `0xC0..0xDF` is ambiguous: true `r` or true `r&~0x40`.
- A read byte in `0xE0..0xFF` is ambiguous: true `r`, true `r&~0x40`, or
  `r&~0x20`.

Disambiguation used: (a) both dumps for the random bit-5 noise, (b) TC32
instruction-validity (objdump `-b binary -m tc32`), (c) the 32-bit `tjl` rule
(first halfword `& 0xF000 == 0x9000`, second halfword also `0x9xxx`), (d)
address sanity for literal pools. Decodes whose bytes were ambiguous are marked
**UNCERTAIN** below; decodes with no ambiguous bytes are **solid**.

The two dumps' boot region differs only at 24 bytes (all bit-5), so the
deterministic bit-6/bit-5 artifacts are identical in both — two dumps alone do
not resolve them; the resolution above is what was used.

---

## 2. Bootloader identity — it is a Tuya build, NOT the SDK sample

Stock bootloader header (true bytes after artifact correction):

```
0x00: 41 80 00 00     tj 0x86            <- reset vector (SDK sample: 58 80 = tj 0xB4)
0x04: 00 00 00 00
0x08: 4b 4e 4c 54     "KNLT"             <- own magic (same as SDK)
0x0c: 00 08 c8 00     ramcode_size=0x800 -> 0x8000 bytes RAM code @0x840000
0x10: ee c0 00 00
0x18: 0c 6a 00 00     size = 0x6A0C = 27148 bytes
0x1c: 00 00 00 00
0x20: 0c 64 c1 e2
0x24: 09 0b 1a 40
```

SDK sample (`build/bootloader/bootloader_ZBWS01A.bin`) header for comparison:
reset vector `58 80` (`tj 0xB4`), size `0x5494` (21652), `+0x0c = 70 02 88 00`,
`+0x10 = 06 81 64 64`. The two binaries are **different builds**, not just
different config constants.

Decisive negative results (searched the whole bootloader, both true and
artifact-corrupted byte forms):

- **No `0x70000`** (`00 00 07 00`), no `0x70008`, no `0x68000`, no `0xD8000`
  literal anywhere.
- **No KNLT 4-byte constant** `0x544C4E4B` (LE `4b 4e 4c 54` — only at `0x08`,
  which is the bootloader's own header magic) and **no two's-complement**
  `0xABB3B1B5` (`b5 b1 b3 ab`).
- No `0xE0<<11` construction (`tmovs r0,#0xE0` = `e0 a0`, then
  `tshftls r0,r0,#11` = `c0 f2`) — i.e. the bootloader never computes `0x70000`
  the way the SDK app does.

Conclusion: the SDK `is_valid_fw_bootloader()` (4-byte KNLT compare) and the SDK
`FLASH_OTA_IMAGE_MAX_SIZE=0x68000` / `FLASH_ADDR_OF_OTA_IMAGE=0x70000`
derivation **do not exist in the stock binary**.

---

## 3. Boot flow

Reset handler is at `0x86` (reset vector `tj 0x86`):

- `0x86`: `tloadr r0,[0x168]` → `r0 = 0x00800620` (timer ctrl reg) — solid
  literal (true `20 06 80 00`).
- `0x88..0x90`: builds a value and writes it to `[0x800620]` (watchdog/timer
  setup; the exact mask is UNCERTAIN — the `0x88` instruction decodes to
  `tmovs r1,#0xF7 ; tshftls r1,r1,#9` but the operand bytes are ambiguous).
- `0x92..0xA0`: MCU status + stack-pointer setup (`tmcsr`, `tmov sp`).
- **`0xEC`: `tjl 0x6638`** — the reset handler calls the install/boot state
  machine directly. (32-bit `tjl`, bytes `90 06 9a a4` after correction; target
  solid because the 32-bit halfword rule pins it.)

So **0x6638 runs on every reset** through the reset handler; there is no
separate reset-reason branch before it.

---

## 4. The install/boot state machine `0x6638..0x6854`

Literal pool `0x680C..0x682C` (true values):

```
0x680C: 0x00800620   timer ctrl reg
0x6810: 0x7FFFFFFF
0x6814: 0x00000BF8   = 3064
0x6818: 0x00847100   SRAM buffer A
0x681C: 0x00006978   jump table (see below)
0x6820: 0x008472DC   SRAM buffer B   <- the header buffer
0x6824: 0x008472D0   SRAM buffer C
0x6828: 0x00800623   timer byte
0x682C: 0x00800602   CPU halt/reboot reg (0x602)
```

Key sequence (address → decode, confidence):

```
0x6650  r4 = [0x680C] = 0x800620; [0x800620] &= 0x7FFFFFFF; |= 0x40; |= 0xC00000   (timer setup)
0x6672  r7 = [0x6818] = 0x847100
0x6674  r0 = 0xF7            ; 0x6676  r0 <<= 12   -> r0 = 0xF7000     (SOLID shift #12)
0x6678  r1 = 12
0x667A  r2 = r7 = 0x847100
0x667C  tjl 0x6228            ; flash primitive (reads 12 bytes @0xF7000 -> 0x847100)  [UNCERTAIN target semantics]
0x6680..0x6690  loop over [r7] / [r7+4] / [r7+8] (descriptor fields)
0x6692  [r7+8] = 0
0x6696  r1 = [0x681C] = 0x6978 (jump table base); [sp+4] = r1
0x669A  r6 = [0x6820] = 0x8472DC     <- header buffer
0x669C  r2 = [0x6824] = 0x8472D0
0x66AE..0x66B8  state loop: [r7+8] < 4 -> dispatch through jump table @0x6978
0x67C6  4a 33   tloadrb r3,[r6,#8]     ; byte @0x8472E4          (SOLID, no ambiguous bytes)
0x67C8  ab 4b   tcmp   r3,#75          ; == 0x4B 'K'             (SOLID)
0x6786..0x6796  reads bytes [r6+24],[r6+25],[r6+27] and assembles the image size
                field from the header at offset +0x18              [offset #26 vs #27 UNCERTAIN]
0x67E6  0b 11   tloadr r3,[0x682C]=0x800602
0x67E8  40 1a   tstorerb r2,[r3]        ; write reboot/halt reg   (value 0xC8 UNCERTAIN)
0x67EA  87 60   tj 0x66AE               ; back into the state loop
```

The jump table at `0x6978` (solid 16-bit values, each padded to 32 bits):

```
0x6978: 0x67E6   0x697C: 0x675E   0x6980: 0x671A   0x6984: 0x66E2
0x6988: 0x67EC   0x698C: 0x6862   0x6990: 0x6866   0x6994: 0x686A
0x6998: 0x686E   0x699C: 0x685E   0x69A0: 0x683C
```

These are the switch cases inside `0x6638` (flash-copy / erase / verify steps).

---

## 5. Accept criteria (what the bootloader actually checks)

1. **Flag gate — `0x4B` at image-header offset `+8`.**
   `0x67C6` loads byte `[0x8472DC + 8]` and `0x67C8` compares to `0x4B`.
   `0x8472DC` is a RAM buffer the bootloader fills by reading the first 256
   bytes of the staged image, so this is the *first byte of "KNLT"* — but only
   the **single byte** `0x4B` is tested, not the 4-byte word. **This is the
   whole magic check.** (SOLID decode.)

2. **Size field at `+0x18`.** `0x6786..0x6796` reads header bytes at buffer
   offset `24..27` (`0x8472F4..0x8472F7`) and assembles a 32-bit size used to
   bound the copy loop. There is **no `0x68000` limit literal** — the limit, if
   any, is a different constant I did not pin down. (Size-field read SOLID;
   exact bound UNCERTAIN.)

3. **No version comparison, no manufacturer/image-type comparison.** No version
   field is read and no such constants exist in the binary. (Negative finding.)

4. **No install-only-if-app-invalid check found.** The app slot `0x8000` is not
   read by the install gate; the only `0x8000`-family literal in the bootloader
   is the peripheral-register base `0x00800000` and register addresses
   (`0x800620`, `0x800602`, SPI regs `0x80000C/0x0D/0x643`, analog regs
   `0x8000F8/F9/FA`). (Negative finding; see §7 for the one caveat.)

5. **No reset-reason analog gate found.** No literal for the deep-retention /
   MCU-status analog registers `0x80007F` or `0x80003A..0x3C` or
   `0x800035..0x39` exists. The analog-register functions at `0x658C`/`0x65CC`
   touch `0x8000F8/F9/FA` (ADC/voltage, not reset reason). The reset handler
   calls `0x6638` unconditionally. (Negative finding.)

6. **Reboot after install** via `0x800602` write at `0x67E8` (the same CPU
   control register the SWire flasher uses; value `0xC8` UNCERTAIN).

---

## 6. The `0xF7000` anomaly (the most likely missing piece)

`0x6674..0x6676` computes `r0 = 0xF7 << 12 = 0xF7000` and then reads **12
bytes** (`r1=12`) into `0x847100` (`r2`). This is the only flash-address-like
constant constructed in the install path, and it is **not** `0x70000`.

- `0xF7000` sits just below the board-config block `0xF8000`.
- The bootloader has **no `0x70000` literal or construction**, so it cannot
  directly read the SDK's staging bank unless the address is supplied at
  runtime (from the 12-byte descriptor read at `0xF7000`, or from SRAM).
- The **stock app does not reference `0xF7000`** either (searched for the
  literal `00 00 f7 00` and the `0xF7<<12` sequence `f7 e0 00 f3` / `f7 a0 00 f3`
  in `0x8000..0x5B5F0`: zero hits). The stock app *does* reference `0x70000`
  (5 literal hits) and `0x70008` (2 hits), same as our firmware.

**Interpretation (UNCERTAIN, needs one hardware check):** the stock bootloader's
real staging/descriptor address differs from `0x70000`. The 12-byte read at
`0xF7000` is a Tuya OTA descriptor; the stock app writes the descriptor at
`0xF7000` (or the bootloader derives `0x70000` from it) before its final reboot,
and our firmware never writes it — so the bootloader reads erased `0xFF`s there,
finds no valid descriptor, and skips the install even though `0x70008 == 0x4B`
is set. The alternative (also possible) is that the descriptor is written to
SRAM, not flash; but the bootloader's `0x847100/0x8472DC/0x8472D0` are inside
its own RAM-code window `0x840000..0x848000` (`.data`/`.bss`), which the
bootloader initializes/zeroes, so an app-set SRAM magic there would not survive.

---

## 7. What is (and is not) an APP-slot `0x8000` check

The task's question (b) — "install-only-if-app-invalid" — is answered **no**
for the gate itself: I found no read of `0x8000` in `0x6638` or its callees.
The one caveat: the bootloader's own header has `ramcode_size = 0x8000`, and the
reset path copies `0x8000` bytes of RAM code to `0x840000` (this is the
bootloader's own `.text`/`.data` relocation, not an app check). The final
`0x800602` write is the reboot that jumps into the freshly written app at
`0x8000`; there is no validity test of the old app before the copy.

---

## 8. Explaining the two observations

**(1) 2026-08-15 — stock-staged build 06 installed.**
The *stock* app's OTA-complete path writes the exact marker/descriptor the
*stock* bootloader expects (its own scheme, which includes the single-byte
`0x4B` header flag and — per §6 — likely a descriptor the stock app writes to
its own address), then resets. The stock bootloader read the descriptor, read
the staged image header into `0x8472DC`, saw `0x4B` at `+8`, copied to `0x8000`,
and rebooted. Everything matched because both sides were the stock Tuya scheme.

**(2) 2026-08-16 — our-staged build 07 did NOT install across two resets.**
Our firmware is SDK-based. Its OTA-complete path writes `0x4B` to `0x70008` and
resets (the SDK flag), but it does **not** write whatever the stock bootloader
reads first (the `0xF7000` 12-byte descriptor, or the stock app's SRAM/extra
flash marker). The stock bootloader therefore never enters the image-copy state,
leaves `0x70000` untouched, and the flag byte at `0x70008` stays `0x4B` — which
is exactly the observed post-mortem (staging intact, flag still set, app slot
still build 06). The "KNLT+CRC-valid" evidence is irrelevant to this bootloader:
it does not perform the SDK's 4-byte KNLT or the SDK's CRC gate at all.

The contradiction "CSMA ran but the earlier check did not" (from the other
agent's SRAM capture) is orthogonal: it is about the *app* (build 06) running
after the reset, not about the bootloader. The bootloader correctly declined the
install for the reason above, then jumped to the old app at `0x8000`, which
rejoined and hit its own hang.

---

## 9. Recipe — what our firmware must write, where, before which reset

Minimal, highest-confidence items first:

1. **Keep writing `0x4B` to flash `0x70008`** (already done by
   `ota_mcuReboot`). It is necessary (the single-byte gate) but not sufficient.
2. **Determine and write the stock bootloader's descriptor.** The concrete lead
   is a **12-byte structure read from `0xF7000` into SRAM `0x847100`**
   (`0x6672..0x667C`). Before the final reset, write that 12-byte descriptor at
   `0xF7000` in the format the stock app uses. Its fields are read at
   `[0x847100]`, `[+4]`, `[+8]` by the loop at `0x6680..0x6690`; the exact field
   semantics (image address / size / flag) are **UNCERTAIN** — recover them by
   disassembling the stock app's OTA-complete path (the `0x70008` literal in the
   app is at dump offset `0x5897A` = app offset `0x5097A`; the surrounding
   function is the stock `ota_mcuReboot`-equivalent and must be re-traced with
   artifact correction).
3. **Reset kind:** the stock app's own final reset is the thing to replicate.
   The bootloader runs on the reset-vector path regardless; prefer the same
   soft reset the stock app issues (write `0x20`/`0x22` to register `0x6F`) —
   but first confirm the stock app's exact reset write, because the descriptor
   may only be honored on a specific reset cause. **UNCERTAIN.**

If (2) turns out to be wrong and the descriptor is actually an SRAM mailbox, the
fallback is to find, in the stock app, the SRAM address it writes immediately
before reboot and mirror that write. The bootloader's own buffers
(`0x847100/0x8472D0/0x8472DC`) are **not** the mailbox — they are inside the
bootloader's RAM-code region and get initialized/zeroed.

**Concrete next check (no SWire writes, read-only):** re-read the stock app
region around `0x5097A` and the stock app's `0xF6000`/`0xF7000` references, and
trace the stock `ota_mcuReboot`-equivalent to see the exact bytes it writes and
the exact reset it issues. That single trace converts §6/§9 from "UNCERTAIN" to
"recipe".

---

## 10. Confidence summary

**Solid (no ambiguous bytes / unique decodes):**
- Stock bootloader ≠ SDK sample (header + reset vector + missing constants).
- Reset handler `0x86`, call to `0x6638` at `0xEC`.
- Literal pool `0x680C..0x682C` values (timer reg, SRAM buffers, jump table,
  reboot reg).
- `0x67C6/0x67C8`: `[0x8472DC+8] == 0x4B`.
- `0x6676`: `r0 <<= 12`; `0x6674`: `r0 = 0xF7` (immediate byte ambiguous, but
  all competing decodes are nonsense → `0xF7000`).
- Jump table `0x6978` entries.
- No `0x70000/0x68000/0xD8000/KNLT-4byte` literals anywhere in the bootloader.

**UNCERTAIN:**
- Exact semantics of the 12-byte `0xF7000` descriptor (`0x6680..0x6690`).
- Whether `0x70000` is supplied by that descriptor or is hard-wired elsewhere
  (I could not find a `0x70000` construction, so descriptor-supplied is the best
  fit).
- Size-limit constant (the `+0x18` field is read, but no `0x68000` literal).
- Reboot register value `0xC8` at `0x67E8`.
- The stock app's exact pre-reboot writes (flash descriptor vs SRAM) — needs the
  app-side trace described in §9.

Toolchain/commands used: `tc32-elf-objdump -D -b binary -m tc32` on extracted
`/tmp/boot1.bin`/`/tmp/boot2.bin`; artifact-correction scripts in `/tmp`
(correct2.py). No files were modified in the repo other than this report.
