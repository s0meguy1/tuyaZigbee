# Danger zones — read before changing any of these

Every item here has already cost real hardware or a fleet-wide outage. They are
listed because the code that contains them looks ordinary: none of these are
obviously risky lines, and the failure in every case was **discovered in the
field, not at the bench**.

Each site is tagged in the source with `MOES-DANGER` so you can find them:

```
grep -rn "MOES-DANGER" light/
```

**The common shape of these failures:** the device boots, the fault recurs at
the same point every boot, the exception handler resets, and you now have a
device that can only be recovered with a hardware programmer — or not at all if
it is in a ceiling. Assume anything touching boot order, NV, or the watchdog can
brick a fixture you cannot reach.

---

## 1. Ordering around `stack_init()` — `light/tuyaLight.c`

**Cost: one ceiling light.**

`nv_init()` lives inside the prebuilt stack library and runs from `zb_init()`.
Any NV access before `stack_init()` returns runs against an uninitialised NV
subsystem. `factoryRst_init()` does three NV operations and schedules a
`TL_ZB_TIMER`; calling it early crashed on **every** boot, and the registered
exception handler turned that into `SYSTEM_RESET()` — a permanent boot loop on a
device that can only be fixed over the air.

**Rule:** no NV access before `stack_init()`. If you add an init step, put it
after, and know where it sits relative to `moes_rescueBootCheck()`.

## 2. Boot watchdog interval and when it tightens — `light/app_cfg.h`

**Cost: one bench module to a corruption cascade, plus a fleet-wide reset loop.**

`MOES_BOOT_WATCHDOG_ENABLE`, `MOES_BOOT_WATCHDOG_BOOT_MS` and the settle time
control a long boot-phase watchdog that later tightens to the stock 600 ms.

* **Build 15** tightened immediately after `user_init()`. The bench then lost a
  module to a mid-sequence corruption cascade (`bughunt/build15_runaway_cascade.md`).
* **Build 17** raised the boot interval from 10 s to 30 s because a blank-NV boot
  runs channel scans and commissioning first; 10 s starved that phase on the
  field pilot and rescue-mode boots never reached their OTA query.
* A too-tight window produced a **reset loop with a 15.11 s mean period**
  (n=14), measured 2026-08-30 — and z2m and PC sampling are both **blind** to a
  reset loop. Only the `.data` boot marker sees it.

**Rule:** these are not tuning knobs. Shortening either value re-opens a
failure whose only symptom is a fixture that looks fine in Zigbee2MQTT.

## 3. NV heal, ownership marker, and erase ordering — `light/moes_nvheal.c`

**Two builds bricked in different ways before build 30 worked.**

* **Build 28** erased, then wrote its marker immediately. `nv_init()` runs inside
  `stack_init()` *before* the heal, so the NV layer's in-RAM index still
  described the erased records; the write corrupted the sector and the SDK's
  next NV write hung (12/12 PC samples in `factoryRst_powerCntSave`).
* **Build 29** erased, reset, and deferred the marker behind a `DEEP_ANA_REG`
  guard. **`DEEP_ANA_REG*` does NOT survive `SYSTEM_RESET` on this part**, so the
  guard read clear every boot and the device erased and reset **forever**.
* **Build 30 works** because `moes_nvHealPreStack()` erases *before*
  `stack_init()` using a direct flash scan (the NV API is not up yet), so
  `nv_init()` initialises against blank flash and nothing can disagree. No reset
  is needed. `moes_nvHealPostStack()` writes the marker afterwards through the
  normal API.

**Rule:** do not move the erase, the marker write, or the reset relative to
`stack_init()`. Do not reintroduce a `DEEP_ANA_REG` guard across a reset.

## 4. NV item ids — `light/moes_nvitems.h`

**Build 30 shipped a collision that corrupted the stack.**

Two records shared `NV_MODULE_APP` item `0x70` with incompatible lengths — a
4-byte owner marker and a 1-byte reset-skip flag. The SDK matches on length, so
a one-byte read consumed the four-byte payload and **overwrote three bytes of
stack**, and the reset-skip write superseded the owner record so the next boot
found no ownership and erased. Confirmed in silicon from a real capture; pinned
by `nvheal` host scenario 17.

**Rule:** every NV item id must be unique and centrally declared. Never reuse an
id for a different length.

## 5. Scene extension field layout — `light/zcl_sceneCb.c`

**Changing this silently invalidates every stored scene on every fixture.**

Store and recall must stay byte-for-byte symmetric, and the layout is persisted
on the device. A build that parses it differently from the build that wrote it
will restore garbage — which presents as lights coming on the wrong colour or
brightness, not as an error.

Build 35 changed this layout (adding colour temperature and `colorMode`), so
**every scene on every converted fixture must be re-stored after installing it.**

## 6. Output calibration — `light/tuyaLightEpCfg.c`, `light/tuyaLightCtrl.c`

Not brick-risk, but fleet-visible and easy to get wrong:

* `COLOR_TEMPERATURE_PHYSICAL_MIN/MAX` do **not** just set the endpoints.
  `temperatureToCW()` interpolates linearly across them, so narrowing the range
  rescales **every** mired value and a converted fixture can no longer match a
  stock one in the same room.
* The brightness curve must match stock's linear map into `brightmin..brightmax`
  percent. Squaring the level is 5x dimmer than stock at level 51 and only 1.3x
  at 191 — so it looks right at full brightness and plainly wrong when dimmed.

---

## 7. The OTA path — everything carrying the `OTA-CRITICAL` banner

Grep for it:

```bash
grep -rln "OTA-CRITICAL" light/ tools/
```

Today that is `light/moes_otaScheme.{c,h}`, `light/tuyaLight.c` (the
`ota_preamble_t` a coordinator matches against), `light/zb_appCb.c` (the OTA
callback table), `light/zcl_tuyaLightCb.c` (the upgrade-end abort),
`light/CMakeLists.txt` (the NV base, which places the staging bank), and
`tools/{make_ota,tl_check_fw}.py`.

**Why this one is different from the six above.** Break any of those and you
get a light that misbehaves. Break this and you get a light that will not boot
or will not rejoin — and there is no way back over the air. The fixture comes
down from the ceiling and goes on the SWire bench. That has already happened
once here: `INCIDENT_2026-08-15.md`.

**The host suites do not cover it and cannot.** They never run the bootloader,
never write flash and never perform a transfer. A green `make check` says
nothing about whether an update still installs. The only evidence that counts
is a real OTA onto the bench fixture, followed by a power cycle and a rejoin.
An image that boots once is not proof.

**Do not over-apply this.** Build 38 rewrote the entire dimming and transition
path — level, colour, output stage — and touched none of these files, so it
needed no bench time. The image grew 464 bytes and stayed at 81% of the
`0x40000` slot. Check which side of the line a change is actually on:

```bash
git diff --name-only | xargs grep -l "OTA-CRITICAL" 2>/dev/null
```

Empty output means the OTA path is untouched. That is the check to run before
concluding you are exempt, and the one to run before concluding you are not.

## The rule that underpins all of these

**A successful ZCL command or attribute readback does not prove LED output, and
does not prove the device is healthy.** This project has been misled by that
repeatedly: a fixture in a reset loop, a fixture rendering the wrong colour, and
a fixture waiting to rejoin all report normally. Confirm on hardware you can
see, or with direct PWM/register evidence.

**And: never flash a fixture you cannot physically recover without a tested
backup and a way to reach it.**
