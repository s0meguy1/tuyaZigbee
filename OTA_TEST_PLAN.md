# Test plan — the next OTA attempt

**Target:** one healthy Moes ZB-TDD6-RCW-4 ceiling light, still on stock Tuya
firmware.
**Tools:** zigbee2mqtt and MQTT. Nothing else is needed for the ceiling phase.
**Prerequisite:** Phase 0 on a bench unit. Non-negotiable — see
`POSTMORTEM_2026-08-14.md` §5.4.

---

## The one thing to internalise before starting

**A light that boots, joins and interviews perfectly can still be dead.**

That is exactly what happened on 2026-08-14. The interview was clean, the IEEE
was preserved, `sw_build_id` read back correctly, `supports_ota` was true. The
light was resetting every 11 seconds and could never be updated again.

**The only signal that distinguishes the two is the `device_announce` cadence.**
A healthy mains-powered router announces once when it joins and then goes quiet
essentially forever. Every repeat is a reboot.

So: **the announce watcher is the test.** Everything else is setup.

---

## Abort criteria — read this before step 1

Watch `zigbee2mqtt/bridge/event` from the moment the transfer completes.

| what you see | verdict | do |
|---|---|---|
| **1 announce, then silence for 15 min** | healthy | continue to the soak, then step 8 |
| **≥ 2 announces within the first 2 minutes** | **reset loop** | **ABORT.** Do not touch another light. |
| **> 1 announce within 15 minutes** | **reset loop** | **ABORT.** Do not touch another light. |
| **2–7 announces in the first ~2 min, then silence** | **rescue mode latched** — see below | do NOT declare success; go to §"If it latched rescue mode" |
| **announces continuing past ~2 min** | reset loop that rescue mode did **not** catch | **ABORT.** This is a pre-`stack_init()` fault (`FALLBACK_DESIGN.md` §4.1) and the fixture is unrecoverable. |
| **`--fresh-join` sees no liveness within 10 min of the install** (no `device_joined`, successful `device_interview`, or `device_announce`) | did not come back | ABORT. Wait 30 min before concluding — a stalled transfer leaves the light on stock and it never rebooted at all. |

The "2–7 then silence" row is new in v1.3 and is the one most likely to be
misread. With boot probation enabled, a light with an application-level bug
**deliberately** reboots up to six times and then latches rescue mode and goes
quiet. Quiet is not the same as healthy. Distinguish them by behaviour, not by
the announce log:

* **Healthy:** responds to on/off, level and colour; the fixture visibly
  changes.
* **Rescue mode:** emits a fixed **dim cool white**; accepts ZCL commands (the
  attributes read back changed) but **the light does not move**; identify does
  not blink; `light_show` commands are rejected.

A light in rescue mode is fine — it is exactly what the mechanism is for, and
it is one OTA away from normal. It just means the image is bad.

### Run the watcher

```bash
# on the z2m host, read-only, publishes nothing
cd work/tuyaZigbee
./tools/announce_watch.py --ieee 0x<IEEE> --minutes 15

# conversion phase: a factory-new first join is silent in the announce log
# (no device_announce), so count join/interview liveness instead
./tools/announce_watch.py --ieee 0x<IEEE> --minutes 15 --fresh-join

# if mosquitto is in a container there:
./tools/announce_watch.py --ieee 0x<IEEE> --minutes 15 \
    --mqtt-cmd 'docker exec mosquitto mosquitto_sub -h localhost'
```

Exit 0 = healthy for the window, 2 = abort criteria met. Or by hand:

```bash
mosquitto_sub -t 'zigbee2mqtt/bridge/event' -v | grep --line-buffered device_announce
```

**`rollout.py` does not implement this gate.** It declares success on an
`installed_version` change, which is precisely the false positive that cost a
fixture. Run the watcher alongside it, or in place of it.

---

## Phase 0 — bench unit. Mandatory.

