/********************************************************************************************************
 * @file    moes_liveness.c
 *
 * @brief   See moes_liveness.h.
 *
 * @date    2026
 *******************************************************************************************************/

#if (__PROJECT_TL_DIMMABLE_LIGHT__)

#include "../common/comm_cfg.h"
#include "tl_common.h"
#include "zb_api.h"
#include "zcl_include.h"
#include "moes_liveness.h"

#if (MOES_TS0505B && MOES_LIVENESS_ENABLE)

/* 60 s of no progress at the 1 s sample rate. */
#define MOES_LIVENESS_PROGRESS_RESET_TICKS   (MOES_LIVENESS_PROGRESS_RESET_S * 1000U / MOES_LIVENESS_SAMPLE_MS)

/* The sample period in hardware-timer ticks, for the explicit one-shot re-arm
 * in moes_livenessHwSampleCb. TIMER_IDX_0 counts the 48 MHz system clock, so
 * MOES_LIVENESS_SAMPLE_MS * 1000 us * 48 ticks/us = 48 000 000 ticks = 1 s.
 * (TIMER_TICK_1US_GET is the SDK's µs->tick scale from drv_timer.h.) */
#define MOES_LIVENESS_SAMPLE_TICKS   ((u32)(MOES_LIVENESS_SAMPLE_MS) * 1000UL * TIMER_TICK_1US_GET(TIMER_IDX_0))

/* volatile: written in task context (the progress ticker, the BDB arm/activity
 * hooks) and read in the hardware-timer IRQ. The compiler must not cache them
 * across the IRQ callback. */
static volatile bool s_armed    = FALSE;
static volatile u8   s_progress = 0;

/* IRQ-only state. The only writer is moes_livenessHwSampleCb, so these do not
 * need volatile - the IRQ both writes and reads them. */
static u8 s_lastProgress = 0;
static u8 s_noProgress   = 0;

/* Task-only state. The only writers are the ensure/arm functions, which all run
 * in task context. */
static bool s_tickerRunning = FALSE;
static bool s_hwArmed       = FALSE;

/*********************************************************************
 * @fn      moes_livenessProgressTickerCb
 *
 * @brief   The task-context heartbeat. Deliberately scheduled on the
 *          cooperative ev_timer list: it advances only while ev_main is
 *          returning and ev_timer callbacks are actually being serviced.
 *          When the stack wedges this ticker starves, and that is precisely
 *          the signal the IRQ sampler watches.
 */
static s32 moes_livenessProgressTickerCb(void *arg)
{
	(void)arg;

	/* u8 wrap is fine (liveness_redesign.md F6): at 1 Hz the wrap is 256 s,
	 * far above the 60 s fuse, and the sampler compares equality. */
	s_progress++;

	return 0;   /* ev_timer re-arms with the same MOES_LIVENESS_PROGRESS_TICK_MS period */
}

/*********************************************************************
 * @fn      moes_livenessHwSampleCb
 *
 * @brief   The hardware-timer IRQ sampler (drv_hwTmr TIMER_IDX_0). Runs
 *          independently of ev_main, so it keeps firing while the
 *          cooperative scheduler is wedged.
 *
 *          Deliberately does NOT call moes_resetSkipNextBoot(): that helper
 *          writes NV, and an NV write from a timer IRQ is not safe
 *          (drv_flash.c takes IRQ-off critical sections and is not
 *          re-entrant). See moes_liveness.h for why the missing mark cannot
 *          accumulate the factory-reset power count.
 */
