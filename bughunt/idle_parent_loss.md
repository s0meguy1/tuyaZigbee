# idle_parent_loss — why build 06 declares its parent lost ~70–95 s after the last inbound traffic

> ## Superseded certainty notice (2026-08-17)
> Parent loss remains a theory. The observed wedge has no exact halted PC/SP,
> and this analysis does not prove that its inferred neighbor-aging sequence
> reaches the claimed rejoin/scan transition. Use it as a lead only.

**Branch:** `moes-ts0505b`. Analysis is against the build-07 ELF `build/light/light_TS0505B`
(symbolized, not stripped; the prebuilt `libzb_router.a` nwk code is read via `tc32-elf-objdump`),
and the build-06 SRAM captures `dump/bench_2026-08-15/hang_capture/{sram_1.bin,sram_2.bin}`.
Read-only analysis; nothing committed to firmware.

## Verdict up front

The bench light is a **router** (`light/CMakeLists.txt` `-DROUTER=1`; `light/stack_cfg.h` →
`ZB_ROUTER_ROLE 1`). As a router it does **not** poll; it maintains its parent through the NWK
link-status mechanism. The disassembly shows that mechanism has two independent halves that point
in opposite directions:

- The device **transmits** link status every `NIB.linkStatusPeriod` = **15 s** to NWK broadcast
  `0xfffc` (all routers + coordinator). This TX half is alive and healthy when idle.
- On the **same 15 s tick** it runs neighbor **aging**: every non-end-device neighbor (including
  the coordinator parent) gets `age++`, and at `age >= NIB.routerAgeLimit` (default **3**) the entry's
  `age/transFailure/lqi/outgoingCost` are zeroed. Only **inbound link-status** from the parent resets
  that `age`; inbound *application data* (OTA image blocks, ZCL responses) does **not** reset `age`,
  it only refreshes the entry's `lqi`.

So the parent-lost window is fundamentally **RX-driven, not join-driven**: a router whose parent's
link-status is not being delivered/processed ages the parent out after ~3 × 15 s = **45 s nominal**,
plus the rejoin-scan/backoff startup, which lands in the observed 70–95 s window. Continuous inbound
traffic during the 19.5-minute OTA masked this because inbound frames keep the parent entry's `lqi`
(and therefore its "still a usable neighbor" status) refreshed, while the device's **own** periodic
link-status cannot — it is outbound.

The exact final step that converts "parent entry aged out" into `ZDO_NETWORK_LOST` (0x60) →
`BDB_COMMISSION_STA_PARENT_LOST` → rejoin scan is **INFERRED** (the direct trigger in the prebuilt
ZDO/nwk path was not localized register-by-register). Everything in Section 1 up to that step is
verified from the ELF and the runtime SRAM NIB.

---

## 1. Mechanism — link-status TX, neighbor aging, and the RX-only parent refresh

All function addresses below are build-07 ELF addresses. The runtime SRAM addresses in Section 2 are
build-06; the two builds differ by exactly +8 bytes in the relevant BSS region (see Section 2).

### 1.1 What the device transmits when idle (routers send link-status every ~15 s — confirmed)

`nwk_linkStPeriodic` (`0x22d58`) is registered in the SDK periodic-task table (rodata word
`0x00022d59` found in the task table). It is a countdown:

```
22d58: r3 = 0x8426f0            ; link-status countdown cell
22d5c: r2 = [r3] ; if 0 -> return
22d62: r2-- ; [r3] = r2
22d6a: if r2 != 0 -> return
22d70: delay = (drv_u32Rand() & 0x7f) + 5
22d7c: ev_timer_taskPost(0x22d21 /* tl_zbNwkLinkStatusTimerEvtCb+1 */, 0, delay)
22d86: [0x8426f4] = timer-id
```

`tl_zbNwkLinkStatusStart` (`0x22d00`) is what reloads that countdown and sends the frame:

```
22d02: tl_zbNwkSendLinkStatus()
22d0a: r3 = [g_zbInfo + 80]     ; NIB.linkStatusPeriod
22d0c: if r3 == 0 return
22d12: [0x8426f0] = r3          ; reload countdown = 15
```

