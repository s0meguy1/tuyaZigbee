/********************************************************************************************************
 * @file    moes_rescue.c
 *
 * @brief   See moes_rescue.h.
 *
 * @date    2026
 *******************************************************************************************************/

#if (__PROJECT_TL_DIMMABLE_LIGHT__)

#include "../common/comm_cfg.h"
#include "tl_common.h"
#include "zb_api.h"
#include "zcl_include.h"
#include "moes_liveness.h"
#include "moes_rescue.h"

#if (MOES_TS0505B && MOES_RESCUE_ENABLE)

/* Our own NV items in NV_MODULE_APP. Item ids are per-module and the SDK's
 * nv_item_t enum uses 0x01..0x2C plus 0x80; 0x70 is moes_flashcfg.c's
 * reset-skip flag and 0x71 is the probation count. Neither 0x00 (reserved)
 * nor 0xFF (ITEM_FIELD_IDLE) may be used; nothing else is range-checked. */
#define MOES_NV_ITEM_BOOT_PROBATION     0x71
#define MOES_NV_ITEM_BOOT_YOUNG         0x72

/* How long a boot must survive before it counts as "grew up". Any
 * watchdog-bounded reset loop dies inside the boot interval (b17 caps it
 * at 30 s), so 60 s catches every loop class while clearing on any
 * genuinely functional boot - including a routine power-on after an
 * outage, which runs far longer than a minute. */
#define MOES_BOOT_GROWNUP_SECONDS       60

static bool s_rescue    = FALSE;
static u8   s_failCnt   = 0;
static u8   s_minsLeft  = 0;
static ev_timer_event_t *s_stableTimer = NULL;
static ev_timer_event_t *s_grownupTimer = NULL;

/* Build-09 stable-clear gate (liveness_redesign.md S3). s_confirm marks the
 * one-minute confirmation window after the countdown reaches zero; a clear is
 * committed only on the tick AFTER that window, so a wedge landing at exactly
 * minute 20 cannot erase the probation history. s_stableLastProgress is the
 * progress counter seen on the previous stable tick; a joined-but-wedged boot
 * stalls progress and is therefore never allowed to decrement or clear. */
static bool s_confirm            = FALSE;
static u8   s_stableLastProgress = 0;

static void moes_probationWrite(u8 v)
{
	nv_flashWriteNew(1, NV_MODULE_APP, MOES_NV_ITEM_BOOT_PROBATION, 1, &v);
}

static void moes_youngWrite(u8 v)
{
	nv_flashWriteNew(1, NV_MODULE_APP, MOES_NV_ITEM_BOOT_YOUNG, 1, &v);
}

/*********************************************************************
 * @fn      moes_rescueBootCheck
 */
void moes_rescueBootCheck(void)
{
	u8 cnt = 0;
	u8 prevYoung = 0;

	/* A fresh device, or one whose NV was just factory-reset, returns
	 * NV_ITEM_NOT_FOUND and leaves the value untouched. Treat that as
	 * zero explicitly rather than relying on the callee not writing the
	 * buffer. */
	if(nv_flashReadNew(1, NV_MODULE_APP, MOES_NV_ITEM_BOOT_PROBATION, 1, &cnt) != NV_SUCC){
		cnt = 0;
	}
	if(nv_flashReadNew(1, NV_MODULE_APP, MOES_NV_ITEM_BOOT_YOUNG, 1, &prevYoung) != NV_SUCC){
		prevYoung = 0;
	}

	/* Clamp, for the same reason factory_reset.c does: never let an
	 * out-of-range NV byte drive control flow. The direction is safe
	 * here - a garbage high value can only set an advisory flag that
	 * asks for an early OTA query. It can never suppress it or trigger
	 * a reset. */
	if(cnt > MOES_RESCUE_FAIL_THRESHOLD){
		cnt = MOES_RESCUE_FAIL_THRESHOLD;
	}
	s_failCnt = cnt;

#if defined(MOES_RESCUE_FORCE)
	s_rescue = TRUE;
#else
	if(prevYoung){
		/* The previous boot died before its grown-up timer fired: this is
		 * a rapid-cycling storm. Build probation (writing only while below
		 * the threshold, so a latched storm stops writing), and mark this
		 * boot young so the storm keeps counting. */
		if(cnt < MOES_RESCUE_FAIL_THRESHOLD){
			s_failCnt = cnt + 1;
			moes_probationWrite(s_failCnt);
		}
	}else{
		/* The previous boot grew up: this is a routine power event. Any
		 * probation history is stale by definition - a firmware that just
		 * ran for a full minute is recoverable - so wipe it. Written only
		 * when there is something to wipe, so healthy boots do zero NV
		 * writes here. */
		if(cnt){
			s_failCnt = 0;
			moes_probationWrite(0);
		}
	}

	if(s_failCnt >= MOES_RESCUE_FAIL_THRESHOLD){
		s_rescue = TRUE;
	}
#endif

	/* Mark this boot young. The grown-up timer below clears the marker; a
	 * boot that dies first leaves it set, which is exactly the signal the
	 * NEXT boot uses to recognise the storm. */
	/* Build 24: skip the write when the marker is ALREADY 1. It was
	 * unconditional, so a reset loop wrote NV on every boot - ~240 writes an
	 * hour at the 15 s period measured on 2026-08-30, thousands before anyone
	 * noticed, each one consuming a fresh item record (nv_flashWriteNew never
	 * compares the old value) and forcing periodic sector migration and erase.
	 * tuyaLight.c refuses to write NV from the exception handler for exactly
	 * this reason and this path quietly did it anyway. In a storm prevYoung is
	 * already 1, so the steady-state cost is now zero writes. */
	if(prevYoung != 1){
		moes_youngWrite(1);
	}
	if(s_grownupTimer){
		TL_ZB_TIMER_CANCEL(&s_grownupTimer);
	}
	s_grownupTimer = TL_ZB_TIMER_SCHEDULE(moes_rescueGrownupCb, NULL, MOES_BOOT_GROWNUP_SECONDS * 1000);
	/* TL_ZB_TIMER_SCHEDULE can return NULL if the 24-entry pool is full.
	 * That fails safe-but-noisy: the marker stays 1 and the next boot
	 * counts itself into a storm that never happened. The stable clock
	 * still clears the count during long healthy runs, and the flag is
	 * advisory only, so the cost is one extra OTA query cadence and a
	 * boot blink - never functionality. */
}

