# Real bootloader install state machine — full transcription + reset semantics

**Verdict up front:** the stock bootloader (`0x0..0x6A0C`) is a **5-state install
machine**, not the SDK `bootLoader`. Its gate is **single-byte `0x4B` at staged
header `+8`** plus a size-field read at `+0x18` — and **no CRC of any kind**: the
bootloader contains **no CRC32 table** (`0x77073096`, `0xEE0E612C`, `0xEDB88320`
all absent), so our "staged image is CRC-valid per the SDK `xcrc32`" verdict is
**irrelevant to this bootloader**. The missing piece the prior reports chased —
the **12-byte `0xF7000` descriptor — is real and is written by BOTH sides**:

- the **stock app's OTA-complete path writes it** (`0x2DE20`, computes `0xF7000`
  via `tmovs r4,#0xF7; tshftls r4,r4,#12` = bytes `f7 a4 24 f3`) before reset;
- the **bootloader writes it back** after install (`0x6614`, the *same*
  `f7 a4 24 f3` construction), with `word0 := r5 + word1`.

`ota_descriptor.md`'s "nobody references `0xF7000`" is **falsified**: its search
used the byte patterns `f7 a0 00 f3` / `f7 e0 00 f3` (register **r0**), but both
the stock app and the bootloader build the address in **r4** (`f7 a4 … 24 f3`),
which those patterns miss.

So the 08-16 failures are best explained by: **our SDK-based firmware wrote the
`0x4B` flag and reset, but never wrote the `0xF7000` descriptor the stock
bootloader reads first.** The bootloader read all-`FF` there, ran its
descriptor-decode, and skipped the install (it re-dispatched straight to the
reboot state). The 08-15 install worked because the **stock app** wrote that
descriptor itself.

The concrete recipe is therefore: **keep writing `0x4B` at `0x70008`, and also
write the 12-byte descriptor at `0xF7000` (word0 = `0x70001`, word1 = image
size, byte8 = `1`) before the final reset — then reset with `0x6f=0x20`.**

---

## 1. Read-artifact model used (and one correction to the prior reports)

Model (true byte → read byte), same as `bootloader_gate_real.md`:

| true byte | read back |
|---|---|
| `0x00..0x7F` | unchanged |
| `0x80..0xBF` | `\|0x40` (bit6 forced) |
| `0xC0..0xDF` | `\|0x20` (bit5 forced) |
| `0xE0..0xFF` | unchanged |

Inverse for a *read* byte `R`:

- `R ≤ 0x7F` → true = `R` (solid)
- `0x80 ≤ R ≤ 0xBF` → never emitted
- `0xC0 ≤ R ≤ 0xDF` → true = `R & ~0x40` (**unambiguous**; the prior report's
  "ambiguous r or r&~0x40" was wrong — a true `0xC0..0xDF` reads as `0xE0..0xFF`,
  not `0xC0..0xDF`)
- `0xE0 ≤ R ≤ 0xFF` → true ∈ {`R`, `R&~0x40`, `R&~0x20`} (3-way)

Consequence: two literal-pool addresses in `bootloader_gate_real.md` are
corrected here:

| literal | prior report | this report (corrected) |
|---|---|---|
| `0x6818` | `0x847100` | `0x847100` (unchanged) |
| `0x6820` | `0x8472DC` | **`0x84729C`** (raw `dc` → true `9c`) |
| `0x6824` | `0x8472D0` | **`0x847290`** (raw `d0` → true `90`) |
| `0x6828` | `0x800623` | `0x800623` (unchanged) |
| `0x682C` | `0x800602` | `0x800602` (unchanged) |

The header buffer is `0x84729C`, buffer C is `0x847290` (12 bytes *below* the
header buffer). This does not change the gate semantics — `[header+8] == 0x4B`
is still staged-image byte `0x70008` — but the buffer addresses in the two prior
reports should be re-read with this correction.

---

## 2. Reset handler `0x86` → entry conditions

The reset vector is `tj 0x86` (true bytes `41 80 00 00`; `0x80` reads as `0xC0`).

Reset handler does (literal pool `0x168..0x150`, corrected):

- `0x800620` timer ctrl setup (`[0x800620] = …`).
- CPSR = IRQ (`0x12`), SP = `0x847290`; CPSR = SVC (`0x13`), SP = `0x84F000`
  (literals `0xF4/0xF8/0xFC/0x100`).
