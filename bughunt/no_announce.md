# The no-announce question

**Workstream:** no-announce (§2.3 of `AI_BUGHUNT_BRIEF.md`)
**Date:** 2026-08-15
**Branch:** `moes-ts0505b` at `315f78e`

---

## Verdict

**This SDK does not send `device_announce` on a first, factory-new join.**
It sends `device_announce` on *rejoin* and on *network-address conflict
resolution*, but a device with empty NV that performs a fresh association join
comes up joined and reachable **without ever announcing**.

This is not a hang candidate. It is a **monitoring blind spot**: the
announce-cadence abort gate (`tools/announce_watch.py` and the abort table in
`OTA_TEST_PLAN.md`) was written on the assumption "one announce = one join",
and that assumption is false for the very first boot after a conversion.

The conclusion matches the field evidence exactly:

* Build 04 did a **fresh join** (empty NV) → answered a full z2m configure
  pass at 10:21:53 → **no `device_announce` at any point**.
* The 2026-08-14 casualty **rejoined an existing network** → the stack's
  rejoin path announced → 7 announces in 4 minutes revealed the reset loop.

---

## 1. Where the announce actually lives, and what triggers it

### 1.1 The send itself is in the prebuilt library

The public API is declared in the vendored SDK and its body is inside
`libzb_router.a`:

* `build/tl_zigbee_sdk/zigbee/zbapi/zb_api.h:714`
  `u8 zb_zdoSendDevAnnance(void);`
* `build/tl_zigbee_sdk/zigbee/lib/tc32/libzb_router.a:zb_api.o`
  defines `zb_zdoSendDevAnnance` (global, linkable — `gp_proxy.c` already
  calls it).
* `build/tl_zigbee_sdk/zigbee/lib/tc32/libzb_router.a:zdp_services.o`
  defines the lower-level `zdo_device_announce_send`.

`nm -A` over the whole archive shows exactly **three call sites** of
`zdo_device_announce_send`, and **none of them is the fresh-join path**:

| object | function | when it announces |
|---|---|---|
| `zdo_nwk_manager.o` | `zdo_startup_complete` | successful start, **gated** (see 1.3) |
| `nwk_join.o` | `tl_zbNwkRejoinRespCmdHandler` | after a **rejoin response** |
| `nwk_addr_conflict.o` | `nwk_addrConflictCb` / `tl_zbNwkStatusAddrConflictInd` | after a **short-address conflict** resolution |

The application never calls `zb_zdoSendDevAnnance()`:

* `grep` over `light/` returns no matches.
* The SDK sample apps (`sampleLight`, `sampleSwitch`, `sampleContactSensor`)
  also never call it.

### 1.2 The compiled BDB source does not announce either

`bdb.c` is compiled from source into the image (listed in
`light/CMakeLists.txt`). Its only announce-adjacent action on join is a
**parent** announce, not a device announce:

* `build/tl_zigbee_sdk/zigbee/bdb/bdb.c:1101-1102`
  ```c
  /* check if sending parent announcement */
  zb_zdoSendParentAnnce();
  ```
  This is the *router* "I am a parent" announcement, a different ZDO
  cluster (`PARENT_ANNCE_CLID`, not `DEVICE_ANNCE_CLID`).

`bdb_topLevelCommissiongConfirm()` (`bdb.c:1430-1439`) — the function that
calls the app's `bdbcommissioningCb(BDB_COMMISSION_STA_SUCCESS)` — sends
**nothing**; it just invokes the application callback.

### 1.3 The fresh-join gate, decoded from `zdo_startup_complete`

`zdo_startup_complete` (in the prebuilt `zdo_nwk_manager.o`) is posted by
`zdo_nlme_join_confirm` via `zdo_startDeviceCnf` after every join, fresh or
rejoin. Its disassembly contains this logic (registers decoded against the
header layouts):

