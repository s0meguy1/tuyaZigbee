# OTA speed analysis — why 201,570 B took 54 minutes

**Date:** 2026-08-15
**Device:** `0xa4c138…d282` (build 04 conversion image, served from
`<ota-server>:8093`).
**Measured result:** 201,570 bytes in 3,235 s ≈ **62 B/s**, zero aborts, lqi
255. The same bytes at the documented 50 B / 250 ms would take ~17 min, so
there is an unexplained ~3.2× slowdown.

This is Part B of the bug hunt. Nothing was flashed. The production
zigbee2mqtt host was only read. The conclusion is that **the slowdown is not
set by any z2m knob currently in effect**, and the honest next step is one
debug-log bench transfer, not a config change.

---

## 1. Measured per-block budget

| quantity | value | derivation |
|---|---|---|
| bytes served | 201,570 B | brief §3 |
| blocks | **4,200** | `ceil(201570 / 48)`; last block is 18 B |
| wall time | 3,235 s | z2m "Update … successful (3235 s)" |
| **average block period** | **770 ms** | `3235 / 4200` |
| average throughput | 62.3 B/s | `201570 / 3235` |
| server's own estimate | **1,008 s** (16.8 min) | log: "estimated at 1008 seconds (4032 chunks, 4 per second)" |
| ideal at 250 ms × 48 B | **1,050 s** (17.5 min) | `4200 × 0.25` |

So the average block took 770 ms, but the server thought it was pacing at
250 ms. The two numbers reconcile only because the transfer was **bursty, not
uniform** (§3).

---

## 2. What the server actually did

The live host runs zigbee2mqtt **2.11.0** with **zigbee-herdsman 10.1.0**.
Read-only inspection of the container found:

* `configuration.yaml` `ota:` contains **only** `disable_automatic_update_check:
  true`. There is no `image_block_response_delay`, no
  `default_maximum_data_size`, no `image_block_request_timeout`, and no
  `advanced` congestion/backoff overrides.
* The z2m schema defaults therefore apply
  (`/app/dist/util/settings.js:106-108`):
  * `image_block_request_timeout: 150000`
  * `image_block_response_delay: 250`
  * `default_maximum_data_size: 50`
* The OTA session is created with those values, with the same 250 / 50
  fallbacks, in `/app/dist/extension/otaUpdate.js:120` and `:275`. An MQTT
  payload can override them per-update at `:295-298`, but the log proves that
  did not happen here (§2.1).

### 2.1 The log line that pins it

`log.log:3468`:

```
[2026-08-15 09:25:10] zh:controller:ota: OTA update of '0xa4c138…d282'
  estimated at 1008 seconds (4032 chunks, 4 per second)
```

* "4 per second" = `1000 / 250` → `responseDelay` was **250 ms**, the default.
  Had anyone passed `image_block_response_delay: 750`, this would have said
  "1.33 per second", not "4".
* "4032 chunks" = `ceil(201570 / 50)` → `baseSize` was **50**, the default.

So the 54-minute transfer was **not** caused by a raised
`image_block_response_delay`. That knob was at its documented default for the
whole transfer.

### 2.2 Does z2m honour maxDataSize = 48? Yes.

`zigbee-herdsman/dist/controller/helpers/ota.js:277`
(`buildImageBlockPayload`):

```js
let dataSize = baseDataSize;                       // 50
...
if (Number.isFinite(requestPayload.maximumDataSize)) {
    dataSize = Math.min(dataSize, requestPayload.maximumDataSize); // min(50, 48) = 48
}
```

The device requests `maximumDataSize = 48` (firmware
`zigbee/ota/ota.c:1152-1156`, from `OTA_IMAGE_MAX_DATA_SIZE` in
`zigbee/ota/ota.h:32`). z2m therefore sends **48 bytes per block**, not less.
The 4,032-chunk estimate is slightly optimistic only because it divides by 50
instead of 48; the actual block count is 4,200.

