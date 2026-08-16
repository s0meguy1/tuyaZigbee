/* Host-test shim: the minimum of the Telink SDK that light/moes_rescue.c
 * actually uses, so the real source file can be compiled and exercised on a
 * PC. See tools/rescue_hosttest/README.md. Not part of any firmware build. */
#pragma once
#include <stdint.h>
#include <string.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t  s32;
typedef uint8_t  bool;
#define TRUE  1
#define FALSE 0

typedef enum { NV_SUCC = 0, NV_ITEM_NOT_FOUND = 3 } nv_sts_t;
#define NV_MODULE_APP 6

typedef struct ev_timer_event_s ev_timer_event_t;
typedef s32 (*ev_timer_callback_t)(void *);

/* --- minimal drv_hwTmr surface (see proj/drivers/drv_timer.h) ---
 * The firmware liveness monitor now also uses a hardware-timer IRQ sampler.
 * The shim models it as a second, independently-tickable timer list; the test
 * driver implements drv_hwTmr_init/drv_hwTmr_set and the hw tick primitive. */
typedef int (*timerCb_t)(void *p);

typedef enum hw_timer_sts_e {
	HW_TIMER_SUCC      = 0,
	HW_TIMER_IS_RUNNING = 1,
	HW_TIMER_INVALID,
} hw_timer_sts_t;

#define TIMER_IDX_0     0
#define TIMER_IDX_1     1
#define TIMER_IDX_2     2
#define TIMER_IDX_3     3
#define TIMER_NUM       4
#define TIMER_MODE_SCLK 0

/* Ticks-per-us on the 48 MHz part. Only used for shim arithmetic, never by the
 * firmware units under test (they return a hw-timer period in microseconds). */
#define H_TIMER_CLOCK_1US     48
#define TIMER_TICK_1US_GET(i) H_TIMER_CLOCK_1US

void drv_hwTmr_init(u8 tmrIdx, u8 mode);
hw_timer_sts_t drv_hwTmr_set(u8 tmrIdx, u32 t_us, timerCb_t func, void *arg);

/* --- hooks the test driver implements --- */
nv_sts_t nv_flashReadNew(u8 single, u8 id, u8 itemId, u16 len, u8 *buf);
nv_sts_t nv_flashWriteNew(u8 single, u8 id, u8 itemId, u16 len, u8 *buf);
bool     zb_isDeviceJoinedNwk(void);
ev_timer_event_t *host_timerSchedule(ev_timer_callback_t cb, void *arg, u32 ms);
void     host_systemReset(void);

#define TL_ZB_TIMER_SCHEDULE(cb, arg, ms)  host_timerSchedule((cb), (arg), (ms))
#define SYSTEM_RESET()                     host_systemReset()
