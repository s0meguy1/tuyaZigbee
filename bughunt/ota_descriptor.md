# The 0xF7000 descriptor — decisive read + authorship trace

**Verdict up front:** the current `0xF7000` is **all `0xFF`** (two independent
tact=0 passes), identical to both 2026-08-15 full dumps. **Nobody writes
`0xF7000`.** The stock bootloader reads 12 bytes there exactly once (install
path), never writes it; the stock app never references the address at all. The
"install-once / stale descriptor at `0xF7000`" theory is therefore **FALSIFIED**:
the descriptor area is erased, constant, and was erased on the *successful*
08-15 install exactly as it is on the *failed* 08-16 attempts. `0xF7000` is not
the missing piece, and our OTA-complete path should **not** erase or write it.

---

## 1. Current `0xF7000` — hardware read (bench unit, CPU-halted)

Method (tact=0, no reset, chip kept halted):

```
ssh <bench-pi> ; cd ~/tlsr ; fuser -v /dev/ttyAMA0   # exit 1, no holder
SWS_RDSIZE=0x40 SWS_DIV=110 python3 -u pi_run.py TLSR825xComFlasher.py \
  -p /dev/ttyAMA0 -b 460800 rf 0xF7000 0x40 boothang_2026-08-16/<name>.bin
```

All four reads returned `Read OK: 64 B ... 0 byte-level retries, 0 resyncs`.

| capture | addr | size | result |
|---|---|---|---|
| `f7000_pass1.bin` | `0xF7000` | 0x40 | 64 × `FF` |
| `f7000_pass2.bin` | `0xF7000` | 0x40 | 64 × `FF` (byte-identical to pass1) |
| `f6000.bin` | `0xF6000` | 0x40 | 64 × `FF` |
| `f7400.bin` | `0xF7400` | 0x40 | 64 × `FF` |

Files copied to `dump/bench_2026-08-16_boothang/` (`.bin` + `.log`).

Artifact model applied: `0xFF` is artifact-immune (reads unchanged), both passes
agree with 0 retries/0 resyncs, and the surrounding sector is contiguous erased
flash. The only theoretical ambiguity is a true `0xBF` (bit6-forced reads as
`0xFF`); `0xBF` in an otherwise all-erased sector is implausible and would not
match the 08-15 dumps either. Conclusion: **true `0xFF`, near-certain**.

Cross-check against 08-15: `bench_full_1.bin` and `bench_full_2.bin` are both
`0xFF` for 64 B at `0xF7000`, `0xF6000`, `0xF7400`. So the area is **unchanged**
from before the successful build-06 install.

---

## 2. Authorship — who writes `0xF7000`? **Nobody.**

### 2.1 Bootloader: one read site, zero write sites

Search space: `bench_full_1.bin[0x00000..0x6A0C]` (the whole stock bootloader),
cross-checked against `bench_full_2.bin` (24 differing bytes, all xor `0x20`
bit-5 noise, none in the address construction).

- The **only** `0xF7000` construction in the bootloader is the install-path
  read at `0x6672..0x667C`:
  - `0x6672  tloadr r7,[pc,#420]` → `r7 = [0x6818] = 0x00847100` (SRAM buffer A)
  - `0x6674  tmovs r0,#0xF7`   (`f7 a0`; read back `f7 e0` — byte `0xA0` is in
    the `0x80..0xBF` band and reads with bit6 forced to `0xE0`)
  - `0x6676  tshftls r0,r0,#12` → `r0 = 0xF7000`
  - `0x6678  tmovs r1,#12`      (`0c a1`; read back `0c e1`, same bit6 artifact)
  - `0x667A  tadds r2,r7,#0`    → `r2 = 0x00847100`
  - `0x667C  tjl <flash_read>`  → reads 12 B `@0xF7000` into `0x00847100`
- The 4-byte construction `f7 e0 00 f3` / `f7 a0 00 f3` occurs **exactly once**
  in the whole bootloader (at `0x6674`). No `00 70 0f 00` literal, no `00 f7`
  byte pair. There is **no flash write/erase targeting `0xF7000`** anywhere in
  the bootloader — the address is read-only to it.

### 2.2 Stock app: zero references to `0xF7000`

Search space: `bench_full_1.bin[0x8000..0x5B5F0]` (stock app image), cross-checked
against `bench_full_2.bin`.

- `f7 e0 00 f3` / `f7 a0 00 f3` (the `0xF7<<12` construction): **0 hits**.
- `00 70 0f 00` (LE literal `0x000F7000`): **0 hits**.
- Per-register `tmovs rN,#0xF7` byte scan: the `f7 e0` pairs that do exist are
  **not** followed by `00 f3`, i.e. none of them is a `0xF7000` construction.

**Correction to the prior lead:** the `0x70008` literal "at app offset 0x5097A"
(flash `0x5897A`) in `bootloader_gate_real.md` is a **false positive**. The bytes
at `0x5897A` are `08 00 07 00`, but they sit inside a decreasing 16-bit lookup
table (`0x000D, 0x000C, 0x000B, 0x000A, 0x0008, 0x0008, 0x0007, 0x0006 ...`),
i.e. two adjacent table entries `0x0008`/`0x0007`, **not** a code literal. The
real `0x70008` literal in the stock app is at flash `0x2DE78` (verified: bytes
`08 00 07 00 08 c0 00 00 01 00 07 00` are a genuine literal pool).