/*********************************************************************
 * @fn      moes_rescueGrownupCb
 */
s32 moes_rescueGrownupCb(void *arg)
{
	(void)arg;
	moes_youngWrite(0);
	s_grownupTimer = NULL;
	return -1;
}

/*********************************************************************
 * @fn      moes_rescueActive
 */
bool moes_rescueActive(void)
{
	return s_rescue;
}

/*********************************************************************
 * @fn      moes_rescueFailCount
 */
u8 moes_rescueFailCount(void)
{
	return s_failCnt;
}

/*********************************************************************
 * @fn      moes_rescueClear
 */
void moes_rescueClear(void)
{
	if(s_failCnt){
		s_failCnt = 0;
		moes_probationWrite(0);
	}
}

/*********************************************************************
 * @fn      moes_rescueStableTimerCb
 *
 * @brief   One tick per minute. Counting minutes in RAM rather than asking
 *          for a single 20-minute TL_ZB_TIMER keeps every arithmetic value
 *          small and lets us re-check "still joined?" on every tick - the
 *          requirement is up *and joined*, not merely up.
 *
 *          Build 09 adds: the scheduler must still be making progress. A
 *          class-2 wedge leaves zb_isDeviceJoinedNwk() TRUE, so the joined
 *          bit alone cannot be trusted. The clear needs joined AND
 *          continuous progress, plus one confirmation minute after the
 *          countdown reaches zero (liveness_redesign.md S3).
 */
static s32 moes_rescueStableTimerCb(void *arg)
{
	(void)arg;

	u8 p = moes_livenessProgressCount();

	if(!zb_isDeviceJoinedNwk() || p == s_stableLastProgress){
		/* Fell off the network, or the scheduler stopped making progress.
		 * Restart, do not pause, and never commit a clear from this branch.
		 * The +1 keeps the effective healthy window conservative: after any
		 * stall the full window must be re-earned before a clear can commit. */
		s_minsLeft           = MOES_RESCUE_STABLE_MINUTES + 1;
		s_confirm            = FALSE;
		s_stableLastProgress = p;
		return 0;
	}

	s_stableLastProgress = p;

	if(s_minsLeft){
		s_minsLeft--;
	}

	if(s_minsLeft == 0 && !s_confirm){
		/* Countdown reached zero: require ONE more minute of joined+progress
		 * before the NV write. This closes the boothang timing race, where the
		 * wedge lands exactly at minute 20. */
		s_confirm = TRUE;
		return 0;
	}

	if(s_minsLeft == 0 && s_confirm){
		moes_rescueClear();
		s_stableTimer = NULL;
		return -1;
	}

	return 0;
}

/*********************************************************************
 * @fn      moes_rescueStableTimerStart
 */
void moes_rescueStableTimerStart(void)
{
	if(s_stableTimer){
		return;                 /* already counting */
	}
	if(s_failCnt == 0){
		return;                 /* nothing to clear, do not burn a timer slot */
	}

	s_minsLeft = MOES_RESCUE_STABLE_MINUTES;
	s_stableTimer = TL_ZB_TIMER_SCHEDULE(moes_rescueStableTimerCb, NULL, 60 * 1000);
	/* TL_ZB_TIMER_SCHEDULE can return NULL if the 24-entry pool is full. That
	 * is not worth reacting to: the only consequence is that this boot does
	 * not clear its counter, and the next boot tries again. */
}

#else  /* !(MOES_TS0505B && MOES_RESCUE_ENABLE) */

void moes_rescueBootCheck(void){}
bool moes_rescueActive(void){ return FALSE; }
void moes_rescueStableTimerStart(void){}
void moes_rescueClear(void){}
u8   moes_rescueFailCount(void){ return 0; }

#endif

#endif  /* __PROJECT_TL_DIMMABLE_LIGHT__ */
