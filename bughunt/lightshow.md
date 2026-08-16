# Light-show robustness pass

**Workstream scope:** independently of leads L1/L2, audit the code a light
show actually hammers — the 0xEF00 effect handler, the effect engine and its
timer, the PWM update path, the ZCL colour/level write path under rapid
successive commands, the reporting sanitizer under attribute churn, and the
queue between ZCL RX and the light task. Hunt hang-class and starvation-class
defects only: unbounded loops per command, timer storms, stale-state when a
new command lands mid-transition, per-command NV writes, watchdog-hostile long
sections.

**Branch / HEAD:** `moes-ts0505b`, working base `315f78e`, plus one fix
committed below.

**Ranking key:** "would this kill a light mid-show?" A light show means
sustained high-rate colour/level/effect writes, often broadcast to a group.

---

## Fixed

### LS-1 · CRITICAL (hang) · Level `move` command with `rate == 0` divides by zero

**Where:** `light/zcl_levelCb.c:219` (before fix), function
`tuyaLight_moveProcess`.

**Causal chain, exactly:**

1. Zigbee-herdsman / a light-show driver sends ZCL Level Control cluster
   command `move` (`0x01`) or `move with on/off` (`0x05`) with `rate = 0`.
   `rate` is read straight off the wire in the SDK parser
   (`zcl_level_clientCmdHandler`, `build/.../zigbee/zcl/general/zcl_level.c:183`)
   and passed to the app callback unvalidated.
2. `tuyaLight_levelCb` (`light/zcl_levelCb.c:334`) dispatches to
   `tuyaLight_moveProcess`.
3. `tuyaLight_moveProcess` computes `u32 rate = (u32)cmd->rate * 100`, which
   is `0`, then executes `pLevel->remainingTime = ((u32)deltaLevel * 1000) / rate`.
4. That compiles to a call into the tc32 software integer divider
   (`platform/tc32/div_mod.S`). `div` writes dividend/divisor/op to the
   hardware divider registers and then busy-waits:
   ```asm
   .L2:
       tloadrb r0, [r3]      ; status register
       tcmp   r0, #0
       tjne   .L2           ; spin until status == 0
   ```
   It never checks for a zero divisor. With divisor 0 the silicon either
   never clears the busy bit (hard hang, radio dead, PWM latched, no reset)
   or completes with a garbage result; in the garbage case the `u16`
   `remainingTime` truncates to `0xFFFF` and `light_applyUpdate` then treats
   `0xFFFF` as "never decrement" — a perpetual 100 ms timer that does nothing.
   Both outcomes are forward-progress failure without a reset.

**Which of the 7 evidence points it supports/refutes:**

- Supports **4** (no reboots — a spin in `div` produces no exception and no
  `SYSTEM_RESET()`), **5** (`MAC_NO_ACK` continuous — CPU is stuck in the
  divider busy-wait, so the radio is never put back in RX), and is consistent
  with **6** if the last command before death was a warm-white *level* move
  (the PWM would latch at whatever duty the last successful `light_fresh`
  wrote).
- Does **not** explain **3** (the no-announce on fresh join is a separate
  question), and is **not** the L1 colour-temperature path (**7**); it is a
  level-cluster path that has existed since before `5d932a6`.

**SWire dump signature if this were the culprit:** SRAM PC at `div`/`.L2`
(busy-wait around `0x800660`), return-address chain
`tuyaLight_moveProcess` → `tuyaLight_levelCb` →
`zcl_level_clientCmdHandler` → `zcl_cmdHandler` → `tl_zbTaskProcedure` →
`main`; `T_evtExcept` all zero (no exception); PWM compare registers holding
a warm/white duty; radio MAC state idle, not RX.

**Fix:** guard `rate == 0` and treat it as a single-tick move. Committed as
`739ad92` ("Guard level move rate against zero (divide-by-zero hang)").
Build verified clean below.

---

## Findings — not committed (debatable or not unambiguous)

### LS-2 · MEDIUM (stale-state corruption, not a hang) · A ZCL colour command landing while an effect is running clobbers its own transition

**Where:** `light/tuyaLightCtrl.c:315` (`light_fresh`) interacting with
`light/tuyaLightCtrl.c:303` (`light_adjust`) and
`light/zcl_colorCtrlCb.c:110` (`tuyaLight_colorInit`).

**Causal chain:** every colour command (`move_to_color_temperature`,
`move_to_hue`, `move_to_saturation`, …) sets up a transition in the static
`colorInfo` and then calls `light_applyUpdate*()`, which calls
`light_fresh()`. If `lightFx_active()` is true (a show is running),
`light_fresh()` first calls `lightFx_start(MOES_EF_STEADY, …)`, which calls
`light_adjust()`, which calls `tuyaLight_colorInit()`. That function resets
`colorInfo.currentHue256/currentColorTemp256`, zeroes
`hueRemainingTime/saturationRemainingTime/colorTempRemainingTime`, and runs
`light_applyUpdate*()` again — **after** the outer handler has already stored
its new transition parameters in the same statics. The net effect: the
requested transition is destroyed, the colour jumps one step, and no colour
timer is scheduled.