### 2.3 Herdsman pacing / backoff

The only pacing in the OTA path is the throttle in `ota.js:437-441`:

```js
const delayNeeded = this.dataSettings.responseDelay - timeSinceLast; // 250 - elapsed
if (delayNeeded > 0) {
    await new Promise((resolve) => setTimeout(resolve, delayNeeded));
}
```

It spaces the *starts* of consecutive `imageBlockResponse` sends by
`responseDelay`, and only waits when the device's next request arrives sooner
than 250 ms after the previous response. When the round trip is longer than
250 ms, no extra wait is applied — so the effective block period is
`max(250 ms, round-trip time)`.

There is **no per-device jitter and no OTA-level backoff** in herdsman. The
only backoff-like behaviour is in the zstack adapter, outside the OTA logic:
a 2,000 ms cool-down before retrying a send that returned
`MAC_CHANNEL_ACCESS_FAILURE`, `BUFFER_FULL`, or `MAC_NO_RESOURCES`
(`zigbee-herdsman/dist/adapter/z-stack/adapter/zStackAdapter.js:427`, inside
`sendZclFrameToEndpointInternal`). There is **no evidence that fired during
the OTA window**: the first logged `MAC_CHANNEL_ACCESS_FAILURE` for this
device is at 10:24:24, after the hang (§3.2).

---

## 3. The burst pattern — why the average is 770 ms, not 250 ms

The z2m info log prints a progress line every ~30 s
(`ota.js` progress block). Consecutive percentages give the instantaneous
rate. The transfer was strongly bimodal throughout the whole 54 minutes:

| window (local) | Δt | Δ% | Δ bytes | blocks | ms/block |
|---|---|---|---|---|---|
| 09:38:46 → 09:39:17 | 31 s | +3.00 % | 6,047 | 126 | **246 ms** |
| 09:26:47 → 09:27:17 | 30 s | +0.37 % | 746 | 15.5 | **1.94 s** |
| 09:27:17 → 09:27:48 | 31 s | +0.37 % | 746 | 15.5 | **2.00 s** |
| 09:43:05 → 09:43:36 | 31 s | +0.37 % | 746 | 15.5 | **2.00 s** |
| 09:58:00 → 09:58:31 | 31 s | +0.37 % | 746 | 15.5 | **2.00 s** |

(Δ bytes = Δ% × 201,570; blocks = Δ bytes / 48.)

* The **fast windows sit on the 250 ms throttle floor** (246 ms/block ≈ 4.06
  blocks/s). That is the rate `image_block_response_delay` allows.
* The **slow windows run at ~1.9–2.0 s/block** (0.5 blocks/s), about 8× slower,
  and they recur in runs of two to five 30-second windows all through the
  transfer.
* The overall 770 ms/block is the average of these two regimes, weighted
  roughly 3:1 toward the slow windows.

The slow windows are **not** explained by `image_block_response_delay` (that
knob only floors the fast windows) and are **not** explained by any other z2m
setting. They are a round-trip / device-side / RF-path latency that the
current info-level log does not resolve.

### 3.1 Candidates for the slow windows (not yet decidable)

1. **Lost `imageBlockResponse` frames + the device's 5 s re-request.**
   A single lost response costs ~5 s (the device re-requests after
   `OTA_MAX_IMAGE_BLOCK_RSP_WAIT_TIME`, §6). A loss rate of roughly one block
   in three during a slow window would produce an average near 1.9 s/block.
   Plausible, but a 33 % loss rate is surprising at lqi 255.
2. **Device-side latency in the per-block path.** The device writes each
   48-byte block to flash (`ota_imageDataProcess` → `flash_writeWithCheck`),
   runs a CRC, and saves download progress to NV every ~39 blocks
   (`FLASH_WRITE_COUNT_GET(size) = size/5280 + 1`,
   `ota_saveUpdateInfo2NV`, `ota.c:35` and `:765-774`). The per-block flash
   write is small, and the NV save is deferred, so neither obviously accounts
   for a *sustained* 1.9 s/block run — but this is the only way to rule it out
   from the firmware side.
