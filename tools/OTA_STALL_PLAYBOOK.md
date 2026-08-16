# OTA stall observability playbook

Scope: read the OTA cluster state of the bench light `0xa4c138…eccd` through
the production z2m install during the next transfer attempt, without touching the
device hardware (that is the sibling SWire workstream).

Environment (verified on `<z2m-host>`):

- z2m container: `zigbee2mqtt` (`koenkk/zigbee2mqtt:latest`)
  - zigbee2mqtt `2.11.0`
  - zigbee-herdsman `10.1.0`
  - zigbee-herdsman-converters `26.61.1`
- MQTT: `base_topic: zigbee2mqtt`, `server: mqtt://localhost:1883` inside the
  z2m container; mosquitto container `mosquitto` publishes `1883 -> 0.0.0.0:1883`,
  no auth.
- Deployed converter: `/app/data/external_converters/moes_ts0505b_ota.js`
  (identical to `converters/moes_ts0505b_ota.js` in this repo). It derives the
  built-in TS0505B_1 definition and sets `ota: true`; it adds **no** OTA
  toZigbee converter and no `convertGet`.
- Device facts from retained `zigbee2mqtt/bridge/devices`:
  - `ieee_address`: `0xa4c138…eccd`
  - `network_address`: `60389` (`0xebe5`)
  - endpoint `1`: input `genBasic`, output `genOta` / `genTime`
  - `model_id`: `TS0505B`, `manufacturer`: `_TZ3210_b8jdosxo`

## Hard rules for this playbook

- Only single-device **get/check** requests targeting `0xa4c138…eccd`.
- Never `set`, never `ota_update/update`/`schedule`/`unschedule`, never
  fleet-wide/broadcast, never modify any z2m config file or container.

## Candidate paths — what works and what does not

1. `zigbee2mqtt/<ieee>/get` with `{"update": ""}` — **does not work**.
   z2m `dist/extension/publish.js` only calls `converter.convertGet` for keys that
   have a matching `toZigbee` converter. The `update` entity is state published by
   `dist/extension/otaUpdate.js`, not a `toZigbee` converter; the deployed moes
   converter exposes no `update`/`update_state` property with get access. Result is
   only a z2m log error `No converter available for 'update'`; there is no MQTT
   response topic.

2. `zigbee2mqtt/bridge/request/device/options` — **not a read**. It writes entity
   options (and can trigger a restart). Out of scope and forbidden here.

3. `zigbee2mqtt/bridge/request/device/ota_update/check` — **works as a fallback,
   but does not return the OTA cluster attributes**. It returns
   `update_available`, `source`, `release_notes` and publishes the `update` entity
   state. It does not report `fileOffset` or `imageUpgradeStatus`. When the device
   is mid-stall / not answering it errors (observed below).

4. `zigbee2mqtt/bridge/request/action` with `{"action":"raw", ...}` — **the
   working path** for raw ZCL attribute reads. It reaches
   `zigbee-herdsman-converters` `ACTIONS.raw` -> `controller.sendRaw(...)`, and the
   response is published to `zigbee2mqtt/bridge/response/action`.

## The raw-read command (primary)

The `raw` action maps these MQTT params to `controller.sendRaw()`:

- `params.ieee_address`, `params.network_address`, `params.dst_endpoint`,
  `params.src_endpoint`, `params.cluster_key`, `params.disable_response`,
  `params.timeout`
- `params.zcl.frame_type`, `params.zcl.direction`, `params.zcl.command_key`,
  `params.zcl.payload` (and optional `manufacturer_code`, `tsn`,
  `disable_default_response`)

ZCL constants used here (from herdsman `dist/zspec/zcl/definition/enums.js`):

- `frame_type` `0` = GLOBAL, `1` = SPECIFIC
- `direction` `0` = CLIENT_TO_SERVER, `1` = SERVER_TO_CLIENT
- foundation `read` = command key `"read"` (ID `0x00`), response `readRsp` (ID `0x01`)

### 1. Liveness read: `genBasic.zclVersion` (attr `0x0000`)

`genBasic` is an input (server) cluster on endpoint 1, so read with
`direction: 0` (CLIENT_TO_SERVER).

