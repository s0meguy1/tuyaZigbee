/* Functional host test for the real light/zcl_levelCb.c implementation. */
#include <stdio.h>
#include <string.h>

#include "onoff_shim/tl_common.h"
#include "onoff_shim/zb_api.h"
#include "onoff_shim/zcl_include.h"
#include "../../light/tuyaLight.h"

zcl_levelAttr_t g_zcl_levelAttrs;
zcl_onOffAttr_t g_zcl_onOffAttrs;

static ev_timer_event_t sim_timer;
static int onoff_count;
static u8 last_onoff;
static int render_count;

ev_timer_event_t *host_timerSchedule(ev_timer_callback_t cb, void *arg, u32 interval)
{
    sim_timer.cb = cb;
    sim_timer.data = arg;
    sim_timer.interval = interval;
    sim_timer.live = TRUE;
    return &sim_timer;
}

void host_timerCancel(ev_timer_event_t **event)
{
    if (event && *event) {
        (*event)->live = FALSE;
        *event = NULL;
    }
}

void light_fresh(void)
{
    render_count++;
}

/*
 * A faithful copy of light_applyUpdate() from light/tuyaLightCtrl.c, which
 * cannot be linked here because it pulls in the whole ZCL and hardware
 * surface. It has to be faithful and not merely approximate: the rounding
 * asymmetry below (round on the way up, floor on the way down) is exactly what
 * makes a long up-ramp able to land one level short of its target, which is
 * the defect the landing tests in this file cover.
 */
void light_applyUpdate(u8 *curLevel, u16 *curLevel256, s32 *stepLevel256,
                       u16 *remainingTime, u8 minLevel, u8 maxLevel, bool wrap)
{
    if ((*stepLevel256 > 0) && ((((s32)*curLevel256 + *stepLevel256) / 256) > maxLevel)) {
        *curLevel256 = (wrap) ? ((u16)minLevel * 256 + ((*curLevel256 + *stepLevel256) - (u16)maxLevel * 256) - 256)
                              : ((u16)maxLevel * 256);
    } else if ((*stepLevel256 < 0) && ((((s32)*curLevel256 + *stepLevel256) / 256) < minLevel)) {
        *curLevel256 = (wrap) ? ((u16)maxLevel * 256 - ((u16)minLevel * 256 - ((s32)*curLevel256 + *stepLevel256)) + 256)
                              : ((u16)minLevel * 256);
    } else {
        *curLevel256 += *stepLevel256;
    }

    if (*stepLevel256 > 0) {
        *curLevel = (*curLevel256 + 127) / 256;
    } else {
        *curLevel = *curLevel256 / 256;
    }

    if (*remainingTime == 0) {
        *curLevel256 = ((u16)*curLevel) * 256;
        *stepLevel256 = 0;
    } else if (*remainingTime != 0xFFFF) {
        *remainingTime = *remainingTime - 1;
    }

    light_fresh();
}

void hwLight_levelUpdate(u8 level)
{
    (void)level;
}

void tuyaLight_onoff(u8 cmd)
{
    onoff_count++;
    last_onoff = cmd;
    g_zcl_onOffAttrs.onOff = (cmd == ZCL_CMD_ONOFF_ON) ? TRUE : FALSE;
}

static void reset_sim(u8 level, bool on)
{
    memset(&g_zcl_levelAttrs, 0, sizeof(g_zcl_levelAttrs));
    memset(&g_zcl_onOffAttrs, 0, sizeof(g_zcl_onOffAttrs));
    memset(&sim_timer, 0, sizeof(sim_timer));
    g_zcl_levelAttrs.curLevel = level;
    g_zcl_onOffAttrs.onOff = on;
    onoff_count = 0;
    last_onoff = 0xFF;
    render_count = 0;
}

static int move_to(u8 command, u8 target)
{
    zclIncomingAddrInfo_t address = { .dstEp = TUYA_LIGHT_ENDPOINT };
    moveToLvl_t move = { .level = target, .transitionTime = 0 };
    return tuyaLight_levelCb(&address, command, &move);
}

static int move_to_over(u8 command, u8 target, u16 transition_time)
{
    zclIncomingAddrInfo_t address = { .dstEp = TUYA_LIGHT_ENDPOINT };
    moveToLvl_t move = { .level = target, .transitionTime = transition_time };
    return tuyaLight_levelCb(&address, command, &move);
}

/* Run the ramp to completion, returning how many ticks it took. The cap is a
 * few times the longest legitimate ramp so a runaway timer fails the test
 * instead of hanging the build. */
static int run_to_completion(void)
{
    int ticks = 0;

    while (sim_timer.live && sim_timer.cb) {
        s32 result = sim_timer.cb(sim_timer.data);
        ticks++;
        if (result < 0) {
            sim_timer.live = FALSE;
            break;
        }
        if (ticks > 5000) return -1;
    }
    return ticks;
}