```c
/* conceptual reconstruction, not literal source */
if (startupStatus == SUCCESS) {
    g_zbNwkCtx.joined        = 1;      /* set bit 2 of ctx byte at offset 45 */
    g_zbNwkCtx.is_tc         = 0;      /* clear bit 4 */
    /* ... */
    if (!g_zbNwkCtx.is_factory_new) {        /* bit 0 of ctx[45] */
        zdo_device_announce_send(buf);
    } else if (buf->hdr.rejoinStartAgain) {  /* bit 5 of buf[195] */
        zdo_device_announce_send(buf);
    }
    /* else: skip announce */
    g_zbNwkCtx.is_factory_new = 0;           /* clear bit 0, AFTER the test */
    tl_zbNwkLinkStatusStart();
    /* ... then the app's zdpStartDevCnfCb -> bdb_zdoStartDevCnf */
}
```

The two key structures that make the byte offsets concrete:

* `build/tl_zigbee_sdk/zigbee/nwk/includes/nwk_ctx.h:103`
  `u8 is_factory_new:1;` is bit 0 of the bitfield byte at
  `g_zbNwkCtx + 45`. `is_device_factory_new()` (`nwk.o`) reads exactly
  that bit (`g_zbNwkCtx[45] << 31 >> 31`).
* `build/tl_zigbee_sdk/zigbee/common/includes/zb_buffer.h:69-80`
  `zb_buf_hdr_t` is 4 bytes; `ZB_BUF_SIZE` is 192
  (`zb_buffer.h:44-46`). The byte at `buf + 195` is therefore
  `hdr + 3`, the bitfield byte whose **bit 5 is `rejoinStartAgain`**.

So the decision is:

* **rejoin** (`is_factory_new == 0`): announce unconditionally.
* **fresh join** (`is_factory_new == 1`, `rejoinStartAgain == 0`):
  **skip announce**, then clear the factory-new flag for next time.

That is exactly the observed asymmetry. The factory-new flag is cleared
*after* the announce decision, so the very first successful join is the one
and only join that is silent.

### 1.4 Why the rejoin path is different

`tl_zbNwkRejoinRespCmdHandler` (`nwk_join.o`) sends the announce directly
after a rejoin response, independent of the `zdo_startup_complete` gate.
The 2026-08-14 casualty (which was rejoining an already-saved network every
reset) therefore produced announces during its reset loop — 7 in 4 minutes —
while build 04's fresh join produced none. (The exact announce-per-rejoin
rate is not pinned here: 7 announces over ~4 minutes of an ~11 s reset loop
is less than one per cycle, so some rejoin boots either used a secure-rejoin
path or were missed by z2m. That does not change the asymmetry: fresh join is
silent, rejoin is not.)

---

## 2. Which §2.1 evidence points this supports