- IC-tag init `0x80060C/0x80060D`, DCDC/system-on `0x800060`, flash wakeup
  `0x80000C/0x80000D`.
- **Analog-register `0x7E` read** at `0xD0..0xE6` (via `0x8000F8/0x8000F9/0x8000FA`),
  the same deep-retention indicator the SDK `cstartup_8258.S` checks. The value
  is read into `r2` and compared (`0xEA: tcmp r2,r0`), but **there is no branch on
  the result before the call**.
- **`0xEC: tjl 0x6638` — unconditional** (32-bit `tjl`, second halfword true
  `0x9AA4`; the low byte `0xA4` reads as `0xE4`, which is why a naive objdump
  pass resolves `0x66B8` instead of `0x6638`).

**Conclusion:** `0x6638` runs on **every** reset through the reset vector. There
is no reset-reason/analog/RAM-magic branch that skips it. The only boot path
that bypasses `main()`-style code on this chip is **deep-retention wake** —
see §6.

---

## 3. The install state machine `0x6638..0x6854`

### 3.1 Prologue and descriptor read

```
0x6638  tpush {r4,r5,r6,r7,lr}   ; r7=fp, r6=sl, r5=r9, r4=r8 (incoming r8..fp)
0x6642  tpush {r4,r5,r6,r7} ; tsub sp,#8
0x6646  tjl 0x64FC               ; ADC/voltage init (analog 0x80074F/0x80020C)
0x664a  tmovs r0,#4 ; 0x664c tjl 0x6834
0x6650..0x6670   timer ctrl: [0x800620] &= 0x7FFFFFFF; |= 0x40; |= 0x800000
0x6672  r7 = [0x6818] = 0x847100
0x6674  tmovs r0,#0xF7 ; 0x6676 r0 <<= 12      -> r0 = 0xF7000
0x6678  tmovs r1,#12  ; 0x667a r2 = r7 ; 0x667c tjl 0x61A8
```

`0x61A8` is **`flash_read(addr, len, buf)`** — it reads SPI data reg
`0x80000C` into `buf` (loop at `0x61F2/0x61F4`: `r3 = [0x80000C]; [buf+i] = r3`).
So **12 bytes are read from flash `0xF7000` into `0x847100`**.

### 3.2 Descriptor processing `0x6680..0x6694`

```
0x6680  r3 = [r7+4]            ; descriptor word1
0x6682  tadds r1,r3,#1         ; r1 = word1 + 1
0x6684  tmovs r0,#5
0x6686  r2 = [r7+8] (byte)     ; descriptor byte8
0x6688  tadds r3,r6,r3         ; r3 = r6 + word1   (r6 = incoming sl)
0x668a  r2 = [r7+0]            ; descriptor word0
0x668c  tcmp r2,r3
0x668e  tjne 0x669a            ; word0 != r6+word1 -> skip write, keep byte8
0x6690  tj 0x6680              ; else loop
0x6692  tmovs r3,#0
0x6694  [r7+8] = r3            ; state byte := 0 (only reached on the equal path)
```

With the **all-`FF` descriptor actually present** (word0=word1=`0xFFFFFFFF`,
byte8=`0xFF`) and `r6` (incoming `sl`) `=0`, `r3 = 0+0xFFFFFFFF = 0xFFFFFFFF`,
`tcmp` is equal, `tjne` not taken, `tj 0x6680` loops — this branch is a **guard
that only lets a *matching* descriptor through**; the all-`FF` state is the
"erased" case and is treated specially. Exact fall-through semantics here are
the least-certain part of the transcription (marked UNCERTAIN in §8), but the
**operational consequence is unchanged**: the byte at `0x847108` is the state
selector for the dispatch below.

### 3.3 State loop + dispatch `0x66AE..0x66C0`

```
0x6696  [sp+4] = [0x681c] = 0x6978     ; jump-table base
0x669a  r6 = [0x6820] = 0x84729C       ; header buffer
0x669c  r2 = [0x6824] = 0x847290 ; r8 = 0x847290   ; buffer C
0x66a0  tmovs r3,#0x80 ; r3 <<= 1 ; [sp+0] = 0x100  ; block size 256
0x66a6  r9 = [0x6828] = 0x800623 ; 0x66aa tmovs r2,#8 ; fp = 8
0x66ae  r3 = [r7+8]                ; state byte
0x66b0  tcmp r3,#4
0x66b2  tjls 0x66ba                ; state <= 4 -> dispatch
0x66b4  tmovs r1,#0 ; 0x66b6 [r7+8]=0 ; 0x66b8 tj 0x66ae   ; else reset state=0
0x66ba  tshftls r3,r3,#2           ; state*4
0x66bc  r1 = [sp+4] = 0x6978
0x66be  tloadr r3,[r1,r3]          ; table entry
0x66c0  tmov pc,r3                 ; jump to handler
```