```sh
ssh -o BatchMode=yes <z2m-host> 'docker exec mosquitto sh -lc '\''
mosquitto_sub -t "zigbee2mqtt/bridge/response/action" -C 1 -W 25 > /tmp/ota_resp.json 2>/dev/null &
S=$!
sleep 2
mosquitto_pub -t "zigbee2mqtt/bridge/request/action" -m "{\"action\":\"raw\",\"params\":{\"ieee_address\":\"0xa4c138…eccd\",\"network_address\":60389,\"dst_endpoint\":1,\"src_endpoint\":1,\"cluster_key\":\"genBasic\",\"disable_response\":false,\"timeout\":10000,\"zcl\":{\"frame_type\":0,\"direction\":0,\"command_key\":\"read\",\"payload\":[{\"attrId\":0}]}},\"transaction\":\"basic-zclversion-10\"}"
wait $S
cat /tmp/ota_resp.json
'\'''
```

Observed now (device not on-air during the sibling SWire read; this is the
liveness signal you want to see change during the next attempt):

```json
{"data":{},"error":"Data request failed with error: 'MAC_CHANNEL_ACCESS_FAILURE' (0xe1)","status":"error","transaction":"basic-zclversion-10"}
```

`MAC_NO_ACK` (`0xe9`) was also observed on other attempts; either means the
device radio is not ACKing. A successful read returns `status:"ok"` with
`data.data` bytes instead.

### 2. OTA stall state: `genOta.fileOffset` + `genOta.imageUpgradeStatus`

`genOta` is an output (client) cluster on endpoint 1; `fileOffset` (`0x0001`) and
`imageUpgradeStatus` (`0x0006`) are `client: true` attributes. Read them with
`direction: 1` (SERVER_TO_CLIENT) on `cluster_key: "genOta"`.

```sh
ssh -o BatchMode=yes <z2m-host> 'docker exec mosquitto sh -lc '\''
mosquitto_sub -t "zigbee2mqtt/bridge/response/action" -C 1 -W 25 > /tmp/ota_resp.json 2>/dev/null &
S=$!
sleep 2
mosquitto_pub -t "zigbee2mqtt/bridge/request/action" -m "{\"action\":\"raw\",\"params\":{\"ieee_address\":\"0xa4c138…eccd\",\"network_address\":60389,\"dst_endpoint\":1,\"src_endpoint\":1,\"cluster_key\":\"genOta\",\"disable_response\":false,\"timeout\":10000,\"zcl\":{\"frame_type\":0,\"direction\":1,\"command_key\":\"read\",\"payload\":[{\"attrId\":1},{\"attrId\":6}]}},\"transaction\":\"ota-fileoffset-status-2\"}"
wait $S
cat /tmp/ota_resp.json
'\'''
```

Observed now (same device-not-ACKing condition):

```json
{"data":{},"error":"Data request failed with error: 'MAC_NO_ACK' (0xe9)","status":"error","transaction":"ota-fileoffset-status-2"}
```

On success the bridge response is:

```json
{"data":{ ...sendRaw return... },"status":"ok","transaction":"ota-fileoffset-status-2"}
```

`data` contains the raw ZCL read response:

- `data.header.frameControl.manufacturerSpecific` -> header length (5 if true, else 3)
- `data.header.commandIdentifier` -> `1` (`readRsp`)
- `data.data` -> `{"type":"Buffer","data":[<bytes>]}` (full ZCL frame incl. header)

Decode `data.data` with the snippet below.

## Decoding the `readRsp` bytes

