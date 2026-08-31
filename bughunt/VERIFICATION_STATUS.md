# MOES verification status

This is the public-safe, authoritative status for `moes-ts0505b`. It separates
observations from static evidence and hypotheses. It intentionally omits local
network details and full hardware identifiers.

## Decision status

**Build 16 is BENCH-PROVEN through the death-trigger class (2026-08-29);
Build 16 is the new known-good bench image. Build 15 remains REJECTED.**
The 13→16 OTA installed at 2026-08-28 16:38:30, then passed every gate that
killed Build 15 plus a full overnight soak: configure pass + interview +
identity readback `v1.16s3.3`, 8 h 13 m unattended soak with zero
`device_announce`/`MAC_NO_ACK` (z2m continuous, 0 restarts), a second forced
configure pass (completed same-second, 12 min clean after — Build 15 died
20–60 s post-configure), and post-soak SWire flash-integrity reads showing
app `0x8000` and OTA staging `0x70000` both byte-exact vs the built image
(no runaway writes; one suspect staging region was disproven as a read
artifact via double re-read). Build 15 stays quarantined: the 13→15 OTA
killed the bench module ~20 s after its configure pass via a corruption
cascade ending in the firmware programming ~100 KB of bit damage over its
own application flash (`build15_runaway_cascade.md`); the same module was
SWire-recovered to byte-exact Build 13 before the 13→16 hop. Field rollout
to the 45 lights still requires the Phase-1 gates of `OTA_TEST_PLAN.md`
and an explicit user GO — bench-proven is not field-proven.

**Build 20 is HARDWARE-VERIFIED for colour output on one fixture
(2026-08-29) — the first build in this project to pass the visual gate.**
SWire-installed on the pilot fixture (byte-exact readback), booted by a true
mains cycle, joined and stable. With zero concurrent programmer activity and
12 s between commands, an observer confirmed xy red, xy green, xy blue and
colour-temperature white in order, every command landing first time.
Corroborated at the register level: xy red drove `R(ch4)=7370` with G/B/CW/WW
all at 0; colour temp 400 gave `CW=519 WW=3968`. Boundary: ONE fixture, bench
mains, short window, no soak — this is an output-path proof, not a stability,
duration, or fleet proof.

