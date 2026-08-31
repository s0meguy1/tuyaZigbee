# Windows Build

> **Experimental firmware — do not flash a ceiling light.** Successful local
> builds and OTA containers are not evidence of an OTA install on silicon.

## Get Windows Toolchain and Zigbee SDK
> Ninja is required for building.
> ```powershell
> winget install Ninja-build.Ninja
> ```

```powershell
mkdir build; cd build; cmake -P ../cmake/TelinkSDK_Win.cmake
cd ..
```

## Configure Build
```powershell
cmake . -G "Ninja" -B build
```

### Custom Toolchain and SDK
```powershell
cmake . -G "Ninja" -B build -DTOOLCHAIN_PREFIX=~/opt/tc32 -DSDK_PREFIX=~/opt/tl_zigbee_sdk
```

## Perform Project Build
```powershell
cmake --build build --target IASsensor.zigbee
```

## TS0505B reproducible A/B artifacts

Do not edit `build/tl_zigbee_sdk/` by hand. The extraction script verifies the
pinned SDK archive and applies the tracked SDK patch; TS0505B configuration
fails if that exact patch set is absent or drifted.
For a custom SDK location, run `python tools/apply_sdk_patches.py --sdk-root
C:\path\to\tl_zigbee_sdk` first; arbitrary SDK revisions are intentionally
rejected.

Build each OTA version in a new directory. The default remains Build 12;
`MOES_APP_BUILD_OVERRIDE` is decimal `0..255` and leaves `common/version.h`
unchanged.

```powershell
mkdir build-13; cd build-13; cmake -P ../cmake/TelinkSDK_Win.cmake; cd ..
cmake -S . -G "Ninja" -B build-13 -DDEVICE_VARIANT=TS0505B -DMOES_APP_BUILD_OVERRIDE=13
cmake --build build-13 --target light_TS0505B.zigbee light_TS0505B.tuya.zigbee
```

The normal custom update is `build-13/light/light_TS0505B.zigbee`; the
conversion container is `light_TS0505B.tuya.zigbee`. The targets run the
read-only verifier, which never invokes `tl_check_fw.py` on a `.zigbee` file.