This is not a hang (the re-entry guard in `light_fresh` keeps the recursion
bounded at depth 2), but it is exactly the "stale-state when a new command
lands mid-transition" class. It means "stop the show and fade to colour X"
does not fade; it snaps. Fixing it cleanly requires reordering effect-stop
before the transition is set up, which touches the shared `light_fresh`
contract, so it stays a finding rather than a silent change.

**Evidence:** consistent with **6** in a show context (a jump to a warm
colour), does not explain radio death / no-announce / no-reboots.

**SWire dump if this were the culprit:** not applicable — it does not hang.

### LS-3 · LOW (starvation-ish, by design) · Continuous colour/level `move` runs a perpetual 100 ms timer with `light_fresh` every tick

**Where:** `light/zcl_colorCtrlCb.c:401/926` (`remainingTime = 0xFFFF` paths)
and `light/zcl_levelCb.c:242`; timer callbacks
`tuyaLight_colorTimerEvtCb` / `tuyaLight_levelTimerEvtCb`.

**Causal chain:** `move up/down` (and the CCT equivalent) sets
`remainingTime = 0xFFFF`, which `light_applyUpdate(_16)` deliberately never
decrements (`tuyaLightCtrl.c:397/425`). The 100 ms timer therefore runs until
a `stop` command arrives. Each tick calls `light_fresh()`, which sets
`gLightCtx.lightAttrsChanged = TRUE`; `app_task` then cancels and re-arms the
1 s attribute-store timer on every idle poll, so the store timer is
perpetually deferred while the move runs. That is bounded and cheap per tick,
and it does **not** cause NV writes during the move — but it is a permanent
timer plus per-tick `light_fresh` churn that a future watchdog build must
keep in mind (each tick is far below 600 ms, so it is still safe).

**Evidence:** does not explain the build-04 hang (it produces live radio and
a moving light, not `MAC_NO_ACK`).

### LS-4 · LOW (starvation/leak) · ZCL RX task queue overflow leaks RX buffers

**Where:** `build/.../zigbee/zcl/zcl.c:878` (`zcl_rx_handler`) →
`TL_SCHEDULE_TASK` = `tl_zbTaskPost` (declared
`build/.../zigbee/common/includes/zb_task_queue.h:131`, implementation in the
prebuilt `libzb_router.a`, `TL_ZBTASKQ_USERUSE_SIZE 32`).

**Causal chain:** every incoming ZCL frame is posted to a 32-deep user task
queue. `zcl_rx_handler` ignores `tl_zbTaskPost`'s return value. If a burst of
>32 ZCL commands arrives before `tl_zbTaskProcedure()` drains the queue, the
post fails and the `apsdeDataInd_t` buffer that `zcl_cmdHandler` would
otherwise `ev_buf_free()` at its end is leaked. Repeated bursts exhaust the
`ev_buffer` pool, which degrades the radio (allocation failures) without a
reset. A normal light show (~25 cmds/s, each handled in microseconds) is far
below this, but a fast script or a broadcast storm can cross it.

**Not committed:** the overflow handling is inside the prebuilt library and
`zcl_rx_handler` is vendored SDK code shared with every app; changing its
buffer-ownership contract is riskier than the finding justifies. Recorded so
it is not mistaken for clean.

**Evidence:** could contribute to radio degradation under a command storm, but
does not explain the deterministic post-configure hang (configure is not a
>32-frame burst).

### LS-5 · LOW · `zcl_parseInWriteCmd` has weak malformed-WRITE bounds handling

**Where:** `build/.../zigbee/zcl/zcl.c:1260` (`zcl_parseInWriteCmd`).

**Causal chain:** the first pass advances `pBuf` by
`2 + 1 + zcl_getAttrSize(dataType, pBuf)` and only checks the loop bound at
the top. An unknown or length-prefixed data type can make `zcl_getAttrSize`
return 0 or a value that advances past the ASDU; the second pass then
`memcpy`s out of bounds. This is an out-of-bounds read into the `ev_buffer`
pool, not a hang, and is upstream SDK code. A light-show driver that emits a
malformed WRITE could trip it, but the practical impact is a bogus response,
not forward-progress failure. **Not committed** (vendored SDK, out of scope
for a minimal light-show pass).

### LS-6 · INFO · Effect timeline wraps after ~49.7 days

**Where:** `light/light_effects.c:199` (`g_moesFx.t += MOES_EFFECT_TICK_MS`,
`u32`).

`g_moesFx.t` is a `u32` in milliseconds; it wraps after ~49.7 days of
continuous show. At wrap the `t % p` / `t / p` terms jump, causing one
discontinuity frame, then the effect continues. Cosmetic; not a hang.

