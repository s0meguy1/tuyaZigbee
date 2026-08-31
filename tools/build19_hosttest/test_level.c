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

void light_applyUpdate(u8 *curLevel, u16 *curLevel256, s32 *stepLevel256,
                       u16 *remainingTime, u8 minLevel, u8 maxLevel, bool wrap)
{
    s32 next = (s32)*curLevel256 + *stepLevel256;
    (void)wrap;

    if (next < (s32)minLevel * 256) next = (s32)minLevel * 256;
    if (next > (s32)maxLevel * 256) next = (s32)maxLevel * 256;
    *curLevel256 = (u16)next;
    *curLevel = (u8)(next / 256);
    if (*remainingTime && *remainingTime != 0xFFFF) (*remainingTime)--;
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
    if (run_live_timer() >= 0) return 62;
    if (!g_zcl_onOffAttrs.onOff || g_zcl_levelAttrs.curLevel != 123 || onoff_count != 1) return 63;
    return 0;
}

static int test_fade_to_minimum_turns_off_only_at_the_end(void)
{
    reset_sim(200, TRUE);
    if (move_to_over(ZCL_CMD_LEVEL_MOVE_TO_LEVEL_WITH_ON_OFF, ZCL_LEVEL_ATTR_MIN_LEVEL, 2) != ZCL_STA_SUCCESS) return 70;
    if (!g_zcl_onOffAttrs.onOff || g_zcl_levelAttrs.curLevel == ZCL_LEVEL_ATTR_MIN_LEVEL ||
        onoff_count != 0 || !sim_timer.live) return 71;
    if (run_live_timer() >= 0) return 72;
    if (g_zcl_onOffAttrs.onOff || g_zcl_levelAttrs.curLevel != ZCL_LEVEL_ATTR_MIN_LEVEL ||
        onoff_count != 1 || last_onoff != ZCL_CMD_ONOFF_OFF) return 73;
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

    printf("all level/onoff scenarios passed (real zcl_levelCb.c)\n");
    return 0;

failed:
    fprintf(stderr, "level/onoff test failed: %d\n", result);
    return result;
}
