# L1 — colour change (commit 5d932a6) hang-class audit

**Workstream:** L1, the colour-temperature change.
**Audited image:** build 04, commit `b485043` (contains `5d932a6`), the exact
tree served to `<redacted-device>` today. HEAD (`315f78e`) has the colour
change reverted by `5274a1f` / `a72a2ae` (build 05), so every line reference
below is against **`git show b485043:light/zcl_colorCtrlCb.c`**, not the
working tree. Line numbers for `light/tuyaLightCtrl.c` are identical in HEAD
and build 04.

---

## Verdict up front

**The colour change does not contain the hard hang.** I read the full diff and
every both-mode branch, the three newly-compiled CCT handlers, the transition
math in `light_applyUpdate` / `light_applyUpdate_16`, `temperatureToCW`, and
the `light_fresh()` re-entry guard. The colour path has no unbounded loop, no
interrupt-disable/restore imbalance, no divide-by-zero reachable from a colour
command, and no uninitialised-state read on the first CCT command after boot.

It **does** contain one genuine hang-class defect (L1-1 below): a
never-stopping 100 ms colour-transition timer when a colour mode switch orphans
the previous mode's `remainingTime`. That is a "scheduled timer that never
stops", but it leaves the CPU and radio alive — it cannot produce `MAC_NO_ACK`,
cannot latch PWM any harder than the last write, and cannot explain "no
reboots". So it is a real defect to fix before the colour change is ever
re-applied, but it is **not the incident**.

The strongest causal role for L1 is indirect: the colour change made the first
real CCT command (the warm state-restore write) change `colorTemperatureMireds`
and mark `lightAttrsChanged`, which one second later triggers the first-ever
colour NV save on this device (`zcl_colorCtrlAttr_save` →
`nv_flashWriteNew` → sector erase). That NV/flash path is L2's lead, not L1.

---

## L1-1 · MEDIUM · Colour-transition timer never stops after a mode switch

**File:** `light/zcl_colorCtrlCb.c` (build 04)
**Key lines:** OR keep-alive at `294-310`; CCT handlers `919-947`,
`958-1009`, `1020-1068`; RGB handlers `379-434` (moveToHue), `445-482`
(moveHue), `493-527` (stepHue), `538-563` (moveToSaturation), `574-611`
(moveSaturation), `613-656` (stepSaturation); XY handlers `692-702`
(moveToColor), `713-723` (moveColor), `734-744` (stepColor).

### Exact causal chain

The timer keep-alive is an OR across *both* compiled halves:

```c
if(0
#if COLOR_RGB_SUPPORT
   || colorInfo.saturationRemainingTime || colorInfo.hueRemainingTime
#endif
#if COLOR_CCT_SUPPORT
   || colorInfo.colorTempRemainingTime
#endif
  ){
    return 0;            /* timer continues */
}else{
    colorTimerEvt = NULL;
    return -1;           /* timer cancelled */
}
```

But the per-mode processing is gated on `pColor->enhancedColorMode`:

- RGB half runs only when `enhancedColorMode` is
  `CURRENT_HUE_SATURATION` or `ENHANCED_CURRENT_HUE_SATURATION` (`271-284`).
- CCT half runs only when `enhancedColorMode` is
  `COLOR_TEMPERATURE_MIREDS` (`285-292`).

The mode-switching handlers set `enhancedColorMode` to the new mode **without
clearing the other mode's `remainingTime`**:

1. Start an RGB fade (`moveToHue`, `moveToSaturation`, …). This sets
   `hueRemainingTime > 0` and/or `saturationRemainingTime > 0` and
   `enhancedColorMode = HUE_SAT`.
2. Before that fade completes, receive a CCT command
   (`moveToColorTemperature` / `moveColorTemperature` / `stepColorTemperature`).
   The handler sets `enhancedColorMode = CCT` and `colorTempRemainingTime > 0`
   but leaves `hueRemainingTime` / `saturationRemainingTime` untouched.
3. The timer now decrements only `colorTempRemainingTime`; the RGB half is
   skipped because `enhancedColorMode == CCT`. The CCT fade finishes and
   `colorTempRemainingTime` reaches 0.