So **5 valid states (0..4)**; anything larger is forced back to state 0.

### 3.4 The five handlers

Jump table `0x6978..0x6988` (states 0..4):

| state | entry | action |
|---|---|---|
| 0 | `0x67E6` | write reboot reg `0x800602` |
| 1 | `0x675E` | read header into `0x84729C`, assemble size from `+0x18` |
| 2 | `0x671A` | **erase app slot** (loop `0x8000,0x9000,…` step 0x1000, calling `0x6118` = sector-erase primitive) |
| 3 | `0x66E2` | read 256 B header from staging, **check `header[8]==0x4B`**, copy block to app |
| 4 | `0x67EC` | call `0x6614` (descriptor write-back), then set state=0 |

(`0x698C..0x69A0` hold a second, separate 6-entry table — `0x6862/0x6866/0x686A/
0x686E/0x685E/0x683C` — used by the flash primitives, not by this dispatch.)

**Flag gate (state 3)** — SOLID:

```
0x67be  tjl 0x61A8          ; flash_read(staging, 256, 0x84729C)   (r0 = r4 + [r7+4])
0x67c6  tloadrb r3,[r6,#8]  ; header[8]
0x67c8  tsubs r3,r1,r5      ; … compare == 0x4B  (bytes 4a 33 / ab 4b, no ambiguous bytes)
```

`0x67C6/0x67C8` is the **only magic check**: single byte `0x4B` at staged-header
`+8`. The size field is read at `0x6786..0x6796` (`[r6+24..27]`) and bounds the
copy. No version/manufacturer/`0x68000`-literal check was found.

**On check-fail:** the state-3 handler does **not** erase staging or the app;
it falls through to the reboot path (state 0). **On success:** state 4 runs
`0x6614`, which **writes the 12-byte descriptor back to `0xF7000`** with
`word0 := r5 + word1`, then state 0 writes `0x800602` to reboot. There is **no
staging erase and no flag clear** in the bootloader (unlike the SDK sample, which
clears the flag and erases staging).