```python
import json, struct
r = json.load(open('/tmp/ota_resp.json'))
if r.get('status') != 'ok':
    raise SystemExit(r)
frame = r['data']
hdr_len = 5 if frame['header']['frameControl']['manufacturerSpecific'] else 3
buf = bytes(frame['data']['data'])[hdr_len:]
DT = {0x10:'bool',0x20:'u8',0x21:'u16',0x22:'u24',0x23:'u32',0x30:'enum8',0x42:'charstr'}
pos, out = 0, []
while pos < len(buf):
    attrId = struct.unpack_from('<H', buf, pos)[0]; pos += 2
    status = buf[pos]; pos += 1
    rec = {'attrId': attrId, 'status': status}
    if status == 0:
        dt = buf[pos]; pos += 1
        rec['dataType'] = dt
        kind = DT.get(dt, 'hex')
        if kind in ('u8','bool','enum8'):
            rec['value'] = buf[pos]; pos += 1
        elif kind == 'u16':
            rec['value'] = struct.unpack_from('<H', buf, pos)[0]; pos += 2
        elif kind == 'u24':
            rec['value'] = int.from_bytes(buf[pos:pos+3], 'little'); pos += 3
        elif kind == 'u32':
            rec['value'] = struct.unpack_from('<I', buf, pos)[0]; pos += 4
        elif kind == 'charstr':
            n = buf[pos]; pos += 1
            rec['value'] = buf[pos:pos+n].decode('utf-8','replace'); pos += n
        else:
            rec['value'] = buf[pos:].hex(); pos = len(buf)
    out.append(rec)
print(out)
```

Expected successful decode for command 2:

- `fileOffset` (`attrId 1`, `dataType 0x23` UINT32) -> current offset in bytes
- `imageUpgradeStatus` (`attrId 6`, `dataType 0x30` ENUM8) -> `0x00` normal,
  `0x01` in progress, `0x02` ready, `0x03` waiting

For command 1, `zclVersion` (`attrId 0`, UINT8) returns e.g. `3`.

The decode logic was verified locally against a synthetic `readRsp` and produced
`[{'attrId':1,'status':0,'dataType':35,'value':31488}, {'attrId':6,'status':0,'dataType':48,'value':1}]`.

## Fallback: `ota_update/check` (availability only)

```sh
ssh -o BatchMode=yes <z2m-host> 'docker exec mosquitto sh -lc '\''
mosquitto_sub -t "zigbee2mqtt/bridge/response/device/ota_update/check" -C 1 -W 30 > /tmp/ota_check_resp.json 2>/dev/null &
S=$!
sleep 1
mosquitto_pub -t "zigbee2mqtt/bridge/request/device/ota_update/check" -m "{\"id\":\"0xa4c138…eccd\",\"transaction\":\"ota-check-1\"}"
wait $S
cat /tmp/ota_check_resp.json
'\'''
```

Observed now:

```json
{"data":{},"error":"Failed to check if OTA update available for '0xa4c138…eccd' (Device didn't respond to OTA request)","status":"error","transaction":"ota-check-1"}
```

This returns `update_available`/`source`/`release_notes` when the device answers;
it does **not** expose `fileOffset` or `imageUpgradeStatus`.

## Reading the retained device facts (no publish)

```sh
ssh -o BatchMode=yes <z2m-host> \
  'docker exec mosquitto mosquitto_sub -t zigbee2mqtt/bridge/devices -C 1 -W 8' \
  | python3 -c 'import sys,json; d=json.load(sys.stdin); print(json.dumps([x for x in d if x.get("ieee_address")=="0xa4c138…eccd"], indent=2))'
```

## Capture reliability notes

- The subscribe-then-publish pattern above is verified, but use the **container
  default broker host** (do not pass `-h 127.0.0.1`; in testing, IPv4-loopback
  subscribers missed the transient `bridge/response/action` publish while the
  default host received it).
- Bridge requests are QoS 0 and can occasionally be missed by z2m under heavy
  network load. If the response file is empty, check whether the request reached
  z2m:
  `docker logs zigbee2mqtt --tail 40 2>&1 | grep bridge/response/action`.
  The z2m log is the most reliable observation point.
- The response can take up to `timeout` (10s here) plus a few seconds, hence
  `-W 25`.

## Next attempt checklist

1. Confirm `network_address` is still `60389` via the retained `bridge/devices`
   (it can change after a rejoin).
2. Run the `genBasic` liveness read (command 1). `status:"ok"` means the device
   is on-air and ACKing; `MAC_NO_ACK`/`MAC_CHANNEL_ACCESS_FAILURE` means it is
   not (off, in flash mode, or stuck with the radio not ACKing).
3. Run the `genOta` read (command 2) during the stall and decode
   `fileOffset`/`imageUpgradeStatus`.
4. If the MQTT capture races, watch `docker logs -f zigbee2mqtt` instead of the
   response topic.
