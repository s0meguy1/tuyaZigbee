/* Functional host test for the real light/zcl_onOffCb.c implementation. */
#include <stdio.h>
#include <string.h>

#include "onoff_shim/tl_common.h"
#include "onoff_shim/zb_api.h"
#include "onoff_shim/zcl_include.h"
#include "../../light/tuyaLight.h"

zcl_onOffAttr_t g_zcl_onOffAttrs;

static ev_timer_event_t sim_timer;
static int schedule_count;
static int cancel_count;
static int fresh_count;

ev_timer_event_t *host_timerSchedule(ev_timer_callback_t cb, void *arg, u32 interval)
{
    schedule_count++;
    sim_timer.cb = cb;
    sim_timer.data = arg;
    sim_timer.interval = interval;
    sim_timer.live = TRUE;
    return &sim_timer;
}

void host_timerCancel(ev_timer_event_t **event)
{
    if (event && *event) {
        cancel_count++;
        (*event)->live = FALSE;
        *event = NULL;
    }
}

void light_fresh(void)
{
    fresh_count++;
}

void hwLight_onOffUpdate(u8 onOff)
{
    (void)onOff;
}

static void reset_sim(void)
{
    memset(&g_zcl_onOffAttrs, 0, sizeof(g_zcl_onOffAttrs));
    memset(&sim_timer, 0, sizeof(sim_timer));
    schedule_count = 0;
    cancel_count = 0;
    fresh_count = 0;
}

static status_t send_command(u8 command, void *payload)
{
    zclIncomingAddrInfo_t address = { .dstEp = TUYA_LIGHT_ENDPOINT };
    return tuyaLight_onOffCb(&address, command, payload);
}

static int run_live_timer(void)
{
    s32 result;
    if (!sim_timer.live || !sim_timer.cb) {
        return 99;
    }
    result = sim_timer.cb(sim_timer.data);
    if (result < 0) {
        sim_timer.live = FALSE;
    }
    return (int)result;
}

static int test_plain_on_cancels_stale_transaction(void)
{
    zcl_onoff_onWithTimeOffCmd_t timed = { .onTime = 5, .offWaitTime = 3 };

    reset_sim();
    if (send_command(ZCL_CMD_ON_WITH_TIMED_OFF, &timed) != ZCL_STA_SUCCESS) return 10;
    if (!g_zcl_onOffAttrs.onOff || g_zcl_onOffAttrs.onTime != 5 ||
        g_zcl_onOffAttrs.offWaitTime != 3 || !sim_timer.live ||
        sim_timer.interval != 100 || schedule_count != 1) return 11;

    if (send_command(ZCL_CMD_ONOFF_ON, NULL) != ZCL_STA_SUCCESS) return 12;
    if (!g_zcl_onOffAttrs.onOff || g_zcl_onOffAttrs.onTime ||
        g_zcl_onOffAttrs.offWaitTime || sim_timer.live || cancel_count != 1 ||
        fresh_count != 2) return 13;
    return 0;
}

static int test_timed_off_keeps_its_fresh_values_and_expires(void)
{
    zcl_onoff_onWithTimeOffCmd_t timed = { .onTime = 1, .offWaitTime = 3 };

    reset_sim();
    g_zcl_onOffAttrs.onTime = 1; /* max(old, new) semantics are still live. */
    if (send_command(ZCL_CMD_ON_WITH_TIMED_OFF, &timed) != ZCL_STA_SUCCESS) return 20;
    if (!g_zcl_onOffAttrs.onOff || g_zcl_onOffAttrs.onTime != 1 ||
        g_zcl_onOffAttrs.offWaitTime != 3 || !sim_timer.live ||
        schedule_count != 1 || cancel_count != 0) return 21;

    /* The real static callback turns the light off and retires the event when
     * OnTime reaches zero; it must not be mistaken for a manually-issued On. */
    if (run_live_timer() >= 0) return 22;
    if (g_zcl_onOffAttrs.onOff || g_zcl_onOffAttrs.onTime ||
        g_zcl_onOffAttrs.offWaitTime || sim_timer.live || fresh_count != 2) return 23;
    return 0;
}

int main(void)
{
    int result = test_plain_on_cancels_stale_transaction();
    if (result) {
        fprintf(stderr, "plain-On timed-off test failed: %d\n", result);
        return result;
    }
    result = test_timed_off_keeps_its_fresh_values_and_expires();
    if (result) {
        fprintf(stderr, "On-With-Timed-Off test failed: %d\n", result);
        return result;
    }
    printf("all onoff scenarios passed (real zcl_onOffCb.c)\n");
    return 0;
}