4. The OR keep-alive still sees the orphaned `hueRemainingTime` (or
   `saturationRemainingTime`) > 0 and returns 0 → the 100 ms timer is
   rescheduled forever. `colorTimerEvt` never becomes NULL.

The symmetric case is identical: a CCT fade in progress, then any RGB command
orphans `colorTempRemainingTime` and the timer never stops. The XY stubs
(`moveToColor` / `moveColor` / `stepColor`) set `enhancedColorMode = CURRENT_X_Y`
and also do not clear either half, so they orphan both.

`tuyaLight_stopMoveStepProcess()` (`1081-1094`) does clear all three
`remainingTime` fields, so a `STOP_MOVE_STEP` command terminates the leaked
timer. Without a stop, the 10 Hz no-op callback runs indefinitely.

### Which evidence points it supports / refutes

- **Supports #7** (colour-temperature code path was the only never-run code and
  is live): yes, in the weak sense that the new both-mode timer logic contains a
  real defect.
- **Refutes #5** (`MAC_NO_ACK`, radio not in RX): a perpetual 10 Hz timer
  callback in the main loop does not disable the radio or stop the main loop.
  The MAC would still auto-ACK in hardware. This is the decisive reason L1-1
  cannot be the hang.
- **Refutes #4** (no reboots): a leaked timer produces no reset and no
  probation count. That part it matches, but it also produces no radio death.
- **Neutral on #6** (latched orange): it does not drive the output; the last
  PWM write stands, but it does not *cause* that write to be warm.

### What the SWire SRAM dump would show if this were the culprit

The CPU would still be alive and in the main loop. The stack would show
`ev_main` → `ev_timer_process` → `ev_timer_executeCB` →
`tuyaLight_colorTimerEvtCb` (a repeated frame, not necessarily the *topmost*
at the exact halt instant), `colorTimerEvt != NULL`, and one of
`hueRemainingTime` / `saturationRemainingTime` / `colorTempRemainingTime`
non-zero while `enhancedColorMode` points at a different mode. The radio would
still be in RX, so **the dump would not show a radio-dead state** — that is how
you would know this was not the whole story.

### Fix (do not commit now — see note)

When any colour command selects a mode, clear the *other* mode's transition
state, exactly as `tuyaLight_stopMoveStepProcess()` already clears all three.
Concretely, in each CCT handler after setting `colorMode`/`enhancedColorMode`,
add `colorInfo.hueRemainingTime = 0; colorInfo.saturationRemainingTime = 0;`
and symmetrically zero `colorInfo.colorTempRemainingTime` in the RGB/XY
handlers. This is a one-to-three-line change per handler and is behaviourally
correct: a transition you are no longer driving must not keep the shared timer
alive.

**Why this is not being committed:** HEAD is deliberately build 05, which
reverts the colour change; re-applying `5d932a6` is forbidden by the brief.
The fix belongs inside whatever change re-applies colour temperature, and must
ship together with it, not on top of the reverted tree.

---

## L1-2 · LOW / INFO · `remainingTime == 0xFFFF` "never decrement" is reachable from CCT commands

**File:** `light/tuyaLightCtrl.c:404-430` (`light_applyUpdate_16`), the
`*remainingTime != 0xFFFF` guard at `422-427`.
**Reachable from:** `light/zcl_colorCtrlCb.c` (build 04) `moveColorTemperatureProcess`
(`958-1009`) and `stepColorTemperatureProcess` (`1020-1068`); also the RGB
`moveHue` / `moveSaturation` handlers.

### Exact causal chain

`light_applyUpdate_16` (and `light_applyUpdate`) treats `remainingTime == 0`
as "finish", `0xFFFF` as "never decrement", and anything else as "decrement":

```c
if(*remainingTime == 0){
    *curLevel256 = ((u32)*curLevel) * 256;
    *stepLevel256 = 0;
}else if(*remainingTime != 0xFFFF){
    *remainingTime = *remainingTime -1;
}
```

