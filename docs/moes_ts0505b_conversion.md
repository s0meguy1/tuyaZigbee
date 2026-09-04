# Converting a MOES TS0505B from stock to this firmware, over the air

This is the procedure for a Moes RGB+CCT downlight (`TS0505B`, Tuya ZT3L module,
Telink TLSR8258) using Zigbee2MQTT. No programmer is needed for the conversion
itself — but read the warning in the README first, and do not attempt this
without a way to recover the device physically if it goes wrong.

## The one step people miss

**After the update installs, you must open permit-join so the device can rejoin.**

The conversion deliberately erases the device's NV storage, and the Zigbee
network credentials live in that NV. The light therefore comes back as an
unjoined device and has to re-associate. Until you open a join window it will
sit there announcing into nothing.

This is the single most confusing failure in the whole process, because a device
waiting to rejoin is **indistinguishable from a device that has died**:

* Zigbee2MQTT shows `interview_completed: false`
* every ZCL read fails with a timeout
* the logs show nothing wrong
* the light is on, but does not respond to any command

All of that resolves the instant a genuine join window opens. In testing, the
device announced **one second** after permit-join was opened, interviewed ten
seconds later, and was fully functional.

### Timing matters

Zigbee2MQTT **caps permit-join at 254 seconds** and rejects any longer request
outright. A conversion transfer takes roughly 35–85 minutes (four measured here: 36, 83,
84 and 84 minutes). Custom-to-custom updates are far quicker, around 25. So a window opened
when you start the transfer is always long gone by the time the device needs it.

**Open the window after the install completes, not before the transfer.**

Always read the response rather than assuming the window opened:

```
topic:   zigbee2mqtt/bridge/request/permit_join
payload: {"time": 254}
```

Check `zigbee2mqtt/bridge/response/permit_join` reports `"status":"ok"`. A
request over 254 s is rejected, and a window you believe is open but is not will
send you chasing imaginary firmware bugs.

## Procedure

1. **Back up the device first.** Take a full 1 MiB SWire dump, read twice and
   compared. This is the only thing that makes the conversion reversible.
2. **Serve the conversion container** (`light_TS0505B.tuya.zigbee`) over plain
   HTTP from a machine your Zigbee2MQTT host can reach. Verify the sha256 at
   both ends.
3. **Deliver it by explicit per-device `url`** — never through the shared OTA
   index:

   ```
   topic:   zigbee2mqtt/bridge/request/device/ota_update/update
   payload: {"id": "<ieee>", "url": "http://<host>:<port>/<file>.zigbee"}
   ```

   The conversion container masquerades as a stock Tuya image (manufacturer
   `0x1141`, image type `0xD3A3`, version `0xFFFFFFFF`). That is what makes a
   stock device accept it — and it would also match **every** stock fixture on
   your mesh, which is why it must never go in a shared index.

   Note `ota_update/check` does not honour `url` and will fail against your
   configured index. Use `update` directly.
4. **Wait for the transfer.** Progress appears in the Zigbee2MQTT log.
5. **Open permit-join** as described above.
6. **Confirm** the device rejoined and interviewed.

## What is normal, and what is not

Expected, not faults:

* **A `device_leave` right after the install.** The NV erase removed the
  credentials. If you run a spurious-leave detector, it will fire here and it is
  a false positive.
* **The light comes up amber at full brightness.** With NV erased there is
  nothing for the `TO_PREVIOUS` startup behaviours to restore, so the compiled
  defaults stand: on at level `0xFE`, colour temperature
  `COLOR_TEMPERATURE_PHYSICAL_MAX` = 454 mireds = 2200 K. Amber at full
  brightness is exactly what a freshly-erased fixture looks like.
* **The first interview may fail** with `can not get active endpoints`.
  Zigbee2MQTT normally retries successfully about a minute later.
* **An aborted transfer loses no progress** — a retry resumes near where it
  stopped.

Genuinely wrong:

* No announce within a minute of a **verified** open join window.
* The device announces and joins, but ZCL reads still fail afterwards.

## Reading back the installed version

**Zigbee2MQTT's automatic post-update interview does not refresh
`software_build_id`.** After a conversion it can still display the *previous*
build while `date_code` has correctly updated. That is a Zigbee2MQTT caching
artefact and says nothing about what is on the chip.

Force a refresh before trusting it:

```
topic:   zigbee2mqtt/bridge/request/device/interview
payload: {"id": "<ieee>"}
```

## Restoring a converted device to stock

Splice a stock image rather than writing a whole donor dump, and preserve the
target's own identity:

* write the stock app region and stock config region from a donor dump
* **erase** the NV region rather than writing the donor's
* never touch the bootloader or the identity/calibration region

**A donor dump contains the donor's MAC inside its NV region.** Writing that
region onto another board puts a duplicate MAC on your mesh. Erase it instead —
the stock application repopulates NV on first boot.

The identity and RF calibration live outside the NV region and survive an erase,
so a converted device keeps its own address.