3. **Coordinator / channel contention.** The coordinator is a zstack adapter
   on `tcp://<internal-ip>:6638` sharing channel 25 with 45 other devices.
   The OTA is only ~2.6 frames/s average, but bursts are ~8 frames/s; shared
   airtime could inject variable latency without ever aborting the transfer.

The info log cannot distinguish these. The one fact it does establish: **no
z2m knob caused the slowdown.**

### 3.2 Why the adapter 2 s cool-down is probably innocent

The zstack `sendZclFrameToEndpointInternal` path waits 2,000 ms and retries
on `MAC_CHANNEL_ACCESS_FAILURE` / `BUFFER_FULL` / `MAC_NO_RESOURCES`
(`zStackAdapter.js:427`). If that fired repeatedly during OTA it would show
up as ~2 s slow windows. But the only logged occurrences for this device are
*after* the hang (10:24 onward), when z2m was polling a dead radio. During
09:25–10:19 there are none. The relevant retry is also debug-logged, so a
debug-level bench transfer would settle this.

---

## 4. The responsible knob, and the recommended server-side change

**Responsible knob:** `ota.image_block_response_delay`. It is **already at
its default 250 ms** and was in effect for this transfer. It sets the *fast*
floor, not the observed 770 ms average.

**Recommended server-side change (zero firmware risk):**

* **Do not raise `image_block_response_delay`.** The §7 note in
  `FIRMWARE_STATUS.md` ("raise to 500–800 ms") was aimed at avoiding bench
  stalls, but raising it *caps the fast windows* at 500–800 ms while leaving
  the slow windows unchanged — the 54-minute transfer would get *slower*, not
  faster.
* **Keep it at 250 ms for now.** Optionally, after the debug-log bench
  transfer below, try **100–150 ms** to raise the fast-window ceiling
  (schema minimum is 50 ms). Only do that if the debug log shows the round
  trip is comfortably under 100 ms; if it shows MAC-layer retries, stay at
  250 ms or higher.