static int run_live_timer(void)
{
    s32 result;

    if (!sim_timer.live || !sim_timer.cb) return 99;
    result = sim_timer.cb(sim_timer.data);
    if (result < 0) sim_timer.live = FALSE;
    return (int)result;
}

static int test_off_to_same_nonminimum_turns_on(void)
{
    reset_sim(123, FALSE);
    if (move_to(ZCL_CMD_LEVEL_MOVE_TO_LEVEL_WITH_ON_OFF, 123) != ZCL_STA_SUCCESS) return 10;
    if (!g_zcl_onOffAttrs.onOff || onoff_count != 1 || last_onoff != ZCL_CMD_ONOFF_ON) return 11;
    return 0;
}

static int test_off_to_lower_nonminimum_turns_on(void)
{
    reset_sim(200, FALSE);
    if (move_to(ZCL_CMD_LEVEL_MOVE_TO_LEVEL_WITH_ON_OFF, 123) != ZCL_STA_SUCCESS) return 20;
    if (!g_zcl_onOffAttrs.onOff || g_zcl_levelAttrs.curLevel != 123 ||
        onoff_count != 1 || last_onoff != ZCL_CMD_ONOFF_ON) return 21;
    return 0;
}

static int test_off_to_higher_nonminimum_still_turns_on(void)
{
    reset_sim(123, FALSE);
    if (move_to(ZCL_CMD_LEVEL_MOVE_TO_LEVEL_WITH_ON_OFF, 200) != ZCL_STA_SUCCESS) return 30;
    if (!g_zcl_onOffAttrs.onOff || g_zcl_levelAttrs.curLevel != 200 ||
        onoff_count != 1 || last_onoff != ZCL_CMD_ONOFF_ON) return 31;
    return 0;
}

static int test_minimum_target_turns_off(void)
{
    reset_sim(123, TRUE);
    if (move_to(ZCL_CMD_LEVEL_MOVE_TO_LEVEL_WITH_ON_OFF, ZCL_LEVEL_ATTR_MIN_LEVEL) != ZCL_STA_SUCCESS) return 40;
    if (g_zcl_onOffAttrs.onOff || g_zcl_levelAttrs.curLevel != ZCL_LEVEL_ATTR_MIN_LEVEL ||
        onoff_count != 1 || last_onoff != ZCL_CMD_ONOFF_OFF) return 41;
    return 0;
}

static int test_without_onoff_leaves_power_state_alone(void)
{
    reset_sim(123, FALSE);
    if (move_to(ZCL_CMD_LEVEL_MOVE_TO_LEVEL, 200) != ZCL_STA_SUCCESS) return 50;
    if (g_zcl_onOffAttrs.onOff || g_zcl_levelAttrs.curLevel != 200 || onoff_count != 0) return 51;
    return 0;
}

static int test_lower_nonminimum_transition_turns_on_immediately(void)
{
    reset_sim(200, FALSE);
    if (move_to_over(ZCL_CMD_LEVEL_MOVE_TO_LEVEL_WITH_ON_OFF, 123, 2) != ZCL_STA_SUCCESS) return 60;
    if (!g_zcl_onOffAttrs.onOff || g_zcl_levelAttrs.curLevel == 123 ||
        onoff_count != 1 || last_onoff != ZCL_CMD_ONOFF_ON || !sim_timer.live) return 61;
    /* 2 deciseconds is 10 ticks at 20 ms; one was consumed by the command. */
    if (run_to_completion() != 9) return 62;
    if (!g_zcl_onOffAttrs.onOff || g_zcl_levelAttrs.curLevel != 123 || onoff_count != 1) return 63;
    return 0;
}

static int test_fade_to_minimum_turns_off_only_at_the_end(void)
{
    reset_sim(200, TRUE);
    if (move_to_over(ZCL_CMD_LEVEL_MOVE_TO_LEVEL_WITH_ON_OFF, ZCL_LEVEL_ATTR_MIN_LEVEL, 2) != ZCL_STA_SUCCESS) return 70;
    if (!g_zcl_onOffAttrs.onOff || g_zcl_levelAttrs.curLevel == ZCL_LEVEL_ATTR_MIN_LEVEL ||
        onoff_count != 0 || !sim_timer.live) return 71;
    if (run_to_completion() != 9) return 72;
    if (g_zcl_onOffAttrs.onOff || g_zcl_levelAttrs.curLevel != ZCL_LEVEL_ATTR_MIN_LEVEL ||
        onoff_count != 1 || last_onoff != ZCL_CMD_ONOFF_OFF) return 73;
    return 0;
}