The bench unit exists to absorb this failure. Skipping it is what turned a
5-line bug into a ladder. If the bench transfer will not complete, **that is a
blocker, not a reason to skip ahead** — retry it (a power cycle restored the
bench light's LQI from 167 to 255 and it then got 4× further), raise
`ota.image_block_response_delay` to 500–800 ms, or move the bench unit closer
to the coordinator.

Run every step of `FALLBACK_DESIGN.md` §7.3 on the bench unit, in order.
Step 3 — **an OTA that completes while the unit is in forced rescue mode** —
is the load-bearing one. If that fails, the fallback is worthless and this plan
should stop here.

Then soak the *identical image* that will go to the ceiling, for at least
2 hours, watching announces the whole time.

Bench unit #2 is the light pulled from the ceiling on 2026-08-14. It needs a
wired flash to bring back, which is why the TB-03F matters
(`FALLBACK_DESIGN.md` §8).

---

## Phase 1 — before you touch anything

### 1. Build a clean image and check it

```bash
cd work/tuyaZigbee
cmake . -B build -DDEVICE_VARIANT=TS0505B
cmake --build build --target light_TS0505B.zigbee -j8
```

* **Zero new compiler warnings.** Two are expected and benign:
  `tuyaLight_colorLoopTimerEvtCb` / `tuyaLight_colorLoopTimerStop` "defined but
  not used" in `zcl_colorCtrlCb.c`. Anything else — especially an implicit
  declaration — is a finding, not a nuisance.
* **`APP_BUILD` bumped** in `common/version.h`, or z2m will not offer the image
  as an update to an already-converted light.
* Run the host test: `cd tools/rescue_hosttest && make check` — 22 scenarios,
  all must pass.

### 2. Verify the layout actually compiled in

Not the source — the *preprocessed value for the TS0505B target*.
`compile_commands.json` has entries for several targets; pick the one
containing `BUILD_TS0505B`.

| must be | why |
|---|---|
| `NV_BASE_ADDRESS` == `0xD8000` | anything higher puts the keypair NV module over the factory config at `0xF8000` and the per-device IEEE at `0xFB000`, erasing the light's MAC permanently on first join |
| `FLASH_ADDR_OF_OTA_IMAGE` == `0x70000` | the address the stock bootloader actually reads. `0x77000` installs once and then never again |
| NV span ends ≤ `0xF8000` | `0xD8000 + 0x1000*14 + 0x1000*4*2` = `0xEE000` ✓ |

### 3. Verify the image itself

| check | expected |
|---|---|
| `5d 02` at `+6` | present |
| `KNLT` at `+8` | present |
| `+0x18` size field | == file length |
| last 4 bytes | == `crc32(file[0:-4]) ^ 0xFFFFFFFF` (Telink's variant — plain `zlib.crc32` will **not** match) |
| file size | ≤ `0x68000` |
| `+0x0C` × 16 (RAM code) | ≤ 3584 B, the stock app's figure |
| byte before `_TZ3210_b8jdosxo` | `0x10` (16) |
| byte before `TS0505B` | `0x07` |

A wrong `manufacturerName` prefix breaks z2m's fingerprint match, the light
shows as unsupported, it loses its external converter and therefore loses OTA.
We have watched this happen.

### 4. Serve the right file

Two artefacts, and they are not interchangeable:

| image | serve to | filename |
|---|---|---|
| **conversion** | lights still on **stock** firmware | `1141-d3a3-ffffffff-*.zigbee` |
| **update** | lights **already converted** | `6464-0395-<ver>-light_TS0505B.zigbee` |

For this test the target is on stock, so it takes the **conversion** image.

Build **both**, not just the default target:

```bash
cmake --build build --target light_TS0505B.zigbee      -j8   # update image
cmake --build build --target light_TS0505B.tuya.zigbee -j8   # conversion image
```

`light_TS0505B.zigbee` alone does **not** regenerate the conversion image. That
is a live foot-gun: after this audit the update artefact was current while
`1141-d3a3-ffffffff-*.zigbee` on disk was still the pre-audit build — the one
with the key-scanner factory-reset bug (`AUDIT_FINDINGS.md` A-1). Both files
must have the same timestamp and the same byte length as the `.bin`.

> ### ⚠ Check what `rollout.py` is actually serving
>
> `rollout.py` hardcodes two URLs and neither is regenerated by the build:
>
> ```python
> CONVERSION_URL = ".../1141-d3a3-ffffffff-moes-conv-v1.zigbee"      # hand-named, stale
> UPDATE_URL     = ".../6464-0395-11023003-light_TS0505B.zigbee"     # APP_BUILD is now 04
> ```
>
> Both point at **older firmware than the tree builds**. Update them (and the
> file actually sitting in `~/tlsr/ota` on the bench Pi) before any rollout, or
> use `--url` explicitly.

```bash
# on the bench Pi
cd ~/tlsr/ota && python3 -m http.server 8093

# and confirm you are serving what you just built
sha256sum ~/tlsr/ota/*.zigbee work/tuyaZigbee/build/light/*.zigbee
```

### 5. Confirm z2m will offer it at all

```bash
mosquitto_sub -t 'zigbee2mqtt/bridge/devices' -C 1 \
  | python3 -c 'import json,sys;
[print(d["ieee_address"], d.get("friendly_name"), (d.get("definition") or {}).get("supports_ota"))
 for d in json.load(sys.stdin) if (d.get("definition") or {}).get("model")=="ZB-TDD6-RCW-4"]'
```

Every light must show `True`. That comes from
`data/external_converters/moes_ts0505b_ota.js` on the z2m host, which clones
z2m's own definition and changes only `ota: true` plus a fingerprint restricted
to `_TZ3210_b8jdosxo`. If it shows `False`, stop — z2m will refuse outright.

---

## Phase 2 — pick the target

Choose a light that is:

* **easy to reach.** Assume you will have to take it down. Pick the one over a
  landing, not the one over the stairwell.
* **on a circuit you can isolate at the breaker**, so you can cut power to it
  without cutting the other 45.
* **not load-bearing for the household** — not the only light in a windowless
  room.
* **healthy right now**: `linkquality` above ~120 and responding to on/off
  today, before you start.

Write down its IEEE, friendly name, and current `installed_version` (stock
reports **101**).

```bash
mosquitto_sub -t 'zigbee2mqtt/0x<IEEE>' -C 1
```

---

## Phase 3 — the attempt

### 6. Start the watcher first

In its own terminal, **before** starting the transfer:

```bash
./tools/announce_watch.py --ieee 0x<IEEE> --minutes 60
```

### 7. Start the transfer — one device, by URL

```bash
mosquitto_pub -t zigbee2mqtt/bridge/request/device/ota_update/update \
  -m '{"id":"0x<IEEE>","url":"http://<bench-pi>:8093/1141-d3a3-ffffffff-moes-conv-v1.zigbee"}'
```

The per-device `url` form is deliberate: it targets exactly one device and
never publishes an index that the other 45 could match. **Never** use the
fleet-wide `ota_update/check` form during a test.

Then watch progress:

```bash
mosquitto_sub -t 'zigbee2mqtt/0x<IEEE>' | python3 -c '
import json,sys
for l in sys.stdin:
    try: u=(json.loads(l).get("update") or {})
    except Exception: continue
    if u: print(u.get("state"), u.get("progress"), u.get("remaining"))'
```

Expect 15–40 minutes. **A stall is safe** — the light keeps running stock and
is healthy afterwards; this has been observed three times. If it stalls:
retry, or raise `ota.image_block_response_delay` to 500–800 ms and retry.
Nothing irreversible has happened until a complete, CRC-valid image boots.

### 8. The two minutes that matter

The moment z2m reports the transfer complete, the bootloader installs and the
light reboots. **Stop looking at progress and look at the watcher.**

Apply the table at the top of this document. Decide within 2 minutes:

* 1 announce → keep going.
* 2 or more → **ABORT.**

### ABORT procedure

1. **Cut power to that fixture at the breaker.** A light that is not powered is
   not looping and not burning flash. It also stops z2m retrying anything.
2. **Do not touch another light.** Not one. The bug is in the image, and the
   image is the same for all 46.
3. Record: how many announces, over what interval, and whether it responded to
   anything in between. That cadence is the entire diagnosis.
4. Post-mortem before the next attempt. `POSTMORTEM_2026-08-14.md` is the
   template.

### If it latched rescue mode

(2–7 announces, then silence, and the light is dim cool white and does not
respond to on/off.)

This is the fallback working. The image is bad, but the light is reachable.

1. **Do not power-cycle it.** It is stable; leave it alone.
2. Fix the bug. Bump `APP_BUILD`. Rebuild.
3. Serve the **update** image (`6464-0395-*`) — the light is converted now, so
   it queries as `0x6464`/`0x0395`, not the stock pair.
4. Push it with the same per-device command. A light in rescue mode polls for
   OTA every 10 minutes instead of every 6 hours, so it will pick it up
   promptly.
5. Confirm it comes back normal: one announce, responds to on/off, colour
   temperature works.

---

## Phase 4 — the soak, which is the actual test

### 9. Fifteen minutes of silence

The watcher must reach 15 minutes with exactly **one** announce. Do not
interact with the light during this window — no on/off, no colour, no
identify. You are measuring whether it stays up on its own.

### 10. Confirm identity survived

```bash
mosquitto_sub -t 'zigbee2mqtt/bridge/devices' -C 1 \
  | python3 -c 'import json,sys;
[print(json.dumps(d, indent=1)) for d in json.load(sys.stdin)
 if d["ieee_address"].lower()=="0x<ieee>"]'
```

* **same `ieee_address`** — if it changed, `moes_flashGetIeee()` failed and the
  light is a new device with dead history and broken automations. Recoverable
  (it still fingerprints and still gets OTA) but it needs fixing before the
  fleet.
* same `friendly_name`, `model` still `ZB-TDD6-RCW-4`
* `supports_ota: true`
* `sw_build_id` shows the new version

### 11. Function check

Only now, after the soak:

* on / off
* brightness 1 % → 100 %
* **colour temperature across the full range** — this is the one that was
  silently missing until v1.3 (`AUDIT_FINDINGS.md` A-13) and the primary
  function of the fixture
* a colour, to confirm the RGB channels
* back to a sensible white

Watch the announce log throughout. Any announce here means a command triggered
a reset — abort.

### 12. The step that actually proves it — a second OTA

**A light you cannot update again is a light you have lost, on a delay.**

Bump `APP_BUILD`, rebuild, serve the **update** image, and push it to the same
light. It must download, install, reboot and come back.

This is what proves `FLASH_ADDR_OF_OTA_IMAGE == 0x70000` on real hardware, and
it is the specific thing that made the upstream project's firmware a one-way
trip (doctor64/tuyaZigbee#23). **It has never been tested.** Until it passes,
every converted light is one bad update from a ladder.

Soak 15 minutes again afterwards.

### 13. Twenty-four hours

Leave it. Then check the announce log for the whole period:

```bash
./tools/announce_watch.py --ieee 0x<IEEE> --minutes 1440
```

Zero announces in 24 hours is the pass. One or more means something resets it
occasionally, which at ~30 minutes per OTA is still a real risk.

---

## Only then: the fleet

* One light at a time. `rollout.py --confirm-each`, paired with the watcher.
* Never the broadcast/index form.
* Stop on the first failure (`--max-failures 1`, which is the default).
* Do the easy-to-reach fixtures first, in case the answer changes at light #5.
* Rooms with a single light last.

---

## Things not to do

* **Do not skip Phase 0.** That is the whole lesson of 2026-08-14.
* **Do not declare success on `installed_version` alone.** It changed on the
  light that had to come out of the ceiling.
* **Do not interpret "it went quiet" as "it is healthy"** without checking that
  it responds to on/off. Rescue mode is quiet too.
* **Do not power-cycle a light to "fix" it.** Each cycle is another boot. On a
  healthy light it feeds the 3-power-cycle factory reset; on an unstable one it
  is just another loop iteration.
* **Do not touch a second light** until the first has passed step 12 (the
  second OTA), not step 8.
* **Do not use the fleet-wide `ota_update/check`** at any point during testing.
