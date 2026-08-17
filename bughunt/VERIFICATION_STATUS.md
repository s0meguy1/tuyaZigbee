# MOES verification status

This is the public-safe, authoritative status for `moes-ts0505b`. It separates
observations from static evidence and hypotheses. It intentionally omits local
network details and full hardware identifiers.

## Decision status

**Do not deploy this branch.** Build 12 is experimental and not silicon-tested;
there is no working wedge fix, no validated wired write path, and no proof that
an installed custom build can install another custom build. Use only with a
recoverable dedicated programmer and a proven backup/restore procedure.

## Evidence matrix

| Category | Status | Evidence / boundary |
|---|---|---|
| Hardware | Fact | Target family is ZT3L/TLSR8258. Multiple devices were affected; the precise incident inventory is local-only. Current bench identity/build is not wired-confirmed. |
| Flash layout | Static evidence | App `0x8000`; staging `0x70000`; descriptor `0xF7000`; NV `0xD8000`. |
| Build 12 artifact | Fact | Raw app is 202404 bytes with SHA-256 `012ac8bc015419fa1eeb255e11599e8cc87a17d5f6cda73878cdea0c2ada4295`; OTA payload matches it. It is only a build-11 version bump. |
| Descriptor fix | Static evidence | Build 11/12 write `{0x70001, 0x70000, 1}` then reset. The guard is `word0 == byte8 + word1`; build 09 used image size as word1, so its install is declined. |
| Custom-to-custom OTA | Unverified | The descriptor correction is expected to help, but ours-to-ours installation has not been proven on silicon. Coordinator `installed_version` bookkeeping and a completed download/configure do not prove installation. |
| Liveness tests | Observation | The recorded baseline is 27/27. The current checkout's 28 isolated scenarios all pass, but neither count models live timer, IRQ, or stack behavior. The fuse non-fire cause and build-10 one-shot re-arm are unverified on silicon. |
| Wedge | Observation | Devices have become radio-silent/wedged. No exact halted PC/SP has been captured. |
| Wedge root cause | Hypothesis / refuted claim | The old MSPI/NV theory is refuted. MAC-scan, stack reconstruction, and parent-loss narratives are leads inferred from SRAM/stale values, not established causes. |
| Pi UART | Fact | GPIO14/15 use `/dev/ttyAMA0`; `/dev/serial0` points to `ttyAMA10`, the debug connector, not the GPIO header UART. |
| SWS wiring | Observation | Reset-held loopback returned `55 aa 00 ff` exactly through the shared node. It proves loop wiring/node only, not target protocol response. |
| SWS attempts | Observation | No response to no-reset SRAM read at 460800, more than 20 reset-release attempts at 460800, about 10 at 921600, or a cold-power catcher. No flash/app header was captured; no write/erase was performed in that work. |
| UART attempts | Observation | Passive 115200 saw isolated framing-error/junk, reset capture became silent, PB1/pin16 at 1,000,000 produced only `ff`, and a harmless unsupported framed command received zero bytes. No OTA START (`0x0210`) was sent and no staging erase occurred. |
| UART OTA support | Static evidence + inference | `UART_ENABLE=0` and the first preserved `0x6a0c` bootloader bytes lack raw `0x0210`/`0x0211`/response constants. This strongly supports unavailable/compiled-out UART OTA, but is not proof. |
| Wired writes | Observation | SWire reads need repeated read/merge verification because of artifacts. Tested writes were deterministically corrupt and unvalidated; no SWire write to the app is permitted. |
| Radio identity | Inconclusive observation | No actual announcements/messages were recorded after resets and counters did not change; the bench may have been out of range, so this does not identify the unit. |

## Current safe state and next step

The tested rig used 3.3 V and approximately 19 mA while running; RESETB through
a 1 kΩ resistor dropped it to approximately 11 mA. UART is wired to pins 15/16
through the documented series resistor and reset uses GPIO17 through 1 kΩ with
common ground. The final user-confirmed state had 3.3 V supply on after the
final cold cycle, with GPIO17 released/input-high and UART free. It can be
powered down while waiting, but there is no confirmation that it was powered
off.

Wait for the expected TB-03F/Telink programmer. First demonstrate repeatable
full-flash backup, readback, and recovery on an expendable unit; only then make
any write. Do not erase, start UART OTA, or retry SWire writes as a substitute.

## Superseded reports

Historical reports remain for provenance, but their scope is constrained by
the correction banners in `HANG_FINDINGS.md`, `mac_scan_wedge.md`,
`boothang_stack.md`, `fuse_no_fire_b09.md`, `idle_parent_loss.md`, and
`install_sm.md`.