`tl_zbNwkSendLinkStatus` (`0x22af8`) builds the link-status list (`nwk_linkStEntryBuild`, `0x228c4`,
which **skips neighbours whose `lqi` is 0**) and sends it via `nwkLinkStatusCmdSend` (`0x22a80`) →
`nwk_fwdPacket`. The NWK destination written at `0x22b74` is `0xfffc` (broadcast to routers +
coordinator). So **yes, this stack sends link-status every ~15 s when joined and idle**; it is not a
silent radio.

### 1.2 The aging half runs on the same tick and counts *missed RX*, not TX

`tl_zbNwkLinkStatusTimerEvtCb` (`0x22d20`):

```
22d26: r3 = [g_zbNwkCtx + 45]   ; nwk_ctx bitfield #1
22d28: r2 = r3 << 29 ; tjpl ...  ; test bit2 = joined
                                  ; if joined == 0 -> skip aging, clear timer, return -1
22d36: [T_DBG_linkStatus]++      ; 0x8426f1
22d38: tl_zbNwkNeighborTabAging()
22d3c: tl_zbNwkLinkStatusStart()
```

So when joined, every 15 s the device does **both** "age all router neighbours" **and** "send its own
link status". The key asymmetry: sending its own link status does nothing to the parent's `age`
field, because that field is only written by the **RX** path.

`tl_zbNwkNeighborTabAging` (`0x22820`) — verified field-by-field:

```
2284c: entry = tl_zbNeighborEntryGetFromIdx(i)
22854: r3 = [entry+30] & 0x0e   ; deviceType bits (1..3)
2285a: if r3 == 4 -> skip        ; end-device child -> not aged here
2285e: r3 = [g_zbInfo + 115]     ; NIB.addrAlloc
22860: if r3 == 0 -> skip        ; non-router does not age router neighbours
22864: r3 = [entry+31] ; r3++ ; [entry+31] = r3      ; age++
22870: r2 = [g_zbInfo + 87]      ; NIB.routerAgeLimit
22872: if r2 > age -> skip       ; not expired yet
22876: ... store 0 into [entry+31] (age), [entry+33] (transFailure),
      [entry+34] (lqi), [entry+35] (outgoingCost)
22886: ++u16 @[g_sysDiags + 54]  ; 16-bit expired-neighbour diagnostic
```

`tl_zb_normal_neighbor_entry_t` is 36 bytes under `ZB_SECURITY` (offsets: `used/deviceType/relationship`
bitfield = 30, `age` = 31, `depth` = 32, `transFailure` = 33, `lqi` = 34, `outgoingCost` = 35). The
defaults come from `nwkNibDefault` const (`0x38a18`): byte +4 = `linkStatusPeriod = 15`, byte +11 =
`routerAgeLimit = 3`. Both are confirmed at runtime in SRAM (Section 2).

**Net:** the parent entry is aged out after **3 × 15 s = 45 s nominal** (plus the 5–132 ms jitter)
of not receiving link-status from the parent. That is the "missed N link-status periods" logic: `N = 3`.

### 1.3 What resets the parent — only inbound link-status, and only lqi for inbound data

`tl_zbNwkLinkStatusCmdHandler` (`0x22dbc`) — inbound NWK link-status:

```
22dd0: length check: payload_len == (entryCnt * 3 + 2), else drop
22e0c: entry = nwk_neTblGetByShortAddr(senderShortAddr)
22e1a: [entry+31] = 0            ; AGE RESET on inbound link-status
22e28: ... update incomingCost bits in [entry+30]
```

`tl_zbMacMcpsDataIndicationHandler` (`0x25388`) — inbound application **data** (this is the path OTA
image blocks and ZCL responses take):

```
25564: entry = nwk_neTblGetByShortAddr(senderShortAddr)
25570: r0 = [entry+34]           ; current lqi
25574: tl_nwkGetAverageLqi()     ; exponential-moving-average
2557a: [entry+34] = r0           ; LQI updated — [entry+31] (age) is NOT touched
```