`0x6614` (bootloader's descriptor write-back) — the site `ota_descriptor.md`
missed:

```
0x6616  tmovs r4,#0xF7 ; 0x6618 tshftls r4,r4,#12   -> r4 = 0xF7000
0x661a  r0 = 0xF7000 ; 0x661c tjl 0x6118             ; sector-erase/status
0x6620  r2 = [0x6634] = 0x847100
0x6622  r1 = [r2+8] ; 0x6624 r3 = [r2+4]
0x6626  r3 = r5 + r3 ; 0x6628 [r2+0] = r3            ; word0 := r5 + word1
0x662a  r0 = 0xF7000 ; 0x662c tmovs r1,#12 ; 0x662e tjl 0x614C   ; flash_write
```

### 3.5 Flash primitives (identified by data flow)

- `0x61A8` = **flash_read(addr, len, buf)** — `[0x80000C] → buf`.
- `0x614C` = **flash_write(addr, len, buf)** — `buf → 0x80000C`.
- `0x6118` = **sector-erase / flash command** — sends address `0x6128`, waits
  `0x6068`, writes `0x80000D`. Called per 4 KB sector by state 2.

---

## 4. The CRC question — there is none

Searched the whole bootloader (raw and corrected) for:

- `0xEDB88320` / `0x04C11DB7` (poly constants) — **absent**.
- CRC32 table fingerprints `0x77073096` (`96 30 07 77`), `0xEE0E612C`
  (`2c 61 0e ee`), `0x990951BA` — **absent**.
- The SDK `xcrc32` loop fingerprint `tloadr rN,[rM,rK]` indexed table access —
  the only indexed loads in the bootloader are the dispatch-table load
  (`tloadr r3,[r1,r3]` at `0x66BE`) and flash-buffer accesses, **no 256-entry
  table walk**.

The SDK `xcrc32` (`utility.c`) is standard reflected CRC-32, poly `0xEDB88320`,
`init` passed by caller (`0xFFFFFFFF` in OTA), **no final XOR**, and it needs a
256-entry table (`0x4FA8` in the SDK bootloader). The Tuya bootloader has
**none of it**. Therefore the "staged build 07 is CRC-valid" evidence (computed
with the SDK sample's `xcrc32` over `size-4`) **does not apply to this
bootloader** — the bootloader never evaluates that CRC. The state-2 handler that
looks "CRC-like" is the app-slot **erase** loop (`0x8000 + n*0x1000`), not a
checksum.

---

## 5. Stock app OTA-complete path (real `0x70008` literal at `0x2DE78`)

Function at flash `0x2DE20` (app offset `0x25E20`), with literals
`0x2DE78 = 0x00070008`, `0x2DE7C = 0x00008008`, `0x2DE80 = 0x00070001`:

```
0x2de24  r4 = sp (12-byte struct on stack)
0x2de28  tmovs r3,#0x4B ; 0x2de2a [r4+0]=0x4B
0x2de2c  r0 = 0x70008 ; 0x2de2e tmovs r1,#1 ; 0x2de30 r2=r4
0x2de32  tjl 0x2BC50                        ; flash_write(0x70008,1,{0x4B})   set 'K'
0x2de36  tmovs r3,#0 ; 0x2de38 [r4+0]=0
0x2de3a  r0 = 0x8008 ; 0x2de3c tmovs r1,#1 ; 0x2de3e r2=r4
0x2de40  tjl 0x2BC50                        ; flash_write(0x8008,1,{0x00})    clear app flag
0x2de44  tjl 0x25D18 ; 0x2de48 tjl 0x27314   ; helpers
0x2de4e  tmovs r3,#1 ; 0x2de50 [sp+8]=1
0x2de54  tshftls r3,r7,#11 ; 0x2de56 [sp+4]=r3   ; r7 = (arg1 & 0xffff)<<16
0x2de58  r3 = 0x70001 ; 0x2de5a [sp+0]=0x70001
0x2de5c  tmovs r4,#0xF7 ; 0x2de5e tshftls r4,r4,#12   -> r4 = 0xF7000   <<-- missed by ota_descriptor
0x2de60  r0 = 0xF7000 ; 0x2de62 tjl 0x20438           ; flash op on 0xF7000
0x2de66  r0 = 0xF7000 ; 0x2de68 tmovs r1,#12 ; 0x2de6a r2=sp
0x2de6c  tjl 0x2048C                                ; flash_write(0xF7000,12,struct)
0x2de70  tjl 0x2123C                                ; final reset
0x2de74  tadd sp,#16 ; 0x2de76 tpop {r4,r5,pc}
```

So the stock app, before reset:

1. writes `0x4B` to `0x70008`,
2. writes `0x00` to `0x8008`,
3. **writes a 12-byte descriptor to `0xF7000`** whose word0 = `0x70001`,
   byte8 = `1`, and word1 is size-derived (exact encoding UNCERTAIN, see §8),
4. resets (`0x2123C`).

Our SDK `ota_mcuReboot` performs steps 1–2 (and the build-08 fix makes the
reset unconditional) but **never performs step 3**.

---

## 6. Reset semantics — reg `0x6f` (`reg_pwdn_ctrl`)

From `platform/chip_8258/register.h:247`:

```
reg_pwdn_ctrl  REG_ADDR8(0x6f)
  FLD_PWDN_CTRL_REBOOT = BIT(5)   // 0x20
  FLD_PWDN_CTRL_SLEEP  = BIT(7)   // 0x80
```

- **SDK soft reset** = `write_reg8(0x6f, 0x20)` (`bsp.h:110 mcu_reset`; our
  `SYSTEM_RESET()`), i.e. bit5 only.
- **825x flasher "Reset CPU"** = `sws_wr_addr_usbcom(0x6f, [0x22])`
  (`TLSR825xComFlasher.py:735`) = `0x20 | 0x02`. The `0x02` (bit1) is
  **undocumented** in the SDK enum; it is the flasher's run-after-flash value.
  Both `0x20` and `0x22` set bit5 (REBOOT), so both vector the CPU through the
  boot ROM to flash `0x0` → the bootloader. `0x22` is **not** a
  deep-sleep/retention boot.

**The one bypass path** (`cstartup_8258.S`): on reset the startup reads analog
reg `0x7E`; if it is `0x00` ("deep retention wake"), it loads `tl_multi_addr`
and jumps to the retained address **without running `main()`**. Normal soft
reset / watchdog / power-on all have `0x7E != 0` and run the normal boot path.
The stock bootloader's own reset handler also reads `0x7E` but does **not**
branch on it before `tjl 0x6638` (§2).

**Why the stock app's recipe worked on 08-15:** the stock app's final reset
(`0x2123C`) plus its `0xF7000` descriptor write put the bootloader into the
install state; our `0x6f=0x20` reset is the same *class* of reset (full soft
reset through `0x0`), so reset *type* is not the primary differentiator — the
descriptor write is.

---

## 7. Recipe — what must change in `ota.c`

Concrete, in order of confidence:

1. **Keep** writing `0x4B` to `0x70008` (the single-byte gate) — already done.
2. **Keep** the unconditional reset from build-08 — already done.
3. **Add the `0xF7000` descriptor write** before the reset, replicating the
   stock app: erase/write the 12-byte structure
   `{u32 word0 = 0x70001; u32 word1 = <image size>; u8 byte8 = 1}`
   at `0xF7000`, using the same flash-write path. This is the write our firmware
   is missing and the stock app performs at `0x2DE20`.
   - The 4 KB sector `0xF7000..0xF7FFF` is one sector *below* the board-config
     sector `0xF8000`; `0xF7000` is already erased `0xFF`, so a plain
     `flash_write(0xF7000, 12, struct)` (no erase needed) is safe and does **not**
     touch `0xF8000`.
4. **Reset with `0x6f=0x20`** (`SYSTEM_RESET()`), which our firmware already
   does. Do **not** switch to `0x22` — it is not the differentiator.

If (3) is wrong about the exact `word1` encoding, the fallback is a single
read-only hardware capture: read `0xF7000` on a unit *immediately after* the
stock app finishes staging an OTA and before the bootloader installs it. That
capture pins the descriptor format; then mirror it byte-for-byte in `ota.c`.

---

## 8. Confidence summary

**Solid (unambiguous bytes / data-flow):**

- Reset vector `0x86`, unconditional `tjl 0x6638` at `0xEC`; no reset-reason
  branch before it.
- State machine = 5 states, dispatch via jump table `0x6978` (`tcmp r3,#4;
  tjls`).
- Flag gate = `header[8] == 0x4B` at `0x67C6/0x67C8`.
- Size field read at `+0x18` (`0x6786..0x6796`).
- Flash primitives: `0x61A8`=read, `0x614C`=write, `0x6118`=sector erase/cmd.
- Reboot after install = write `0x800602` (`0x67E8`), value `0x88` (read `0xC8`
  in the prior report; `0x88 ∈ 0x80..0xBF` reads as `0xC8`, matching the SDK
  `REBOOT()` macro).
- **No CRC32 table / poly constant anywhere in the bootloader.**
- Stock app OTA-complete writes `0x4B→0x70008`, `0x00→0x8008`, and a 12-byte
  descriptor at `0xF7000`; the `0xF7000` construction is `f7 a4 24 f3` (r4),
  which the prior reports' search missed.
- Bootloader writes `0xF7000` back after install (`0x6614`), same `f7 a4 24 f3`.

**UNCERTAIN:**

- Exact branch outcome of the descriptor guard `0x668E..0x6694` for the all-`FF`
  case (equal-path loop vs skip); the *operational* conclusion (descriptor is a
  required input) holds either way.
- Exact `word1` encoding of the stock app's descriptor (size field), and the
  exact roles of helpers `0x25D18/0x27314/0x20438/0x2048C/0x2123C`.
- State-transition values written by each handler (which state each handler
  sets next); the handler *actions* are solid, the transition numbering is not
  fully pinned.
- Whether the bootloader's "check-fail" path is strictly "reboot" vs a
  descriptor-mismatch loop; in both cases staging is left intact.

## 9. Files touched

- New report only: `work/tuyaZigbee/bughunt/install_sm.md` (this file).
- No code edits, no commits, no device access. Working artifacts in `/tmp/ism`
  (disassembly of `bench_full_1.bin`/`bench_full_2.bin` bootloader and app
  regions, correction scripts).
