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

/* Our own NV item in NV_MODULE_APP. Item ids are per-module and the SDK's
 * nv_item_t enum uses 0x01..0x2C plus 0x80; 0x70 is already taken by
 * moes_flashcfg.c's reset-skip flag. Neither 0x00 (reserved) nor 0xFF
 * (ITEM_FIELD_IDLE) may be used; nothing else is range-checked. */
#define MOES_NV_ITEM_BOOT_PROBATION     0x71

static bool s_rescue    = FALSE;
static u8   s_failCnt   = 0;
static u8   s_minsLeft  = 0;
static ev_timer_event_t *s_stableTimer = NULL;

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

/*********************************************************************
 * @fn      moes_rescueBootCheck
 */
void moes_rescueBootCheck(void)
{
	u8 cnt = 0;

	/* A fresh device, or one whose NV was just factory-reset, returns
	 * NV_ITEM_NOT_FOUND and leaves cnt untouched. Treat that as zero
	 * explicitly rather than relying on the callee not writing the buffer. */
	if(nv_flashReadNew(1, NV_MODULE_APP, MOES_NV_ITEM_BOOT_PROBATION, 1, &cnt) != NV_SUCC){
		cnt = 0;
	}

	/* Clamp, for the same reason factory_reset.c does: never let an
	 * out-of-range NV byte drive control flow. Note the direction is safe
	 * here - a garbage high value can only cause an unnecessary rescue boot,
	 * which self-heals after one stable run. It can never suppress rescue
	 * mode or trigger a reset. */
	if(cnt > MOES_RESCUE_FAIL_THRESHOLD){
		cnt = MOES_RESCUE_FAIL_THRESHOLD;
	}
	s_failCnt = cnt;

#if defined(MOES_RESCUE_FORCE)
	s_rescue = TRUE;
	return;
#else
	if(cnt >= MOES_RESCUE_FAIL_THRESHOLD){
		/* Latch. Deliberately no NV write: a light that keeps resetting must
		 * not keep erasing flash sectors while it does it. The counter stays
		 * at the threshold until something clears it. */
		s_rescue = TRUE;
		return;
	}

	s_failCnt = cnt + 1;
	moes_probationWrite(s_failCnt);
#endif
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
 *          Build 09 adds a second requirement: the scheduler must still be
 *          making progress. A class-2 wedge leaves zb_isDeviceJoinedNwk()
 *          TRUE, so the joined bit alone cannot be trusted. The clear now
 *          needs joined AND continuous progress, plus one confirmation minute
 *          after the countdown reaches zero (liveness_redesign.md S3).
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