This finding is about evidence point **3** ("No announce, ever, including on
the boot that joined and was configured") and does **not** attempt to explain
the hang (evidence points 1, 2, 4–7).

* **Supports §2.1(3) fully.** The no-announce is explained by the stack's own
  fresh-join gate; it is not a symptom of the hang and not a separate failure.
  A healthy fresh join would be equally silent.
* **Refutes the test plan's implicit assumption** in
  `OTA_TEST_PLAN.md:19-21` that "a healthy mains-powered router announces once
  when it joins." For the conversion OTA (first join), that assumption is
  wrong.
* **Neutral on the hang itself.** The hang class (no forward progress without
  reset) and the no-announce class are independent. The watchdog, not the
  announce gate, is the missing half for a hang.

### What the SWire SRAM dump would / would not show

The no-announce mechanism is **not** a hang candidate, so the SRAM dump is not
expected to contain a smoking gun for it. If the dump is inspected anyway:

* `g_zbNwkCtx` at offset **45** should show `joined = 1` (bit 2) and
  `is_factory_new = 0` (bit 0, already cleared by `zdo_startup_complete`).
  That confirms the device passed through the fresh-join path and cleared the
  flag — consistent with "joined but silent".
* The ZDO/NWK manager state and the announce buffers would look idle; no
  pending announce would be queued.
* Nothing in the dump would distinguish "silent because first join" from
  "silent because hung" — that is the entire point of this finding.

---

## 3. Blast radius

### 3.1 `tools/announce_watch.py` is blind during the conversion window

The watcher's only countable signal is `device_announce`
(`tools/announce_watch.py:96-97`). On a fresh-join conversion:

* a **healthy** light produces **zero** announces;
* a **hung** light (build 04) produces **zero** announces;
* a **reset loop** produces zero announces on boot 1, then one per reboot
  from boot 2 onward (because those are rejoins).

The watcher's exit logic then does the wrong thing for the first two cases:

```python
# tools/announce_watch.py:138-148
if not counts:
    print("no device_announce in ... min.")
    print("If the light was expected to (re)join in this window, that is its own "
          "problem - it did not come back. ...")
...
return 0            # <-- "healthy" verdict
```

It returns **0 (healthy)** for both "healthy but silent" and "hung and
silent". The message hints that something is wrong, but the machine-readable
exit status — which is what `OTA_TEST_PLAN.md:67` tells the operator to trust —
says pass.

This is precisely the conversion window the gate was written for, and it is
blind there.

### 3.2 `OTA_TEST_PLAN.md`'s abort table now has a false-positive row

`OTA_TEST_PLAN.md:31-38`:

| what you see | verdict |
|---|---|
| 1 announce, then silence | healthy |
| ≥2 announces in first 2 min | reset loop |
| >1 announce in 15 min | reset loop |
| 2–7 then silence | rescue latched |
| no announce within 10 min of install | did not come back → ABORT |

For the conversion OTA, the **healthy** outcome is "no announce", which the
table maps to **ABORT**. A healthy first join would therefore be cut at the
breaker for no reason. The "1 announce then silence" row can only ever be
reached on the *second* OTA (an update to an already-converted light, which
rejoins and announces).

### 3.3 Rescue-mode probation logic itself is NOT announce-based

Important correction to how §2.3 is often read: the rescue state machine
(`light/moes_rescue.c`) does **not** consume announce cadence. It counts NV
boots (`moes_rescueBootCheck()` at `light/moes_rescue.c:38-74`) and clears
after 20 joined minutes (`moes_rescueStableTimerCb` at
`light/moes_rescue.c:111-132`, gated by `zb_isDeviceJoinedNwk()`).

What *does* depend on announce cadence:

* the OTA test plan's abort gate (§3.1, §3.2);
* the field verification of a natural latch
  (`FALLBACK_DESIGN.md:445-447` — "six announce-and-reset cycles");
* the bench soak criterion (`FALLBACK_DESIGN.md:435` and
  `OTA_TEST_PLAN.md:92-94`).

A hang (no reboots) is not caught by either the announce gate or the probation
counter; it is the documented residual risk that the watchdog is meant to
close (`FALLBACK_DESIGN.md` §5).

---

## 4. Alternative gate signals (concrete, with hooks)

### 4.1 Firmware: send one announce on the *first* join only

**Where:** `light/zb_appCb.c`.

This restores the "exactly one announce per join" contract for conversions
without double-announcing on rejoins.

**Hook 1 — capture "this boot started factory-new":**

`light/zb_appCb.c:129` `zbdemo_bdbInitCb()`. The `joinedNetwork == 0`
branch (line 148) is reached exactly when `g_bdbCtx.factoryNew` was true at
`bdb_init()` (`bdb.c:1599-1636`), i.e. the device had no saved network.

```c
static bool s_firstJoin = FALSE;   /* file-scope, next to heartInterval */

void zbdemo_bdbInitCb(u8 status, u8 joinedNetwork){
    if(status == BDB_INIT_STATUS_SUCCESS){
        if(joinedNetwork){
            s_firstJoin = FALSE;   /* already on a network: rejoin path */
            ...
        }else{
            s_firstJoin = TRUE;    /* factory-new boot: first join is coming */
            ...
        }
    }
    ...
}
```

**Hook 2 — consume the flag on successful commissioning:**

`light/zb_appCb.c:186` `case BDB_COMMISSION_STA_SUCCESS:` in
`zbdemo_bdbCommissioningCb()`.

```c
case BDB_COMMISSION_STA_SUCCESS:
    heartInterval = 1000;

    if(s_firstJoin){
        s_firstJoin = FALSE;
        zb_zdoSendDevAnnance();   /* the stack skips it on first join */
    }
    ...
```

**Why it is safe and why it must be gated:**

* The SDK already exposes `zb_zdoSendDevAnnance()` for exactly this
  (`zb_api.h:714`), and `gp_proxy.c:340` calls it from application context.
* The callback runs in the main loop (posted via `TL_SCHEDULE_TASK`), the same
  context the stack uses, so the send is safe.
* It **must** be gated by `s_firstJoin`. Calling it unconditionally in
  `BDB_COMMISSION_STA_SUCCESS` would add a second announce on every rejoin
  (the stack already sends one), and `announce_watch.py` would then flag a
  healthy rejoin as a reset loop (`>= 2 in 2 min`).

**Status: proposal, not committed.** It is a deliberate on-air behaviour
addition (not a bug fix — the SDK is doing what it was designed to do), so
under the brief's "debatable changes stay as findings" rule I have left it as
an exact diff rather than a commit. It is 5 lines and low-risk, and it is the
only change that makes the existing watcher and abort table correct for
conversions.

### 4.2 z2m-side: watch `device_joined` / `device_interview` (zero firmware risk)

zigbee2mqtt publishes at least two events that fire on a fresh join even when
no `device_announce` is sent:

* `type: "device_joined"` — published when the device joins/rejoins.
* `type: "device_interview"` with `data.status` `"started"` /
  `"successful"` / `"failed"` — published around the interview.

The `device_interview` **successful** event is the strongest "responsive read"
proxy: it means z2m got node descriptor, active endpoints, and the model
fingerprint back — i.e. the device's ZDO and ZCL stacks were answering, which
is exactly what build 04 did at 10:21:53.

**Hook points in `tools/announce_watch.py`:**

* `tools/announce_watch.py:96-97` — the event-type filter. Extend it to also
  recognise `device_joined` and `device_interview`.
* `tools/announce_watch.py:105-120` — the counting/abort logic. The semantics
  need care: on a rejoin, z2m emits *both* `device_joined` and
  `device_announce`, so the watcher must not simply add `device_joined` to the
  announce count or it will false-abort healthy rejoins. The cleanest version
  is a **separate conversion mode** (`--fresh-join`) that counts
  `device_joined`/`device_interview(successful)` as the "one join" signal and
  keeps the existing announce logic for update OTAs.

**Why prefer this first:** it touches no firmware, cannot affect the fleet,
and can be deployed on the z2m host without touching the 45 healthy lights.

### 4.3 Firmware: a stronger stable-clock signal (fits rescue mode, optional)

The probation clear currently uses "joined for 20 minutes"
(`light/moes_rescue.c:111-132`). `FALLBACK_DESIGN.md:489-493` already notes
the stronger signal: "completed an OTA query round-trip". For the *abort gate*
during conversion, the analogous firmware-side signal would be an explicit
announce (4.1) or a tiny reportable attribute that flips after the first
successful configure. That is more invasive than 4.1 and is not proposed as a
near-term change.

---

## 5. Recommendation

1. **Short term, zero firmware risk:** run conversions with a watcher that
   counts `device_joined` and `device_interview(successful)`, not just
   `device_announce` (§4.2). Update the abort table in `OTA_TEST_PLAN.md` to
   distinguish the conversion OTA (expect `device_joined` + interview success,
   then silence) from the update OTA (expect one `device_announce`, then
   silence).
2. **When the next firmware image is being built anyway:** add the 5-line
   first-join announce from §4.1. It restores the original "one announce per
   join" contract and makes `announce_watch.py` correct for conversions with
   no further changes.
3. **Do not** rely on the announce cadence to catch a hang. A hang produces no
   announces and no reboots; the abort gate was designed for reset loops. The
   hang class is the watchdog's job (`FALLBACK_DESIGN.md` §5,
   `AI_BUGHUNT_BRIEF.md` §2.4/Part C).