/* ---------------------------------------------------------------------------
 * Build 38: the ramp advances every 20 ms instead of every 100 ms, and the ZCL
 * RemainingTime attribute is derived from that rather than being the loop
 * counter. These cases pin both halves of that, and the exact landing that the
 * higher tick count made necessary.
 * ------------------------------------------------------------------------- */

void tuyaLight_levelTransitionCancel(void);

static int move_cmd(u8 command, u8 mode, u8 rate)
{
    zclIncomingAddrInfo_t address = { .dstEp = TUYA_LIGHT_ENDPOINT };
    move_t move = { .moveMode = mode, .rate = rate };
    return tuyaLight_levelCb(&address, command, &move);
}

static int step_cmd(u8 command, u8 mode, u8 size, u16 transition_time)
{
    zclIncomingAddrInfo_t address = { .dstEp = TUYA_LIGHT_ENDPOINT };
    step_t step = { .stepMode = mode, .stepSize = size, .transitionTime = transition_time };
    return tuyaLight_levelCb(&address, command, &step);
}

static int test_five_second_fade_renders_250_times(void)
{
    /* The whole point of build 38. 5 s used to be 50 output updates, which is
     * what the eye reads as choppy. */
    reset_sim(254, TRUE);
    if (move_to_over(ZCL_CMD_LEVEL_MOVE_TO_LEVEL, ZCL_LEVEL_ATTR_MIN_LEVEL, 50) != ZCL_STA_SUCCESS) return 80;
    if (run_to_completion() != 249) return 81;
    if (render_count != 250) return 82;
    if (g_zcl_levelAttrs.curLevel != ZCL_LEVEL_ATTR_MIN_LEVEL) return 83;
    if (g_zcl_levelAttrs.remainingTime != 0) return 84;
    return 0;
}

static int test_long_up_ramp_lands_exactly_on_target(void)
{
    /* stepLevel256 is a truncated 8.8 quotient, so a 250-tick ramp can
     * accumulate almost a whole level of shortfall. A rise of exactly 63
     * levels over 5 s is the case where that shortfall crosses the rounding
     * boundary: without the landing snap this ends on 63. */
    reset_sim(1, TRUE);
    if (move_to_over(ZCL_CMD_LEVEL_MOVE_TO_LEVEL, 64, 50) != ZCL_STA_SUCCESS) return 90;
    if (run_to_completion() != 249) return 91;
    if (g_zcl_levelAttrs.curLevel != 64) return 92;
    return 0;
}

static int test_fade_to_off_still_switches_off_after_250_ticks(void)
{
    /* And the reason the landing matters beyond one level of brightness: a
     * with-on-off fade that stops one short of the minimum never issues its
     * Off, and leaves the fixture dimly lit instead of dark. Exactly once,
     * too - the ramp can sit at the minimum for many ticks. */
    reset_sim(254, TRUE);
    if (move_to_over(ZCL_CMD_LEVEL_MOVE_TO_LEVEL_WITH_ON_OFF, ZCL_LEVEL_ATTR_MIN_LEVEL, 50) != ZCL_STA_SUCCESS) return 100;
    if (run_to_completion() != 249) return 101;
    if (g_zcl_onOffAttrs.onOff) return 102;
    if (onoff_count != 1 || last_onoff != ZCL_CMD_ONOFF_OFF) return 103;
    return 0;
}

static int test_remaining_time_stays_in_deciseconds(void)
{
    int i;

    reset_sim(254, TRUE);
    if (move_to_over(ZCL_CMD_LEVEL_MOVE_TO_LEVEL, 100, 50) != ZCL_STA_SUCCESS) return 110;
    /* One tick was consumed by the command itself; 249 of 250 remain, which
     * still rounds up to the full 5.0 seconds. */
    if (g_zcl_levelAttrs.remainingTime != 50) return 111;

    for (i = 0; i < 5; i++) {
        if (run_live_timer() < 0) return 112;
    }
    if (g_zcl_levelAttrs.remainingTime != 49) return 113;

    if (run_to_completion() < 0) return 114;
    if (g_zcl_levelAttrs.remainingTime != 0) return 115;
    if (g_zcl_levelAttrs.curLevel != 100) return 116;
    return 0;
}

static int test_unspecified_transition_time_is_immediate(void)
{
    /* 0 and 0xFFFF both mean "now" and must stay one tick, not one decisecond
     * of ticks: zigbee2mqtt sends 0 for an ordinary brightness set. */
    reset_sim(10, TRUE);
    if (move_to_over(ZCL_CMD_LEVEL_MOVE_TO_LEVEL, 200, 0xFFFF) != ZCL_STA_SUCCESS) return 120;
    if (g_zcl_levelAttrs.curLevel != 200) return 121;
    if (sim_timer.live) return 122;

    reset_sim(10, TRUE);
    if (move_to_over(ZCL_CMD_LEVEL_MOVE_TO_LEVEL, 200, 0) != ZCL_STA_SUCCESS) return 123;
    if (g_zcl_levelAttrs.curLevel != 200) return 124;
    if (sim_timer.live) return 125;
    return 0;
}

