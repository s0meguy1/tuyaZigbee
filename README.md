# tuyaZigbee
[![Build](https://github.com/doctor64/tuyaZigbee/actions/workflows/build.yml/badge.svg)](https://github.com/doctor64/tuyaZigbee/actions/workflows/build.yml)

> **MOES TS0505B: experimental custom firmware, and the conversion path works
> end to end.** A stock fixture can be converted over the air, keeps its Zigbee
> address and its Home Assistant entities, and can be converted back. What it
> adds over stock is described below. Read
> [DANGER_ZONES.md](DANGER_ZONES.md) before changing it and
> [docs/moes_ts0505b_conversion.md](docs/moes_ts0505b_conversion.md) before
> installing it.

**ATTENTION!**
Current version of firmware have a critical bug, making impossible next updates over OTA. If you don't have hardware programmer, do not install updated firmware until bug is fixed.

> The upstream warning immediately above concerns where an update is staged in
> flash. **This fork fixes that specific bug** for the MOES TS0505B target, by
> placing the staging bank where the stock Tuya bootloader actually looks (see
> "OTA that keeps working" below). It still stands for upstream builds.

## MOES TS0505B custom firmware

Replacement firmware for the Moes ZB-TDD6-RCW-4 RGB+CCT downlight
(`TS0505B` / `_TZ3210_b8jdosxo`, Tuya ZT3L module, Telink TLSR8258). It is
installed over the air, keeps the stock Tuya bootloader, and preserves the
fixture's identity so zigbee2mqtt and Home Assistant see the same device
afterwards.

### The on-device light-show engine

The headline feature, and the reason the rest of this exists. Fifteen effects
render **on the chip at 50 fps**, driven through the Tuya manufacturer cluster
(`0xEF00`) from a small zigbee2mqtt external converter in
[`converters/`](converters/). A whole show costs a couple of Zigbee frames
instead of a stream of them, and one group broadcast runs the whole room.

Effects: `rainbow` `pulse` `candle` `twinkle` `fire` `strobe` `wave`
`lightning` `chase` `color_step` `snow` `burst` `explode` `solid`, plus `stop`
to hand the output back to normal control.

What you can control, all of it readable back from the device:

| control | what it does |
|---|---|
| `light_show` | which effect owns the output |
| `light_show_speed` | rate, applied live without restarting the effect |
| `light_show_hue` / `_saturation` | show colour, independent of the fixture's own colour |
| `light_show_level` | show brightness, independent of the fixture's own level |
| `light_show_phase` | this fixture's offset into the effect's period |
| `light_show_index` / `_spread` | fixture position in a room, and degrees of phase per position |
| `light_show_takeover` | whether an ordinary Zigbee command stops the show |
| `light_show_duration` | run for N milliseconds, then stop |
| `light_show_density` | how much of a `burst` is sparks |
| `light_show_cue` | several of the above plus a delay, in one atomic frame |
| `light_show_cue_list` / `_cue_run` | up to 32 timed steps, uploaded and then played by the chip |

Three of those are worth calling out:

* **A cue list moves choreography off the network.** Upload up to 32 timed
  steps, trigger them with one frame, and the chip plays the sequence itself.
  A thirty-second show stops being dozens of frames fighting for airtime.
* **Deferred execution makes group sync tight.** A frame carrying a delay is
  applied by every member at the same moment after receipt, rather than
  whenever each frame happens to arrive.
* **Index and spread turn a broadcast into a chase.** Give each fixture a
  position once, then a single group command runs a wave across the room in
  order.

Show state lives in the engine, not in the light's own attributes, so a colour
or brightness meant for a show never disturbs a fixture that is simply on, can
be staged while the fixture is dark, and is fully restored when the show stops.
Full reference: [docs/light_show.md](docs/light_show.md).

### Colour and output, fixed

* **Real RGB+CCT.** Both colour modes exist at once and the ZCL colour mode
  decides which drives the output. Upstream's control layer supported one or
  the other, which left the white channels unreachable on this board.
* **XY colour commands actually render.** They previously moved attributes and
  nothing else.
* **The full colour-temperature range**, with a linear map across it, so a
  converted fixture matches a stock one in the same room.
* **Stock's brightness curve.** Squaring the level, which looked right at full
  brightness, was five times too dim at the bottom of the dial where a
  downlight actually lives.
* **The bottom of the dimming range works.** Levels 1 to 9 no longer all render
  identically.
* **Scenes carry colour temperature.** They previously stored stale hue and
  saturation, so a recall came back the wrong colour.
* **`onWithTimedOff` works**, which stock ignored entirely.

### Reliability, and getting out of trouble

Every item here exists because something failed in a ceiling, where the only
way in is over the air.

* **Rescue mode.** Consecutive failed boots are counted; a fixture that keeps
  failing comes up doing nothing but joining the network and serving the OTA
  cluster, so a bad application build is recoverable without a ladder.
* **A boot watchdog** sized for the long unjoined boot phases, tightening to
  the stock interval only once the fixture is up and settled.
* **NV self-heal.** Inherited stock NV wedges the stack; it is classified and
  erased before the stack ever reads it, and genuine custom state is preserved
  across updates instead.
* **The reporting table is sanitized at boot**, so an update that removes an
  attribute cannot turn a restored reporting entry into a permanent boot loop.
* **A boot-reason breadcrumb** readable over the air, so a silent reset can be
  told apart from a power cut.
* **The key scanner is compiled out.** This board has no buttons, and scanning
  a floating pin factory-reset the light every few seconds.

### OTA that keeps working, and identity that survives

* **The OTA staging bank is placed where the stock bootloader looks.** Getting
  this wrong installs once and then never again, which is the upstream bug the
  warning at the top of this file describes.
* **NV is relocated** so it can never overwrite the factory blocks holding the
  board configuration and the fixture's unique address.
* **The address is read from the Tuya factory block**, so a converted fixture
  keeps its identity, its zigbee2mqtt entry, its friendly name and its Home
  Assistant history.
* **Aborted transfers resume** from their checkpoint rather than restarting.
* SDK-side fixes for the install commit path and an unbounded flash busy-wait
  travel with the tree as a verified patch set, not as hand edits.

### Status, honestly

Build 36 is the current tip.

* Builds through 35 have run on a fleet of nineteen fixtures across three
  rooms, including conversions from stock, mains power cycles and day-scale
  operation.
* The light-show engine has been exercised on hardware and measured with a
  mains power meter; those measurements drove the build 36 rewrite.
* Build 36 itself is newer than that: it has been installed on a bench fixture,
  booted, and passed a scripted acceptance pass over the air covering the cue
  list, deferred frames, state readback, the takeover policy and persistence
  across a power cycle. It has not yet had a long soak.

What does **not** count as evidence here, because it misled this project
repeatedly: a successful Zigbee command or attribute readback does not prove
LED output, and it does not prove the device is healthy. A fixture in a reset
loop reports normally. Confirm on hardware you can see, or with direct PWM
evidence.

Do not deploy any of this without a hardware programmer you can reach the
device with, and a tested backup and restore procedure.

`bughunt/VERIFICATION_STATUS.md` carries the evidence matrix. The copy
published here is a historical snapshot and lags the branch; treat this section
as the current statement of what is and is not proven.

(Historical notes in `bughunt/` and the `*_FINDINGS.md` files refer to a
`moes-ts0505b` branch. That was this work's branch name before it moved to
`main`; those documents are dated records and were left as written.)

This project intended to replace firmwares in TuYa devices based on Telink TLSR82XX chips
## Supported devices
Currently, implemented main code for following classes of devices:
* Lights amd LED controllers
* Switches and buttons (not include relays and switches with relays)
* Intruder Alarm Systems (IAS) sensors
* More to come!

After main code implemented, implementing new devices of same class usually much easier - only implementation specific hardware detail need to be added.  
List of currently implemented devices [Supported](docs/devices.md)  
List of work in progress devices [WIP](docs/devices-wip.md)

## Is my device supported?
Theoretically speaking, any device based on Telink zigbee chips can be supported. Practically, I worked only on TLSR8258 based devices.
You can check your device by opening it and look on board. If you see chip named TLSR8258 or tuya modules named ZT3L, ZTLC5, ZTU-IPEX, ZTU, ZT5, ZT2S, ZTC - this device theoretically can be supported.
Or look at device in zigbee2mqtt interface. If device IEEE (MAC) address begins with 0xa4c138 - this is Telink based device.

Unfortunately, for some devices replacement firmware can't be implemented. Mostly because some complex devices like thermostats, roller shaders openers, etc have two MCU. Telink chip used only for zigbee network operations, and some other MCU do real operations with hardware. This architecture used by many Tuya devices and allows manufacturer to put any Zigbee module or even WiFi/Bluetooth without changing hardware-related code.

List of unsupported devices [Unsupported](docs/devices-not-supported.md)

## Why?
Because TuYa firmware sucks, that's why :)  
But, seriously, not counting common drawbacks of TuYa devices firmware like manufacturer dependent messages and overall strange approach to zigbee standart, TS050X devices have very annoying bug: sometimes, then device receive On command, it turn on light for few seconds and turned off. I was unable to find exact condition to trigger this bug, but it happens from time to time. TS0041 does not support binding, etc.  
So, this project was born.  
This project partially based on samples from TeLink zigbee SDK.
This is continuation of earlier project [TuyaLight](https://github.com/doctor64/tuyaLight)

# Compilation
+ For Windows see: [docs/windows_build.md](docs/windows_build.md)
+ For Linux see: [docs/linux_build.md](docs/linux_build.md)

# Flashing
See [docs/flash.md](docs/flash.md)

# OTA update of Tuya firmware
See [docs/ota_tuya.md](docs/ota_tuya.md)

# OTA
See [docs/ota.md](docs/ota.md)

# Support & help
If you need assistance you can check [opened issues](https://github.com/doctor64/tuyaZigbee/issues). Feel free to help with Pull Requests when you were able to fix things or add new devices or just share the love on social media.

Check Diskord channels 
+ [#general](https://discord.gg/xSRjUS7Vpy)
+ [#development](https://discord.gg/GThy6Ednx7)