This is the crux of the hypothesis. Inbound **data** refreshes the parent's `lqi` but **not** its
`age`; inbound **link-status** resets `age` but is a separate frame type. The device's own periodic
frames are outbound and therefore reset nothing on the parent entry. So the only thing that holds the
parent entry open while idle is the parent's *own* link-status stream — and on the bench that stream
is evidently not being delivered or not being matched (see Section 3).

### 1.4 Parent-lost → rejoin (verified ends, inferred begins)

- `zdo_startDeviceCnf` (`0x30cd4`) posts `zdo_startup_complete`; `bdb_zdoStartDevCnf` (`bdb.c:1260`)
  maps `status == ZDO_NETWORK_LOST (0x60)` to `BDB_COMMISSION_STA_PARENT_LOST`.
- `zdo_nlme_join_confirm` (`0x31630`, `0x31678`) loads `0x60` into the start-device status when a
  rejoin/join confirm arrives non-SUCCESS — this is the rejoin-failure side, not the idle side.
- The rejoin implementation is `nwk_rejoinReq` (`0x26da8`); `nwk_rejoinScanCnfHandler` (`0x27220`)
  re-enters `nwk_rejoinReq` on a failed scan (the retry loop `mac_scan_wedge.md` already traced).
- `zdo_auth_check_timer_cb` (`0x30ea4`) calls `tl_zbNeighborTableInit` before starting the device —
  this is why the neighbour table is empty in the wedged captures (Section 2).

What I did **not** fully pin down is the exact idle-time trigger that takes "parent entry's
`lqi/outgoingCost/transFailure` just got zeroed" and turns it into a `ZDO_NETWORK_LOST` indication.
The strongest INFERRED chain, consistent with both the disassembly and the OTA observation, is:

> parent link-status not received → `age` reaches 3 → aging zeroes the parent's `lqi/outgoingCost` →
> the parent no longer looks like a usable router neighbour (`nwk_linkStEntryBuild` already skips
> `lqi==0` neighbours when the device builds its own link-status) → the nwk/ZDO layer declares the
> parent lost and issues a rejoin scan.

Under that chain, **inbound application traffic suppresses the wedge** because it keeps the parent's
`lqi` (and thus "usable neighbour" status) refreshed, even though it does not reset `age`. That
reconciles the 19.5-minute OTA run with the "~70–95 s after last inbound traffic" window. The
alternative — that the decision keys strictly on `age` — would predict a mid-OTA wedge and is
therefore contradicted by the OTA evidence; I mark that as **REFUTED by observation**, not by code.

---

## 2. Capture evidence — NIB and neighbour table in the build-06 SRAM

The captures are build-06, while the ELF at hand is build-07. The relevant BSS objects moved by
exactly **+8 bytes** from build 06 to 07 (the app added `moes_liveness.c`). Build-06 `g_zbInfo` is
pinned by `mac_scan_wedge.md`'s `macMaxCSMABackoffs` anchor (`0x847118+0x3e = 0x847156`, value `0x04`,
re-verified below), so:

| object | build-06 SRAM addr | build-07 ELF addr |
|---|---|---|
| `g_zbInfo` (macPib/NIB/bdb) | `0x847118` | `0x847120` |
| `g_zbNIB` (= +0x4c) | `0x847164` | `0x84716c` |
| `g_zbNwkCtx` | `0x847498` | `0x8474a0` |
| `g_zb_neighborTbl` | `0x842fa4` (derived, verified below) | `0x842fac` |
| `g_sysDiags` | `0x843600` | `0x843608` |
| `zdo_cfg_attributes` | `0x8476d8` | `0x8476e0` |

Verification that the SRAM is build-06 and the mapping is right (all match `mac_scan_wedge.md`):

```
macMaxCSMABackoffs @0x847156 = 0x04   (expect 4)
scan channel index  @0x8421e4 = 0x18   (expect channel 24)
scan timer task id  @0x847478 = 0xc46e74
radio state         @0x842550 = 0x01   (RX)
s_failCnt           @0x842471 = 0x01
s_rescue            @0x842478 = 0x00
```

### 2.1 Runtime NIB — defaults confirmed, device is unjoined at capture

`g_zbNIB @0x847164` (identical in `sram_1.bin` and `sram_2.bin`):