Conclusion: the stock app has **no** `0xF7000` reference, so it does not write
the descriptor. Combined with §2.1 (bootloader read-only), the descriptor area
is written by nothing in the entire stock stack. It is permanently erased
`0xFF`.

---

## 3. What this does to the leading theory

Leading theory (task brief): "install-once — the bootloader or the stock app
wrote a descriptor of the installed build-06 image, which now mismatches staged
build 07."

That is now **ruled out on three independent grounds**:

1. Current `0xF7000` is all-`FF` — not a build-06 descriptor.
2. `0xF7000` is all-`FF` in both 08-15 dumps — it never held a build-06
   descriptor.
3. Neither the bootloader nor the stock app has any write path to `0xF7000`
   (bootloader reads once, stock app zero references).

All-`FF` is the constant, default state and it was the state during the
successful 08-15 install. Therefore the install decision does **not** depend on
`0xF7000` content.

---

## 4. The recipe

**Do not add a `0xF7000` erase/write to our OTA-complete path.** That would be
chasing a non-existent gate and would burn a 4 KB sector (`0xF7000..0xF7FFF`)
that sits directly below the Tuya board-config sector `0xF8000` — for zero
benefit (the sector is already erased `0xFF`, which is the pass state). The
erase-granularity note in the task is moot: `0xF7000` is in its own 4 KB sector
`0xF7000-0xF7FFF`, one sector below `0xF8000` (board config, never touch).

The install gate the stock bootloader actually uses remains:

1. **Flag gate — `0x4B` at staged-header `+8`** (`0x70008`). Satisfied: current
   staged build 07 has `0x4B` at `+8` (artifact-immune byte).
2. **Size field at `+0x18`** (`0x70018`). Satisfied: staged build 07 size raw
   `0x000315E4`, true `0x000315A4 = 202148` B (high byte `0xE4` reads as
   `0xA4`/`0xC4`/`0xE4`; the built `.bin` is 202148 B, pinning true `0xA4`).
   Well under the `0x68000` (425 984 B) region.
3. **`0xF7000` 12-byte read** — constant all-`FF`, identical on success and
   failure. Not a variable we control or need to.

Because (1)+(2)+(3) were all satisfied on 08-16 yet the install did not happen,
the actual blocker is **outside** the `0xF7000` descriptor. What our OTA-complete
path should do:

- **Keep** writing `0x4B` to `0x70008` and **keep the build-08 unconditional
  reset** (already patched). This is necessary and was broken in the build-06
  image that actually ran the 08-16 OTA (`flash_writeWithCheck` gated the reset
  on a read-back that can return `FALSE` after the write has landed — so the
  flag was set but `ota_mcuReboot` may have skipped its own reset; the 14:29 and
  17:20 resets may then have been app-wedge reboots, not OTA-complete reboots).
- **Do not** write/erase `0xF7000`. Reverting to all-`FF` is not needed — it is
  already `0xFF`.
- **Next investigation target (the actual missing piece):** trace the stock
  app's OTA-complete function around the **real** `0x70008` literal at flash
  `0x2DE78` with full artifact correction, to see whether it writes an extra
  marker in **SRAM** or another flash address before reset. The function at
  `0x2DC00..0x2DE70` is a byte-at-a-time bit-manipulation routine (CRC-like)
  that references `0x70008`/`0x8008`/`0x70001`; its exact pre-reset writes are
  still not decoded.

---

## 5. Residual UNCERTAIN items

1. **Staging-address source.** The bootloader has no `0x70000` literal (searched
   all encodings: `00 00 07 00`, `0xE0<<11`, `0x70<<12`). It reads a 12-byte
   descriptor at `0xF7000`, but that descriptor is all-`FF`. Either the staging
   address is hard-wired via a construction I have not yet found, or the
   all-`FF` descriptor is special-cased to a default. This needs a full
   artifact-corrected disassembly of the state machine `0x6638..0x6854`.
2. **Exact semantics of the 12-byte descriptor fields** (`[0x847100]`,
   `[+4]`, `[+8]`) — still UNCERTAIN; only the read site is confirmed.
3. **Stock app's exact OTA-complete pre-reset writes** (SRAM vs flash) — not yet
   traced; the `0x5897A` lead was a false positive, and the real literal is at
   `0x2DE78`.
4. **Size-field high byte** at `+0x18` is artifact-ambiguous (`0xE4` read →
   true `0xA4`/`0xC4`/`0xE4`); pinned to `0xA4` for build 07 by the built file
   size (202148 B), but single-pass reads alone cannot disambiguate it.
5. **Which reset actually ran on 08-16 at 14:29/17:20** — whether those were the
   OTA-complete reset or app-wedge reboots is not established; the build-06
   conditional-reset bug makes "flag written, OTA reset skipped, later app
   reboot" the leading micro-explanation.

---

## 6. Files touched / evidence

- New report: `work/tuyaZigbee/bughunt/ota_descriptor.md` (this file).
- New captures in `dump/bench_2026-08-16_boothang/`:
  `f7000_pass1.bin/.log`, `f7000_pass2.bin/.log`, `f6000.bin/.log`,
  `f7400.bin/.log`.
- No code edits, no git mutations, no flash writes, chip left CPU-halted.
