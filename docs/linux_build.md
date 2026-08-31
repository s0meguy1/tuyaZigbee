# Linux Build

> **Experimental firmware — do not flash a ceiling light.** A byte-valid
> artifact and an OTA container do not prove OTA installation on silicon.

## Get Linux Toolchain and Zigbee SDK
```bash
mkdir build && cd build && cmake -P ../cmake/TelinkSDK_Linux.cmake
cd ..
```

## Configure Build
```bash
cmake . -B build
```

### Custom Toolchain and SDK
```bash
cmake . -B build -DTOOLCHAIN_PREFIX=~/opt/tc32 -DSDK_PREFIX=~/opt/tl_zigbee_sdk
```

## Perform Project Build
```bash
cmake --build build --target IASsensor.zigbee
```

## TS0505B reproducible A/B artifacts

Do not hand-edit `build/tl_zigbee_sdk/`. The extractor verifies the pinned SDK
archive and applies `sdk_patches/telink_zigbee_sdk_0d0859e2_moes.patch`; the
TS0505B configure step then rejects a missing, mixed, or drifted patch set.
For a custom SDK location, run `python3 tools/apply_sdk_patches.py --sdk-root
/path/to/tl_zigbee_sdk` first; arbitrary SDK revisions are intentionally
rejected.

Use a separate build tree for each OTA version. The default is Build 12;
`MOES_APP_BUILD_OVERRIDE` accepts decimal `0..255` without changing
`common/version.h`.

```bash
mkdir build-13 && (cd build-13 && cmake -P ../cmake/TelinkSDK_Linux.cmake)
cmake -S . -B build-13 -DDEVICE_VARIANT=TS0505B -DMOES_APP_BUILD_OVERRIDE=13
cmake --build build-13 --target light_TS0505B.zigbee light_TS0505B.tuya.zigbee -j8
```

The normal custom update is `build-13/light/light_TS0505B.zigbee`; the
stock-Tuya conversion container is `light_TS0505B.tuya.zigbee`. Both build
targets run the read-only artifact verifier. It never modifies a raw `.bin`
and never runs `tl_check_fw.py` on a `.zigbee` file.