| field | value | meaning |
|---|---|---|
| `linkStatusPeriod` (+4) | **15** | link-status period, s |
| `routerAgeLimit` (+11) | **3** | router-neighbour aging limit |
| `panId` (+24) | `0xffff` | unjoined default |
| `nwkAddr` (+26) | `0xfffe` | unjoined default |
| `addrAlloc` (+39) | 2 | nonzero → aging enabled |
| `depth` (+37) | 1 | |

The unjoined `panId/nwkAddr` is exactly what a device looks like **after** it has declared parent-lost
and entered the rejoin scan — it is not a healthy joined router.

`g_zbNwkCtx @0x847498`: byte +45 = `0x01` → `joined` (bit 2) = **0**, consistent with the wedge.

`zdo_cfg_attributes @0x8476d8`: `config_parent_link_retry_threshold` (+8) = **5**,
`config_rejoin_times` = 5, `config_rejoin_duration` = 6 s, `config_rejoin_backoff_time` = 30 s,
`config_max_rejoin_backoff_time` = 90 s, `config_rejoin_backoff_iteration` = 8. These backoff numbers
are part of why the death window stretches beyond the bare 45 s aging point.

### 2.2 Neighbour table — empty active list at capture (parent already gone)

`g_zb_neighborTbl @0x842fa4` (header at `+168..+179`):

```
freeHead          = 0xc43058  -> SRAM 0x843058  (first array slot; runtime ptrs are 0xc4xxxxxx-based)
activeHead        = 0          (NO active neighbours)
additionNeighborNum = 0
normalNeighborNum   = 0
childrenNum         = 0
```

The `neighborTbl[26]` array starts at `0x843058` (stride 36 bytes). Every entry reads `used = 0`,
`relationship = 3` (NEIGHBOR_IS_NONE_OF_ABOVE), `age/lqi/transFailure/outgoingCost = 0` — i.e. the
whole table is on the free list. **The parent entry is gone by capture time**, which is the expected
aftermath of parent-lost + `zdo_auth_check_timer_cb` re-initialising the table during the rejoin scan.

The aging-expired diagnostic counter (`g_sysDiags + 54 = 0x843636`) reads **0**. This is ambiguous and
I will not over-read it: either the parent was removed through a path other than the aging expiry, or
the counter was reset by the same rejoin re-initialisation. **Not** treated as evidence either way.

**SRAM bottom line:** the captures show the *aftermath* (unjoined NIB, empty neighbour table) and
confirm the runtime `linkStatusPeriod=15` / `routerAgeLimit=3` constants, but they do not contain the
parent entry's `age/failure` fields at the *moment* of parent-lost — that moment predates both
captures. Do not treat the captures as a live snapshot of the trigger.

---

## 3. Needed observations (z2m-side — do NOT run these yourself; another agent owns the host)

These would decide the open questions; none require touching the device.

1. **Does the coordinator send NWK link-status at all, and at what cadence?** Capture on the
   coordinator's channel for ≥3 min with no application traffic and look for NWK command `0x08`
   (link-status) from the coordinator's short addr. If the coordinator never sends it, the parent's
   `age` can never be reset when idle and the wedge is fully explained. If it *does* send it every
   ~15 s, then the defect is on the device's RX/match side and needs the length/entry-count check in
   `tl_zbNwkLinkStatusCmdHandler` (entryCnt×3+2) examined against the coordinator's actual payload.
2. **Timeline around the death window** for the bench unit (`<redacted-device>`): last
   coordinator→device frame time, then the first rejoin/beacon-request burst. Confirm the "~70–95 s
   after last inbound" figure precisely and correlate it with 3×15 s + rejoin backoff.
3. **What the last inbound frames were** before silence (link-status vs application data). If the
   only inbound frames that reset the death clock are application frames (OTA/ZCL), that confirms the
   data-refreshes-lqi path; if link-status is also present and still the device dies, the RX/match
   defect is the lead.
4. **Other deployed mains fixtures**: do they also go quiet ~70–95 s after idle, or only the bench unit?
   Fleet-wide incidence tells us whether this is a coordinator behaviour (all routers affected) or a
   per-device RX quirk.

---

## 4. Recommendation — build-08 keepalive, ranked by risk

