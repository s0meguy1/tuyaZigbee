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

	if(!s_armed){
		/* Factory-new/pairing: nothing to return to. Keep the baseline fresh
		 * so arming later never inherits a stale no-progress stretch. */
		s_lastProgress = s_progress;
		s_noProgress   = 0;
		return MOES_LIVENESS_SAMPLE_MS * 1000;
	}

	if(s_progress != s_lastProgress){
		s_lastProgress = s_progress;
		s_noProgress   = 0;
	}else if(++s_noProgress >= MOES_LIVENESS_PROGRESS_RESET_TICKS){
		/* Wedged regardless of joined state. Unmarked reset (see above). */
		SYSTEM_RESET();
		s_noProgress = 0;   /* unreachable guard for a port where reset returns */
	}

	/* drv_timer.c re-arms the hw timer from this return value, which is in
	 * MICROSECONDS (drv_timer.c:171 multiplies it by TIMER_TICK_1US_GET
	 * again). The redesign paper's pseudo returned
	 * "MOES_LIVENESS_SAMPLE_MS * TIMER_TICK_1US_GET(...) / 1000", which would
	 * be ~48 us on a 48 MHz part - a spin. Corrected here to 1 s. */
	return MOES_LIVENESS_SAMPLE_MS * 1000;
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
		drv_hwTmr_set(TIMER_IDX_0, MOES_LIVENESS_SAMPLE_MS * 1000,
		              moes_livenessHwSampleCb, NULL);
		s_hwArmed = TRUE;
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
