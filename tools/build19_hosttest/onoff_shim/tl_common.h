/* Minimal host-only SDK surface for compiling the real zcl_onOffCb.c. */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t s32;
typedef uint8_t bool;
typedef int nv_sts_t;

#define TRUE 1
#define FALSE 0

typedef struct ev_timer_event_s ev_timer_event_t;
typedef s32 (*ev_timer_callback_t)(void *);

struct ev_timer_event_s {
    ev_timer_callback_t cb;
    void *data;
    u32 interval;
    bool live;
};

ev_timer_event_t *host_timerSchedule(ev_timer_callback_t cb, void *arg, u32 interval);
void host_timerCancel(ev_timer_event_t **event);

#define TL_ZB_TIMER_SCHEDULE(cb, arg, interval) host_timerSchedule((cb), (arg), (interval))
#define TL_ZB_TIMER_CANCEL(event) host_timerCancel((event))