Goal: force periodic **inbound** traffic from the parent so the parent entry's `lqi` (and whatever
"usable neighbour" state the parent-lost decision reads) never decays while idle. The device already
transmits every 15 s, so "send something periodically" is *not* the fix; the fix must elicit a
**response**.

### 4.1 Ranked candidates

**1. Periodic ZCL read of the coordinator — RECOMMENDED (low risk).**
Read a cheap coordinator attribute (e.g. Basic `ZCLVersion`, or Time `Time`) unicast to short addr
`0x0000` every 30–60 s. The coordinator's ZCL response is application data, which takes exactly the
`tl_zbMacMcpsDataIndicationHandler` → `nwk_neTblGetByShortAddr` → LQI-refresh path verified in §1.3.
- **Anti-wedge effect:** keeps the parent entry's `lqi` alive → parent never looks stale → no
  parent-lost → the rejoin-scan wedge class never starts. Expected effect: high.
- **Airtime:** one unicast read + MAC ACK + response + MAC ACK ≈ 4 frames per keepalive. At a
  60 s cadence, assess aggregate fleet airtime against the actual deployment; it is roughly in
  addition to each device's existing 15-s link-status broadcast background.
- **Interaction with `moes_liveness.c`:** a ZCL read goes through APS/ZCL, **not** BDB, so it does
  **not** call `moes_livenessStackActivity()` and cannot directly hold the 60 s fuse open. While the
  keepalive works, the device stays `joined`, so `moes_livenessSampleCb` keeps `s_silence = 0` and the
  fuse simply never arms. If the stack *does* wedge (rejoin scan freeze → `joined = 0`, no BDB
  events), the keepalive's TX either fails or is never answered, and the fuse still fires at 60 s —
  the keepalive cannot mask a real wedge of this class. This is the critical property and it holds.

**2. ZDO `NWK_addr_req` to the coordinator (low–medium risk).**
Unicast a ZDO NWK/ieee-address request to `0x0000` on the same 30–60 s cadence. The ZDO response is
also inbound application data and refreshes the same path.
- Effect/cost: similar to #1. Risk slightly higher because the ZDO request/response handler is in the
  prebuilt ZDO code and the app has less control over retries/timing.

**3. Bare unicast keepalive to the parent (medium risk, weaker).**
A one-way APS/NWK frame to `0x0000` relying on the MAC ACK. The MAC ACK is consumed at MAC level and
there is no evidence it reaches `nwk_neTblGetByShortAddr`, so it likely refreshes **neither** `lqi`
nor `age`. Expected anti-wedge effect: low. Not recommended as the primary fix.

**4. Patch the aging path itself — raise `routerAgeLimit` or gate aging off for the parent (high risk).**
This lives in prebuilt `libzb_router.a` (`tl_zbNwkNeighborTabAging`); without SDK source this means a
binary patch or a rebuild we cannot reproduce. It would also weaken *legitimate* parent-loss detection
(a truly dead parent would take far longer to notice) and could break routing. Last resort only.

### 4.2 Host-test additions (`tools/rescue_hosttest`)

The host test already compiles the real `moes_rescue.c` + `moes_liveness.c` against a shim and runs
22 isolated scenarios. A build-08 keepalive should add a `moes_keepalive.c/h` unit to the Makefile and
these scenarios:

- **Keepalive must not reset the liveness silence clock.** Simulate `armed + unjoined + keepalive
  tick every 5 s + zero BDB events`; assert `SYSTEM_RESET` still fires at the 60 s boundary (the
  keepalive callback is not `moes_livenessStackActivity`).
- **Keepalive while joined is a no-op on the fuse.** `joined = 1`, keepalive ticks, assert no reset
  and `s_silence` stays 0.
- **Keepalive scheduling is idempotent and pool-safe.** Re-arm on every `BDB_COMMISSION_STA_SUCCESS`
  without leaking a `TL_ZB_TIMER` slot; assert only one timer exists.
- **Keepalive is armed only when joined / on-network**, and stopped (or made harmless) on
  `PARENT_LOST`/rejoin so it does not transmit during the very scan it is trying to avoid.

---

### Scope note

This document is analysis only. No code file was modified; the only file created is this one.