`moveColorTemperature` UP/DOWN sets `colorTempRemainingTime = 0xFFFF`
deliberately (`983-991`) — that is the ZCL "move until stopped" semantic, and
the SDK's RGB `moveHue`/`moveSaturation` do the same. `moveToColorTemperature`
and `stepColorTemperature` can also land on `0xFFFF` when `transitionTime ==
0xFFFF`, because they only special-case `== 0` (`933`, `1045`); for a
move-to/step command `0xFFFF` then means "step forever in tiny increments"
rather than "complete the move".

In all cases the 100 ms timer runs until a `STOP_MOVE_STEP` arrives (or, for
UP/DOWN, forever even after the value clamps at the physical min/max). This is
a never-stopping timer by design, not a crash, and the same upstream semantics
already existed for RGB before `5d932a6`.

### Evidence

- Supports #7 weakly (new CCT commands now trigger this path).
- Refutes #5 and #4 for the same reason as L1-1: the CPU/radio stay alive; no
  reset occurs. Not the hang.

### SWire prediction

Same living-main-loop signature as L1-1, but `colorTempRemainingTime == 0xFFFF`
(never decrements) and `stepColorTemp256` possibly non-zero or zero
(rate==0 degenerates to a pure no-op keep-alive). Radio still in RX.

---

## L1-3 · CHECKED CLEAN · `light_fresh()` re-entry guard balances on every path

**File:** `light/tuyaLightCtrl.c:315-371`.

Proof requested by the brief, repeated here because the counter is the only
thing between the colour path and unbounded recursion:

```c
static u8 inLightFresh = 0;

if(inLightFresh >= 2){ return; }      /* A: no net change */
#if MOES_TS0505B
if(moes_rescueActive()){ return; }    /* B: no net change */
#endif
inLightFresh++;                       /* C: +1 */
if(lightFx_active()){
    lightFx_start(MOES_EF_STEADY, ...); /* may re-enter via light_adjust ->
                                           colorInit -> light_applyUpdate(_16)
                                           -> light_fresh */
}
tuyaLight_updateColor();              /* void, no re-entry */
tuyaLight_updateOnOff();              /* void, no re-entry */
gLightCtx.lightAttrsChanged = TRUE;
inLightFresh--;                       /* D: -1 */
```

- The only returns are A and B, both **before** the increment.
- Between C and D there is no `return`, and every called function is `void` or
  its return value is ignored; nothing can jump out of `light_fresh` past D.
- The only re-entry is through `lightFx_start(MOES_EF_STEADY, …)` →
  `light_adjust()` → `tuyaLight_colorInit()` → `light_applyUpdate` /
  `light_applyUpdate_16` → `light_fresh()`. `lightFx_start` sets
  `g_moesFx.effect = MOES_EF_STEADY` **before** calling `light_adjust()`
  (`light/light_effects.c:220-229`), so the inner `lightFx_active()` is FALSE.
  The inner `light_fresh()` therefore increments 1→2, runs the body, decrements
  2→1, and returns; any would-be third level hits A at 2 and returns without
  changing the counter.

Net change is 0 on every path (normal, early-return A, early-return B). Depth
is bounded at 2. No hang, no counter leak.

---

## L1-4 · CHECKED CLEAN · Colour arithmetic has no hang primitive

- **`temperatureToCW`** (`tuyaLightCtrl.c:222-236`): divisor is
  `colorTempPhysicalMaxMireds - colorTempPhysicalMinMireds` =
  `0x01C6 - 0x00FA` = 204, both constants (`tuyaLightEpCfg.c:44-45`), both
  attributes `ACCESS_CONTROL_READ` only and not restored from NV. No
  divide-by-zero; `W <= level`, so `C = level - W` cannot underflow.
- **`light_applyUpdate_16`** (`tuyaLightCtrl.c:404-430`): no loops; the clamp
  and wrap branches use `u32`/`s32` intermediates within range
  (`currentColorTemp256 <= 0xFFFF<<8`). No wraparound can spin it.
- **CCT step computation** (`zcl_colorCtrlCb.c` build 04 `935-936`, `989-990`,
  `1047`): `stepColorTemp256` is `s32`; `((s32)u16) << 8` peaks at
  `0xFFFF00` ≈ 16.7 M, division by a `u16` of at least 1. No divide-by-zero.