---

## Checked and found clean (light-show specific)

### Effect engine arithmetic — no unbounded loops, no divide-by-zero

Re-walked every `fxRender` case at `light/light_effects.c:72`:

- `fxPeriod()` (`:39`) cannot divide by zero (`sp = speed ? speed : 50`,
  `p = (slowMs*100)/(2*sp)`, returns `p ? p : 1`).
- `MOES_EF_LIGHTNING`'s `r0 % (p * 3 / 4)` is safe: smallest `p` for
  `slowMs=3000` is 588, so the modulus is ≥ 441.
- Every `fxSine[...]` index is `< 64` (form `(t % p) * 64 / p`).
- `MOES_EF_CHASE`, `MOES_EF_COLOR_STEP`, `MOES_EF_WAVE`, `MOES_EF_TWINKLE`
  sums are bounded and reduced before any narrowing cast (the two historical
  cast-order bugs are fixed in `d0285ce`).
- `fxNoise()`'s `zb_random()` call is a cheap LCG; two calls per 25 fps frame
  are negligible.

No case has an unbounded loop. A frame is one integer HSV conversion plus
five PWM register writes — tens of microseconds out of a 40 ms budget.

### Effect timer lifecycle — no timer storm

`lightFx_start()` (`light/light_effects.c:205`) only schedules `fxTimer` when
`fxTimer == NULL`; re-issuing an effect while one is running updates state and
renders one frame but does not stack timers. `MOES_EF_STEADY` cancels and
NULLs the timer before calling `light_adjust()`. Rescue mode refuses any
non-steady effect before any timer is created (`:215`).

### Colour/level timer lifecycle — cancel-before-schedule

Every colour transition handler and every level transition handler calls
`tuyaLight_colorTimerStop()` / `tuyaLight_LevelTimerStop()` **before**
scheduling the 100 ms tick (`zcl_colorCtrlCb.c` each `*Process`,
`zcl_levelCb.c:185/243/288`). No transition path stacks two timers.

### PWM update path — no u32 overflow at the compiled 4 kHz

`moes_flashCfgLoad()` sets `g_moesCfg.pwmhz = MOES_PWM_FREQUENCY_DEFAULT`
(`4000`, `device_config/light_ts0505b.h:99`) and leaves `g_moesCfg.valid = 0`,
so `hwLight_init()` never takes the JSON-driven PWM period branch.
`moes_pwmMaxTick = PWM_CLOCK_SOURCE / 4000`; `pwmSetDuty()` computes
`dutycycle * moes_pwmMaxTick / 25400` with `dutycycle ≤ 25500`. Even at a 48
MHz `PWM_CLOCK_SOURCE` the intermediate is ~3.06e8, well inside `u32`. No
overflow, no unbounded loop.

### Attribute-store NV cadence — debounced, not per-command

`light_fresh()` sets `lightAttrsChanged = TRUE`, but
`tuyaLightAttrsChk()` (`light/tuyaLight.c:279`) only runs once per idle poll
and `tuyaLightAttrsStoreTimerStart()` (`:271`) cancels the existing 1 s timer
before re-arming. A flood of ZCL colour/level commands therefore defers the
single NV save until 1 s after the flood stops — it does **not** write NV per
command. The effect engine bypasses `light_fresh` entirely (it calls
`moes_outSet` directly), so a running show performs **zero** attribute NV
writes.

### Reporting sanitizer — boot-time only, bounded

`tuyaLight_reportingTabSanitize()` (`light/tuyaLight.c:234`) loops over
`ZCL_REPORTING_TABLE_NUM` (4) once at boot, drops unresolvable restored
entries, and persists only when it dropped something. It has no per-frame or
per-command cost, so attribute churn during a show does not re-enter it.
`report_handler()`/`reportNoMinLimit()` (`app_task`) and the reporting timer
are all bounded by the same 4-entry table; `reportAttrTimerStart()` is
guarded by `!reportAttrTimerEvt`, so no timer storm.

### No separate light-task ring buffer

There is no custom queue between ZCL callbacks and the light task: ZCL RX is
posted through the SDK's prebuilt `tl_zbTaskPost` user queue (depth 32), and
PWM updates happen synchronously inside `zcl_cmdHandler` via `light_fresh`.
The only other "queue" is the effect engine's single repeating timer. The
overflow exposure of that prebuilt queue is LS-4.

---

## Build status

```bash
cd work/tuyaZigbee
cmake . -B build -DDEVICE_VARIANT=TS0505B
cmake --build build --target light_TS0505B.zigbee -j8
```

Build passes after the fix. Only `light/zcl_levelCb.c` was recompiled, no new
warnings; the two known-benign unused-static warnings in
`zcl_colorCtrlCb.c` are unchanged. Commit: `739ad92`.
