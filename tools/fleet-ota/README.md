# Fleet OTA tool

`moes_fleet_ota.sh` updates many MOES TS0505B fixtures that already run this
firmware to a newer build, over the air, one fixture at a time, through
zigbee2mqtt. It is the script that carried a 49-fixture house through builds
37, 40, 43 and 44, with the house-specific parts turned into arguments.

It is **custom-to-custom only**. Converting a stock fixture is a different
procedure with a rejoin and a permit-join window; see
[`docs/moes_ts0505b_conversion.md`](../../docs/moes_ts0505b_conversion.md). The
tool refuses a CONVERT image by name.

## What you need

* **zigbee2mqtt 2.x** managing the fixtures, with the coordinator you already
  use. The tool talks to it only over MQTT (`<base>/bridge/devices`,
  `<base>/bridge/request/device/ota_update/update`,
  `<base>/bridge/request/device/interview`, `<base>/<device>/get`) and reads its
  log for progress and failure lines.
* **An MQTT client and zigbee2mqtt's log, from the machine you run it on.** The
  defaults assume a docker host with containers named `mosquitto` and
  `zigbee2mqtt`:
  `docker exec mosquitto mosquitto_pub -h localhost`, the matching
  `mosquitto_sub`, and `docker logs zigbee2mqtt`. Anything else: set
  `MQTT_PUB`, `MQTT_SUB` and `Z2M_LOGS` (a command printing the log, accepting
  `--since 2m`). `Z2M_BASE` if your base topic is not `zigbee2mqtt`.
* **An HTTP server zigbee2mqtt can fetch the image from.** Put the `.zigbee`
  file in a directory and run `python3 -m http.server 8093` there, on the
  zigbee2mqtt host. With host networking the URL is
  `http://127.0.0.1:8093/<file>`; otherwise use the host's LAN address. Pass
  `--sha` with the file's sha256 and the tool checks the served bytes before it
  starts. Never put a CONVERT image in a shared OTA index: it matches every
  stock fixture on the mesh.
* `bash`, `python3`, `curl`, `flock`, `awk`, and permission to run the docker
  commands (or your own MQTT_PUB/MQTT_SUB).
* zigbee2mqtt setting `ota: disable_automatic_update_check: true` is strongly
  recommended, so nothing else starts an update while this runs.
* The **matching external converter** from the same release tag installed in
  zigbee2mqtt if you use the `report` or `turnon` gate (build 43 and later
  report `light_show_cue_slots`; the gate looks for it).
* Let zigbee2mqtt run for about three minutes after any restart before
  starting: it does not honour an explicit-url update request sooner.

## Running it

```sh
# see the queue, touch nothing
./moes_fleet_ota.sh --url http://127.0.0.1:8093/6464-0395-112c3003-light_TS0505B-b44.zigbee \
    --build v1.44s3.3 --date "20260917 06:48" --dry-run

# run it detached, log in the current directory
nohup ./moes_fleet_ota.sh --url http://127.0.0.1:8093/6464-0395-112c3003-light_TS0505B-b44.zigbee \
    --build v1.44s3.3 --date "20260917 06:48" \
    --sha 9d322d37c41f87abe3ac3335c4afbbbd681d157bffc87b1631eced502ebca762 \
    --gate turnon --exclude "0xa4c1380f1e2d3c4b" > fleet-ota.nohup 2>&1 &
```

`--build` is the `swBuildId` the image reports. `--date` is its `dateCode`, the
firmware's own build stamp; get it with `strings light_TS0505B.bin | grep -E
'^20[0-9]{6} '`, or read it from a fixture already updated. The tool counts an
install as done when **either** field says so, because zigbee2mqtt caches the
build id across a reflash and a forced interview does not always refresh it;
trusting the build id alone produced two false failures on a healthy run.

Without `--only`, the queue is every router on this firmware (build id
`v1.NNs3.3`) that is not yet on `--build`, minus `--exclude`, in the order of
their zigbee2mqtt descriptions. To control the order or the scope, give
`--only FILE`: one IEEE address per line, `#` comments allowed:

```
# porch first, then the kitchen - one IEEE address per line, top to bottom
0xa4c1380f1e2d3c4b   # porch, left
# ...the rest of the porch, then the kitchen fixtures in the order you want
```

## What it does per fixture

1. Skips it if it is already on the target build.
2. Refuses it if it is not on custom firmware at all.
3. Requests the update with the explicit URL, then watches the log every 30 s:
   progress lines are logged as they change; six minutes with no progress
   counts as a silent stall and the request is re-sent, which resumes from the
   fixture's checkpoint; a logged abort does the same; an hour without
   confirmation forces an interview.
4. Confirms the install by build id or date code, forcing an interview if
   needed (about 75 s each, up to three).
5. Three failed attempts mark the fixture FAILED and the run moves on; after
   `--max-failures` (default 3) the run stops.

After the **first** fixture it applies the gate you chose. A fixture that has
just installed an image reboots and can take a minute or more to answer again,
so the gate first waits for a plain read to succeed (up to three minutes) and
then asks for the show report up to six times; a one-shot check here once
stopped a healthy run. `report` checks that
the fixture's show report carries `light_show_cue_slots` (firmware and
converter agree). `turnon` additionally fades the fixture off, sends the raw
with-on-off "level 20 over 2 s" turn-on and requires it to read back ON, which
is the build 44 fix; on build 43 that command leaves the light dark. Either
failure stops the run before it touches the rest.

Expect 20 to 40 minutes per fixture. zigbee2mqtt's `image_block_response_delay`
is not worth lowering: measured, a smaller delay was six times slower, because
the fixture's flash writes set the pace.

## Rules learned the hard way

* **One OTA at a time.** Two concurrent transfers blew the coordinator's serial
  deadline and took zigbee2mqtt down mid-transfer. The tool holds a lock so it
  cannot run twice; do not start anything else alongside it.
* A zigbee2mqtt restart mid-run is survivable: the transfer aborts, the tool
  notices and re-sends, and the fixture resumes from its offset. Retry with the
  byte-identical image, or it starts from zero.
* An image can never be installed over the same file version: the fixture
  returns success without downloading and looks like a device that went quiet.
* A successful command or attribute readback proves the radio, not the light.
  If a fixture matters, look at it.
* Nothing here has automatic recovery for a hung coordinator. If your setup has
  a watchdog that restarts zigbee2mqtt, know that it will interrupt transfers.

Log lines: `--- 0x... : v1.43s3.3 -> v1.44s3.3 (attempt 1) ---` starts a
fixture, `"progress":NN` every 30 s while it advances, `... OK` when confirmed,
`GATE ... OK/FAILED` after the first, `=== RUN FINISHED ===` then one line per
fixture with what it reports now.