- **`hsvToRGB`** (`tuyaLightCtrl.c:255-285`): `remainder` is bounded `<= 236`,
  `region` bounded `0..5`. No hang.
- **Uninitialised state on first CCT command after boot:** not present. The
  file statics are zero-initialised, and the both-mode `tuyaLight_colorInit`
  (`zcl_colorCtrlCb.c` build 04 `123-166`) initialises the CCT half when
  `colorMode` is CCT (the attribute-table default). If an RGB command runs
  first, the CCT handlers each set `currentColorTemp256`, `colorTempMinMireds`,
  `colorTempMaxMireds`, `colorTempRemainingTime` and `stepColorTemp256` before
  use (`919-947`, `958-1009`, `1020-1068`).

---

## L1-5 · CONTEXT / BRIDGE TO L2 · The colour change is the trigger for the first colour NV save

Not a hang in L1 code, but it is the causal link between the warm write and the
death window:

1. HA/z2m restores state → a CCT command runs one of the three handlers →
   `light_applyUpdate_16` → `light_fresh()` → `gLightCtx.lightAttrsChanged =
   TRUE` (`tuyaLightCtrl.c:368`).
2. `app_task()` → `tuyaLightAttrsChk()` → `tuyaLightAttrsStoreTimerStart()`
   (`light/tuyaLight.c:271-286`).
3. One second later `tuyaLightAttrsStoreTimerCb` calls
   `zcl_onOffAttr_save()`, `zcl_levelAttr_save()`,
   `zcl_colorCtrlAttr_save()` (`tuyaLight.c:261-269`).
4. On this fresh-conversion unit, that is the first time
   `NV_ITEM_ZCL_COLOR_CTRL` is written with a value different from the
   attribute default (a warm `colorTemperatureMireds`), so `zcl_colorCtrlAttr_save`
   performs an `nv_flashWriteNew` → sector erase inside the configure/state-restore
   window.

That flash-erase path is exactly what L2 is auditing. If the dump shows a
half-written ZCL/APS NV sector or a torn index, L2 (not L1) is the hang. The
orange is explained by the *successful* colour write before the erase hung.

---

## The no-announce question (relevant to the sequence, kept short)

No application code sends `device_announce`: `zb_deviceAnnounce*` / announce
request APIs are absent from `light/`, and the "device announce indication cb"
in `appCbLst` is `NULL` (that slot is for *receiving*). Whether the prebuilt
`libzb_router.a` sends one on first join is opaque; the field result says it
did not. So the announce-cadence gate (`tools/announce_watch.py`) is **blind
during exactly the fresh-join conversion window** this incident occupied. That
confirms master-brief §2.3 and is not a colour-change finding.

---

## What the SWire SRAM dump would show under each L1 outcome

- **If L1-1 or L1-2 were the culprit:** living CPU, stack in
  `ev_main`/`ev_timer_process`/`ev_timer_executeCB`/`tuyaLight_colorTimerEvtCb`,
  `colorTimerEvt` non-NULL, radio still in RX. **Inconsistent with the observed
  `MAC_NO_ACK`, so not expected.**
- **If the colour change merely triggered L2 (my prediction):** the stack at
  the halt is in the flash/NV path (or `libzb_router.a`), not in the colour
  path; a half-written ZCL/APS NV sector or torn index is present in flash at
  `0xD8000–0xEE000`; the last PWM write was warm (orange) because the colour
  write completed before the erase hung.

---

## Ranked summary

1. **L1-1** — never-stopping colour timer on mode switch (MEDIUM, real,
   hang-class, but not the incident). Fix bundled with any future re-apply of
   `5d932a6`, not committed now.
2. **L1-2** — `0xFFFF` continuous-move semantics now reachable via CCT
   (LOW/INFO, intended upstream behaviour, not the incident).
3. **L1-5** — colour change is the trigger for the first colour NV save; the
   hang itself most plausibly lives in L2's flash/NV path.
4. **L1-3 / L1-4** — guard and arithmetic clean (negative results).

**No code changes committed.** The only defect found lives in the deliberately
reverted colour change; re-applying `5d932a6` is forbidden, and committing a
fix against the reverted file would be meaningless. Build verification was
therefore not required for this workstream (no files touched).
