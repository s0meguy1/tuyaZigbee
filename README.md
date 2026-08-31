# tuyaZigbee
[![Build](https://github.com/doctor64/tuyaZigbee/actions/workflows/build.yml/badge.svg)](https://github.com/doctor64/tuyaZigbee/actions/workflows/build.yml)

> **MOES firmware: experimental, but the conversion path now works end to end.**
> The stock-to-custom OTA conversion has been carried through on hardware,
> including recovery back to stock and reconversion. The "radio silence" that
> blocked this project turned out **not** to be a firmware fault: the conversion
> erases NV, which erases the network credentials, so the device must rejoin —
> and it looks dead until you open permit-join. See
> [docs/moes_ts0505b_conversion.md](docs/moes_ts0505b_conversion.md).
>
> This is still experimental research on two fixtures, not a release. Effects, a
> mains power cycle and a soak are unrun on the current build. Do not deploy it
> without a hardware programmer and a tested backup/restore procedure.

**ATTENTION!**
Current version of firmware have a critical bug, making impossible next updates over OTA. If you don't have hardware programmer, do not install updated firmware until bug is fixed.

## MOES experimental branch warning

The `moes-ts0505b` branch is experimental research, not a deployable firmware
release.

Build 32 is the current tip. It has been carried further on real hardware than
earlier builds, but the qualification below still stands in full.

* Build 30 completed a stock-to-custom OTA conversion on a bench fixture, which
  then joined, interviewed and rendered colour on command.
* Build 32 has been installed two ways on two fixtures: as a custom-to-custom
  update, and as a stock-to-custom conversion. Both kept their addresses,
  rejoined, interviewed, and answered device-backed reads.

That is **two fixtures over hours**, not a fleet, a duration, or a field-power
proof. Effects, a mains power cycle, and a soak remain unrun on Build 32.

Note what does **not** count as evidence here, because it misled this project
repeatedly: Zigbee command/readback success does not prove LED output. A
claimed output change needs visual confirmation on a fixture with LEDs, or
direct PWM-register evidence. Do not deploy any of this without a recoverable
hardware programmer and a tested backup/restore procedure.

**If you are converting a stock device, read
[docs/moes_ts0505b_conversion.md](docs/moes_ts0505b_conversion.md) first.** The
conversion erases NV, which erases the network credentials, so **the device must
rejoin and you have to open permit-join after the install completes**. Until you
do, it looks exactly like a bricked device: interview incomplete, every ZCL read
timing out, no useful log output. It is not bricked. That one step accounted for
more wasted debugging on this project than any actual firmware bug.

`bughunt/VERIFICATION_STATUS.md` carries this project's evidence matrix. The
copy published here is a historical snapshot and lags the branch; treat the
paragraphs above as the current statement of what is and is not proven.

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