static s32 moes_livenessHwSampleCb(void *arg)
{
	(void)arg;

	if(s_armed){
		if(s_progress != s_lastProgress){
			s_lastProgress = s_progress;
			s_noProgress   = 0;
		}else if(++s_noProgress >= MOES_LIVENESS_PROGRESS_RESET_TICKS){
			/* Wedged regardless of joined state. Unmarked reset (see above). */
			SYSTEM_RESET();
			s_noProgress = 0;   /* unreachable guard for a port where reset returns */
		}
	}else{
		/* Factory-new/pairing: nothing to return to. Keep the baseline fresh
		 * so arming later never inherits a stale no-progress stretch. */
		s_lastProgress = s_progress;
		s_noProgress   = 0;
	}

	/* Build 10 cadence fix — bughunt/fuse_no_fire_b09.md §1d/§5.
	 *
	 * Build 09 let drv_hwTmr_irq_process() re-arm Timer0 from this callback's
	 * return value. For TIMER_IDX_0/1/2 the SDK's re-arm is hwTimerSet():
	 *
	 *     timer_set_init_tick(tmrIdx, 0);   // write reg_tmr_tick (0x630)
	 *     timer_set_cap_tick(tmrIdx, tick); // write reg_tmr_capt (0x624)
	 *
	 * i.e. it writes the init and capture registers while the free-running
	 * 32-bit counter is STILL RUNNING. TIMER_MODE_SCLK is documented as "free
	 * run from 0 to 0xffffffff" (drv_timer.h), and there is no production
	 * precedent in this firmware for a periodic Timer0/1/2 re-arm: the MAC
	 * CSMA timer is TIMER_IDX_3, whose re-arm is the different, absolute
	 * stimer_set_irq_capture(tick + clock_time()) path. The build-09 field
	 * result (fuse silent inside a 10-minute observation) is consistent with
	 * reg_tmr_tick NOT resetting a running counter, which turns the intended
	 * 1 s cadence into the 32-bit wrap cadence (~89 s at 48 MHz) and the
	 * 60-sample fuse into ~89 minutes.
	 *
	 * The INITIAL arm is not suspect: drv_hwTmr_set() -> hwTmr_setAbs() writes
	 * the same init/capture and then timer_start(), and a freshly enabled B85
	 * timer loads its init tick. The re-arm path is broken only because it
	 * omits that stop/start, so the counter is never re-loaded. The fix is to
	 * make every sample an explicit one-shot: stop, reload init+capture, and
	 * start again - exactly the proven initial-arm sequence plus the stop that
	 * a re-arm of an already-running free-run timer requires.
	 *
	 * We return 0 so drv_hwTmr_irq_process() keeps the driver state as TIMER_WTO
	 * and does not replace our fresh expiry. It still calls hwTimerSet() once
	 * after we return; that extra init_tick/capt write is harmless because the
	 * counter has already been reloaded by the stop/start above, and the capture
	 * value it writes is the same 48M ticks. The next expiry is driven by the
	 * counter we just restarted, not by the suspect return-value re-arm. */
	timer_stop(TIMER_IDX_0);
	timer_set_init_tick(TIMER_IDX_0, 0);
	timer_set_cap_tick(TIMER_IDX_0, MOES_LIVENESS_SAMPLE_TICKS);
	timer_start(TIMER_IDX_0);

	return 0;
}

/*********************************************************************
 * @fn      moes_livenessEnsureTimer
 */
static void moes_livenessEnsureTimer(void)
{
	if(!s_tickerRunning){
		s_tickerRunning =
			(TL_ZB_TIMER_SCHEDULE(moes_livenessProgressTickerCb, NULL,
			                      MOES_LIVENESS_PROGRESS_TICK_MS) != NULL);
		/* A NULL return means the 24-entry ev_timer pool was full, so the
		 * ticker is not scheduled and s_progress can never advance. That is
		 * the CORRECT outcome, not a silent degradation: the IRQ sampler below
		 * observes zero progress and resets after the fuse. (On the real SDK a
		 * full pool also posts a timer exception -> SYSTEM_RESET even sooner;
		 * the host test models only the slower fuse-reset bound.) */
	}

	if(!s_hwArmed){
		drv_hwTmr_init(TIMER_IDX_0, TIMER_MODE_SCLK);
		/* Only latch "armed" when the set actually took. If the driver says the
		 * index is already running (or invalid) this boot stays un-armed and the
		 * next arm call retries; that failure is invisible in the field, but at
		 * least we do not pretend the IRQ sampler is running when it is not. */
		hw_timer_sts_t st = drv_hwTmr_set(TIMER_IDX_0, MOES_LIVENESS_SAMPLE_MS * 1000,
		                                  moes_livenessHwSampleCb, NULL);
		s_hwArmed = (st == HW_TIMER_SUCC);
	}
}

/*********************************************************************
 * @fn      moes_livenessBootedOnNetwork
 */
void moes_livenessBootedOnNetwork(void)
{
	s_armed = TRUE;
	moes_livenessEnsureTimer();
}

/*********************************************************************
 * @fn      moes_livenessJoined
 */
void moes_livenessJoined(void)
{
	s_armed    = TRUE;
	s_progress = 0;
	moes_livenessEnsureTimer();
}

/*********************************************************************
 * @fn      moes_livenessStackActivity
 */
void moes_livenessStackActivity(void)
{
	s_progress++;
}

/*********************************************************************
 * @fn      moes_livenessProgressCount
 */
u8 moes_livenessProgressCount(void)
{
	return (u8)s_progress;
}

#else  /* !(MOES_TS0505B && MOES_LIVENESS_ENABLE) */

void moes_livenessBootedOnNetwork(void){}
void moes_livenessJoined(void){}
void moes_livenessStackActivity(void){}
u8   moes_livenessProgressCount(void){ return 0; }

#endif

#endif  /* __PROJECT_TL_DIMMABLE_LIGHT__ */