* **The one action that actually resolves this:** run the next bench transfer
  with `advanced.log_level: debug`. At debug level every block request and
  response is timestamped by `zh:controller:ota` ("Request offsets…",
  "Payload offsets…", `ota.js`) and by the adapter ("sendZclFrameToEndpoint…
  Data confirm…", `zStackAdapter.js`). That turns the 30-second progress
  samples into a per-block RTT trace and directly identifies whether the slow
  windows are 5 s device re-requests, adapter cool-downs, or something else.

---

## 5. Bench stalls vs the 5 s `ota_imageBlockRspWait`

The 2026-08-14 bench unit stalled at 2.81 % and then 12.44 % with the
signature: *device stops requesting blocks, no error, healthy afterwards*.
That signature is **fully consistent with the 5 s response-wait expiring
quietly after 10 retries** and the OTA aborting without any device-side
error or reboot.

Exact chain:

1. After sending an image-block request, the device arms a 5 s one-shot timer
   (`ota.c:656`; constant `OTA_MAX_IMAGE_BLOCK_RSP_WAIT_TIME 5` in
   `ota.h:44`).
2. If no response arrives, `ota_imageBlockRspWait` runs (`ota.c:620`):
   * it increments `g_otaCtx.imageBlockRetry`;
   * while the count is below `OTA_MAX_IMAGE_BLOCK_RETRIES` (10, `ota.h:50`),
     it re-sends the same request (`ota.c:643`);
   * on the 10th timeout it sends an `UpgradeEndReq` with
     `ZCL_STA_ABORT` (`ota.c:624-631`) and schedules
     `ota_imageBlockRspTimeout` (`ota.c:640`).
3. `ota_imageBlockRspTimeout` (`ota.c:611-613`) calls
   `ota_upgradeComplete(ZCL_STA_ABORT)`.
4. `ota_upgradeComplete` (`ota.c:568-580`) sets
   `zcl_attr_imageUpgradeStatus = IMAGE_UPGRADE_STATUS_NORMAL`, frees the
   download-info buffer, and returns. It does **not** reboot, does **not**
   factory-reset, and has **no error channel on this device** — the light
   simply stops requesting blocks and continues running normally.

So a stall is: ~50 s of silent re-requests (10 × 5 s), then a clean, silent
abort. The progress percentage freezes at the last successful
`zcl_attr_fileOffset`, which is exactly the observed 2.81 % / 12.44 %
fingerprint. The device being "healthy after" is expected — the abort path
resets the OTA cluster to NORMAL. This also matches the observation that a
power cycle (LQI 167 → 255) got the bench unit 4× further: it is an RF/retry
problem, not a firmware crash.

One nuance worth recording: z2m *does* see the abort as a failed update
(herdsman only treats `UpgradeEndRequest` with `SUCCESS` as success), so
"no error" refers to the device, not the z2m log.

---

## 6. Firmware-side ideas — flag-gated future work, NOT shipped now

These stay as design notes. The OTA path is the fleet's only recovery path
(`MOES_EDITING_GUIDE.md` §6), so nothing here ships untested.

1. **Raise `OTA_IMAGE_MAX_DATA_SIZE` from 48** (`zigbee/ota/ota.h:32`).
   Highest leverage: at the same 250 ms server throttle, 96 B/block doubles
   throughput (~9 min); 100 B (z2m's cap) is only marginally better
   (~2.1× over 48 B), so 96 B is the natural target. Must be
   flagged (e.g. `MOES_OTA_FAST_BLOCK`), bench-tested on build 05/06 first,
   and checked against the Telink flash write path (`flash_writeWithCheck` is
   given `copyLen` from the block payload, so larger blocks are mechanically
   fine, but the OTA bank erase/CRC math and the ~39-block NV save cadence
   change with it).
2. **Set `zcl_attr_minBlockPeriod` to pace deliberately**
   (`zcl/ota_upgrading/zcl_ota_attr.c:57`). **Not useful for speed** — the
   device already sends immediately (`ota.c:670-676`) and the server already
   paces at 250 ms. Setting a non-zero period would only *add* device-side
   delay on top of the server throttle. It could be worth a *larger* value as
   a congestion guard if debug logs show the device over-running the
   coordinator, but that is not this incident.
3. **Watchdog CRC servicing** (`ota_newImageValid()` CRC loop,
   `zigbee/ota/ota.c:138`) is already tracked as `FALLBACK_DESIGN.md` §5.2
   and is unrelated to transfer speed.

---

## 7. Evidence locations

**Live host (read-only):**
* z2m config: `/app/data/configuration.yaml` — `ota:` has only
  `disable_automatic_update_check: true`.
* z2m settings defaults: `/app/dist/util/settings.js:106-108`.
* z2m OTA flow: `/app/dist/extension/otaUpdate.js:120`, `:275`, `:295-298`.
* herdsman OTA pacing: `/app/node_modules/zigbee-herdsman/dist/controller/helpers/ota.js:277`,
  `:377-381`, `:437-441`.
* herdsman adapter cool-down: `/app/node_modules/zigbee-herdsman/dist/adapter/z-stack/adapter/zStackAdapter.js:427`.
* transfer log: `/app/data/log/2026-08-14.18-41-07/log.log`, lines 3466–3900
  (start, estimate, progress); line 3468 is the "4 per second" proof.

**Repo:**
* `build/tl_zigbee_sdk/zigbee/ota/ota.h:32,44,50`
* `build/tl_zigbee_sdk/zigbee/ota/ota.c:35,568-580,611-647,656,670-676,765-774,1152-1156,1453-1458,1504-1528`
* `build/tl_zigbee_sdk/zigbee/zcl/ota_upgrading/zcl_ota_attr.c:57`