static int test_move_command_terminates_at_the_end_stop(void)
{
    reset_sim(254, TRUE);
    if (move_cmd(ZCL_CMD_LEVEL_MOVE_WITH_ON_OFF, LEVEL_MOVE_DOWN, 50) != ZCL_STA_SUCCESS) return 130;
    if (run_to_completion() < 0) return 131;
    if (g_zcl_levelAttrs.curLevel != ZCL_LEVEL_ATTR_MIN_LEVEL) return 132;
    if (onoff_count != 1 || last_onoff != ZCL_CMD_ONOFF_OFF) return 133;
    if (g_zcl_levelAttrs.remainingTime != 0) return 134;

    /* A zero rate must not divide by zero; it is a single-tick move. */
    reset_sim(254, TRUE);
    if (move_cmd(ZCL_CMD_LEVEL_MOVE, LEVEL_MOVE_DOWN, 0) != ZCL_STA_SUCCESS) return 135;
    if (g_zcl_levelAttrs.curLevel != ZCL_LEVEL_ATTR_MIN_LEVEL) return 136;
    if (sim_timer.live) return 137;
    return 0;
}

static int test_step_command_lands_on_its_target(void)
{
    reset_sim(200, TRUE);
    if (step_cmd(ZCL_CMD_LEVEL_STEP, LEVEL_STEP_DOWN, 100, 20) != ZCL_STA_SUCCESS) return 140;
    if (run_to_completion() < 0) return 141;
    if (g_zcl_levelAttrs.curLevel != 100) return 142;

    /* A step past an end stop clamps there rather than wrapping. */
    reset_sim(200, TRUE);
    if (step_cmd(ZCL_CMD_LEVEL_STEP, LEVEL_STEP_UP, 200, 10) != ZCL_STA_SUCCESS) return 143;
    if (run_to_completion() < 0) return 144;
    if (g_zcl_levelAttrs.curLevel != ZCL_LEVEL_ATTR_MAX_LEVEL) return 145;
    return 0;
}

static int test_cancel_drops_the_ramp_where_it_stands(void)
{
    u8 level_when_cancelled;

    reset_sim(254, TRUE);
    if (move_to_over(ZCL_CMD_LEVEL_MOVE_TO_LEVEL, ZCL_LEVEL_ATTR_MIN_LEVEL, 50) != ZCL_STA_SUCCESS) return 150;
    if (run_live_timer() < 0) return 151;
    level_when_cancelled = g_zcl_levelAttrs.curLevel;

    tuyaLight_levelTransitionCancel();
    if (sim_timer.live) return 152;
    if (g_zcl_levelAttrs.remainingTime != 0) return 153;
    if (g_zcl_levelAttrs.curLevel != level_when_cancelled) return 154;
    return 0;
}

int main(void)
{
    int result;

    if ((result = test_off_to_same_nonminimum_turns_on()) != 0) goto failed;
    if ((result = test_off_to_lower_nonminimum_turns_on()) != 0) goto failed;
    if ((result = test_off_to_higher_nonminimum_still_turns_on()) != 0) goto failed;
    if ((result = test_minimum_target_turns_off()) != 0) goto failed;
    if ((result = test_without_onoff_leaves_power_state_alone()) != 0) goto failed;
    if ((result = test_lower_nonminimum_transition_turns_on_immediately()) != 0) goto failed;
    if ((result = test_fade_to_minimum_turns_off_only_at_the_end()) != 0) goto failed;
    if ((result = test_five_second_fade_renders_250_times()) != 0) goto failed;
    if ((result = test_long_up_ramp_lands_exactly_on_target()) != 0) goto failed;
    if ((result = test_fade_to_off_still_switches_off_after_250_ticks()) != 0) goto failed;
    if ((result = test_remaining_time_stays_in_deciseconds()) != 0) goto failed;
    if ((result = test_unspecified_transition_time_is_immediate()) != 0) goto failed;
    if ((result = test_move_command_terminates_at_the_end_stop()) != 0) goto failed;
    if ((result = test_step_command_lands_on_its_target()) != 0) goto failed;
    if ((result = test_cancel_drops_the_ramp_where_it_stands()) != 0) goto failed;

    printf("all level/onoff scenarios passed (real zcl_levelCb.c)\n");
    return 0;

failed:
    fprintf(stderr, "level/onoff test failed: %d\n", result);
    return result;
}