**Build 21 is HOST/BUILD-ONLY (2026-08-29).** It adds the EUI byte-order fix
(factory display-order ASCII parsed, OUI-validated, then reversed into the
SDK's LSB-first `addrExt_t`) plus a host test executing the real firmware
object. It has never been flashed or powered. It corrects identity creation on
a factory-new/blank ZB_INFO boot only; it does not and must not rewrite an
already-persisted identity, so it does not by itself migrate a converted
fixture.

**Build 19 was HOST/BUILD-ONLY and is superseded.** It was later installed and
did run (its Colour `CurrentX`/`CurrentY` registration was confirmed live), but
its XY work was incomplete in a harmful way: `MoveToColor` recorded XY and
switched colour mode without calling `light_fresh()`, and both render paths
route non-colour-temperature modes through `hsvToRGB(currentHue,
currentSaturation, …)` — fields no XY command ever wrote. The light therefore
rendered stale HSV(0,0) = white. Build 20 fixes exactly that.

**Builds 17 and 18 have later limited silicon observations, not an accepted
gate result.** The journal records replica operation plus a Build 18 OTA
install on the replica and a fixture, with radio/attribute command evidence.
Those observations did not pass visual/PWM output verification, and later
fixture instability prevents promoting either build to a known-good result.

## Evidence matrix

| Category | Status | Evidence / boundary |
|---|---|---|
| Hardware | Fact | Target family is ZT3L/TLSR8258. Multiple devices were affected; the precise incident inventory is local-only. Current bench identity/build is not wired-confirmed. |
| Flash layout | Static evidence | App `0x8000`; staging `0x70000`; descriptor `0xF7000`; NV `0xD8000`. |
| Build 12 artifact | Fact | Raw app is 202404 bytes with SHA-256 `012ac8bc015419fa1eeb255e11599e8cc87a17d5f6cda73878cdea0c2ada4295`; OTA payload matches it. It is only a build-11 version bump. |
| Descriptor fix | Static evidence | Build 11/12 write `{0x70001, 0x70000, 1}` then reset. The guard is `word0 == byte8 + word1`; build 09 used image size as word1, so its install is declined. |
| Custom-to-custom OTA | **Silicon-proven (2026-08-28)** | Bench ZT3L, rescue-latched (probation 6): v1.13→v1.14 OTA completed 09:26:50, device rebooted, rejoined, re-interviewed, raw-ZCL `swBuildID` read back `v1.14s3.3`, z2m `installed_version` 286142467, zero `device_announce` in the 40 min after (no reset loop). Image sha256-verified workstation→z2m host. The 13→14→15 chain proves an installed custom build installing another custom build. |
| Rescue stable-timer clear | **Silicon-proven (2026-08-28)** | After the 09:26:50 install boot the probation NV item 0x71 was written to 0x00 with normal sector-rotation records — `moes_rescueStableTimerCb`'s joined+progress clear path ran on real hardware (~21 min cadence, dump at 09:58). |
| Build 16 death-trigger A/B | **Silicon-proven (2026-08-29)** | 13→16 OTA installed 2026-08-28 16:38:30 (2,766 s transfer, device self-reported 286076931→286273539); configure pass, interview, `v1.16s3.3` readback all same-minute. Overnight soak 16:38→00:51 (8 h 13 m): zero announces from the module, zero `MAC_NO_ACK` network-wide, 9,242 device messages, 0 leaves/0 address changes. Repeat-configure 2026-08-29 00:53:49 completed same-second, 12 min clean after (Build 15 died 20–60 s post-configure). Post-soak stalled SWire reads: app `0x8000–0x3A2D4` and staging `0x70000–0xA22D4` byte-exact vs built image sha `10d30545…`. Boundary: single module, bench power, ~12 h total runtime — not a field-power/RF-duration proof. |
| Build 20 colour output | **Silicon-proven, single fixture (2026-08-29)** | Pilot fixture, SWire-installed and readback-verified, booted by a true mains cycle. Observer-confirmed xy red / xy green / xy blue / colour-temp white, every command landing first time with no concurrent programmer activity. Register corroboration: xy red → `R(ch4)=7370`, G/B/CW/WW = 0; colour temp 400 → `CW=519 WW=3968`. Channel map read live from `moes_chan[]`: R→PWM4, G→PWM1, B→PWM3, CW→PWM5, WW→PWM0. Boundary: one fixture, bench mains, short window, no soak. Not a stability, duration, or fleet result. |
| Build 21 candidate | **Host/build-only (2026-08-29)** | Seven host suites pass, including `tools/eui_hosttest` executing the real `light/moes_eui.c` (golden vector `a4c138…` display ASCII → SDK LSB-first bytes; lower/upper/mixed case; invalid hex, wrong OUI and fail-unchanged). Both OTA containers verified. Establishes source/build contracts only: no Build 21 flash, OTA, fixture command, or fleet action has occurred. Its identity fix applies to factory-new/blank `ZB_INFO` creation only. |
| Byte-reversed EUI on fresh join | **Root cause established (2026-08-29)** | A converted fixture that loses its NV rejoins under a byte-reversed EUI, abandoning its coordinator record, friendly name, and bound automations. Measured, not inferred: the SDK patch hooks `moes_flashGetIeee()` into `generateIEEEAddr()`, and a read-only SWire read of the live MAC PIB at `g_zbInfo + 0x0C` returned the factory display-order bytes unchanged, which the stack then presents reversed. Fixed host-side in Build 21; NOT yet proven on hardware. Fleet consequence: do not convert further fixtures until an installed build has demonstrated a correct fresh-join identity. |
| Builds 17/18 later observations | **Limited silicon observation** | The replica ran the builds; Build 18 OTA-installed on both the replica and a fixture, with radio/attribute command evidence. They did not pass visual/PWM output verification, and later fixture instability means this is not an accepted gate or a root-cause finding. |
| Liveness tests | Observation | The recorded baseline is 27/27. The current checkout's 28 isolated scenarios all pass, but neither count models live timer, IRQ, or stack behavior. The fuse non-fire cause and build-10 one-shot re-arm are unverified on silicon. |
| Wedge | Observation | Devices have become radio-silent/wedged. No exact halted PC/SP has been captured from a *field* fixture. |
| Wedge root cause | Split (2026-08-28) | The old MSPI/NV theory is refuted. MAC-scan, stack reconstruction, and parent-loss narratives remain leads for the *field* fixtures. The 2026-08-28 *bench* wedge is fully diagnosed: a Pi USB undervoltage (dmesg 06:20:20) sagged the kit-fed 3V3 and hung the TLSR8258 — CPU instruction-frozen at `rf_setTrxState`'s state poll with `reg_irq_mask=0`, `reg_tmr_ctrl=0` (never left `user_init`), SRAM statically garbled, its own stores not landing; dozens of RST resets did not clear it; a true power cycle (kit USB unplugged) recovered it and the same Build 13 booted and rejoined. Fingerprint and recovery procedure in the session notes. Build 15's early-boot watchdog bounds the firmware-side (non-power) variant of this class but cannot clear a hung SRAM controller — only power removal can. |
| Pi UART | Fact | GPIO14/15 use `/dev/ttyAMA0`; `/dev/serial0` points to `ttyAMA10`, the debug connector, not the GPIO header UART. |
| SWS wiring | Observation | Reset-held loopback returned `55 aa 00 ff` exactly through the shared node. It proves loop wiring/node only, not target protocol response. |
| SWS attempts | Observation | No response to no-reset SRAM read at 460800, more than 20 reset-release attempts at 460800, about 10 at 921600, or a cold-power catcher. No flash/app header was captured; no write/erase was performed in that work. |
| UART attempts | Observation | Passive 115200 saw isolated framing-error/junk, reset capture became silent, PB1/pin16 at 1,000,000 produced only `ff`, and a harmless unsupported framed command received zero bytes. No OTA START (`0x0210`) was sent and no staging erase occurred. |
| UART OTA support | Static evidence + inference | `UART_ENABLE=0` and the first preserved `0x6a0c` bootloader bytes lack raw `0x0210`/`0x0211`/response constants. This strongly supports unavailable/compiled-out UART OTA, but is not proof. |
| Wired writes | **Validated (2026-08-28, TB-03F kit)** | The Pi bitbang-era results are superseded: via the TB-03F kit at 230400 baud, Build 13 was written to `0x8000-0x396C4` with per-sector erase, then read back with one guard sector on each side: both guards unchanged, all 202436 app bytes exact, tail erased to `0xFF`, flash status `0x00`. Full 1 MiB pre-write snapshot taken and hash-recorded first. Kit SRAM/register reads are valid (the earlier "untrustworthy" call was the garbled memory being real). |
| Radio identity | Inconclusive observation | No actual announcements/messages were recorded after resets and counters did not change; the bench may have been out of range, so this does not identify the unit. |

## Current safe state and next step

Build 16 remains the latest accepted known-good *bench-soak* gate; Build 20 is
the only build with a hardware output proof, and that proof is one fixture over
a short window with no soak behind it. The Build 17/18 observations above remain
bounded evidence rather than a release decision. Build 21 has no hardware
standing at all: do not flash, OTA, alter a mains-powered fixture, or perform
any fleet action merely because its host tests or artifacts pass.

Two operational rules govern any hardware step here, both learned by
misdiagnosis and each having produced a confident wrong conclusion:

1. **Only true power removal boots this board.** The programmer's soft reset and
   its RST line each fail differently and convincingly (`PC=0`; a loop in
   `FLL_STK`/`BSS_CLEAR`; a park in `start_suspend`'s NOP sled). Never judge
   firmware from a programmer-driven reset.
2. **Programmer activity deafens the radio.** Never interleave SWire reads with
   over-the-air commands and then draw conclusions about command reliability.
   Relatedly, a coordinator's published state after a `set` is optimistic and is
   not evidence the device acted. Any future hardware step requires a separately
approved staged plan, a recoverable sacrificial target/backup, and eyes-on LED
or direct PWM-register evidence for any claimed output behaviour.

## Superseded reports

Historical reports remain for provenance, but their scope is constrained by
the correction banners in `HANG_FINDINGS.md`, `mac_scan_wedge.md`,
`boothang_stack.md`, `fuse_no_fire_b09.md`, `idle_parent_loss.md`, and
`install_sm.md`.
