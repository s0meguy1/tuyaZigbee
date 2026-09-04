/********************************************************************************************************
 * @file    zcl_levelCb.c
 *
 * @brief   This is the source file for zcl_levelCb
 *
 * @author  Zigbee Group
 * @date    2021
 *
 * @par     Copyright (c) 2021, Telink Semiconductor (Shanghai) Co., Ltd. ("TELINK")
 *			All rights reserved.
 *
 *          Licensed under the Apache License, Version 2.0 (the "License");
 *          you may not use this file except in compliance with the License.
 *          You may obtain a copy of the License at
 *
 *              http://www.apache.org/licenses/LICENSE-2.0
 *
 *          Unless required by applicable law or agreed to in writing, software
 *          distributed under the License is distributed on an "AS IS" BASIS,
 *          WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *          See the License for the specific language governing permissions and
 *          limitations under the License.
 *
 *******************************************************************************************************/

#if (__PROJECT_TL_DIMMABLE_LIGHT__)
/**********************************************************************
 * INCLUDES
 */
#include "tl_common.h"
#include "zb_api.h"
#include "zcl_include.h"
#include "tuyaLight.h"
#include "tuyaLightCtrl.h"

#ifdef ZCL_LEVEL_CTRL

/**********************************************************************
 * LOCAL CONSTANTS
 */
/* Build 38. See MOES_RAMP_TICK_MS in tuyaLightCtrl.h for why this is 20 ms
 * and not the decisecond it used to be. */
#define ZCL_LEVEL_CHANGE_INTERVAL		MOES_RAMP_TICK_MS

/**********************************************************************
 * TYPEDEFS
 */
typedef struct{
	s32 stepLevel256;
	u16	currentLevel256;
	/* Build 38. The ramp's own tick counter.
	 *
	 * pLevel->remainingTime used to be both the ZCL attribute and the loop
	 * counter, which welded the render rate to the attribute's decisecond
	 * unit. These two are now separate: this counts 20 ms ticks and drives the
	 * ramp, and remainingTime is derived from it so the attribute keeps its
	 * ZCL meaning. */
	u16 stepsRemaining;
	/* The level the command asked for, so the ramp can land exactly on it. */
	u8	targetLevel;
	u8	hasTarget;
	u8	withOnOff;
	/* Whether this command has already issued its with-on-off Off. */
	u8	offSent;
}zcl_levelInfo_t;

/**********************************************************************
 * LOCAL VARIABLES
 */
static zcl_levelInfo_t levelInfo = {
	.stepLevel256 		= 0,
	.currentLevel256	= 0,
	.stepsRemaining		= 0,
	.targetLevel		= 0,
	.hasTarget			= 0,
	.withOnOff			= 0,
	.offSent			= 0,
};

static ev_timer_event_t *levelTimerEvt = NULL;

/**********************************************************************
 * FUNCTIONS
 */

/*********************************************************************
 * @fn      tuyaLight_levelSteps
 *
 * @brief   ZCL transition time (deciseconds) -> ramp ticks.
 */
static u16 tuyaLight_levelSteps(u16 transitionTimeDs)
{
	u32 steps;

	/* transitionTime 0 and 0xFFFF have always meant "immediately" here, and
	 * that has always been exactly one tick. Keep it one TICK, not one
	 * decisecond's worth of them: zigbee2mqtt sends transitionTime 0 for an
	 * ordinary brightness set, so turning it into a 100 ms ramp would delay
	 * every plain command - and, with-on-off, the Off that ends a move to the
	 * minimum. */
	if((transitionTimeDs == 0) || (transitionTimeDs == 0xFFFF)){
		return 1;
	}

	steps = (u32)transitionTimeDs * MOES_RAMP_STEPS_PER_DS;

	/* transitionTime is network-reachable and unvalidated, and five ticks per
	 * decisecond overflows a u16 counter well before 0xFFFF deciseconds. Clamp
	 * below 0xFFFF too, which light_applyUpdate() reads as "never expires". */
	if(steps > 0xFFFEu){
		steps = 0xFFFEu;
	}

	return (u16)steps;
}

/*********************************************************************
 * @fn      tuyaLight_levelDs
 *
 * @brief   Ramp ticks -> the ZCL RemainingTime attribute, in deciseconds.
 *          Rounded up, so the attribute only reads 0 once the ramp has
 *          actually finished.
 */
static u16 tuyaLight_levelDs(u16 steps)
{
	if(steps == 0xFFFF){
		return 0xFFFF;
	}

	return (u16)((steps + (MOES_RAMP_STEPS_PER_DS - 1)) / MOES_RAMP_STEPS_PER_DS);
}

/*********************************************************************
 * @fn      tuyaLight_levelWiden
 *
 * @brief   See tuyaLightCtrl.h.
 */
u16 tuyaLight_levelWiden(u8 level)
{
	s32 diff = (s32)levelInfo.currentLevel256 - (((s32)level) << 8);

	if(diff < 0){
		diff = -diff;
	}

	/* Within one level, the accumulator IS this level and carries the fraction
	 * the ramp is currently on - light_applyUpdate() floors on the way down and
	 * rounds on the way up, so either direction can differ from the u8 by just
	 * under one. Beyond that the accumulator is stale, because something wrote
	 * curLevel directly: scene recall, a Tuya datapoint and the effect-stop
	 * path all do. Render the attribute rather than a stale sub-level. */
	if(diff < 256){
		return levelInfo.currentLevel256;
	}

	return ((u16)level) << 8;
}

/*********************************************************************
 * @fn      tuyaLight_levelApply
 *
 * @brief   Advance the ramp by one tick and render it.
 */
static void tuyaLight_levelApply(void)
{
	zcl_levelAttr_t *pLevel = zcl_levelAttrGet();

	/* light_applyUpdate() decrements whatever counter it is given, and calls
	 * light_fresh(). Give it the tick counter rather than the ZCL attribute -
	 * that is the whole of build 38's part A, and it means the shared function
	 * and its host-test stub keep their signatures. */
	light_applyUpdate(&pLevel->curLevel, &levelInfo.currentLevel256, &levelInfo.stepLevel256,
					&levelInfo.stepsRemaining, ZCL_LEVEL_ATTR_MIN_LEVEL, ZCL_LEVEL_ATTR_MAX_LEVEL, FALSE);

	/* Land exactly on the commanded level.
	 *
	 * stepLevel256 is a truncated 8.8 quotient, so the shortfall accumulated
	 * over N ticks approaches N/256 of a level. At 50 ticks that was a fifth of
	 * a level and invisible; at 250 it approaches a whole one, and an up-ramp
	 * of exactly 63 levels over 5 s lands on 63 instead of 64. That is worse
	 * than one level of brightness: a with-on-off fade that stops one short of
	 * the minimum never issues its Off, and leaves the light dimly lit. */
	if(levelInfo.stepsRemaining == 0){
		if(levelInfo.hasTarget && (pLevel->curLevel != levelInfo.targetLevel)){
			pLevel->curLevel = levelInfo.targetLevel;
			levelInfo.currentLevel256 = ((u16)levelInfo.targetLevel) << 8;
			light_fresh();
		}
		levelInfo.hasTarget = FALSE;
	}

	pLevel->remainingTime = tuyaLight_levelDs(levelInfo.stepsRemaining);
}

/*********************************************************************
 * @fn      tuyaLight_levelWithOnOffChk
 *
 * @brief   Issue the with-on-off Off, exactly once per command.
 *
 *          light_applyUpdate() clamps at the minimum and lets the counter run
 *          on, so the ramp can sit there for many ticks. Unguarded at 20 ms
 *          that re-issues Off fifty times a second.
 */
static void tuyaLight_levelWithOnOffChk(void)
{
	zcl_levelAttr_t *pLevel = zcl_levelAttrGet();

	if(levelInfo.withOnOff && !levelInfo.offSent &&
	   (pLevel->curLevel == ZCL_LEVEL_ATTR_MIN_LEVEL)){
		levelInfo.offSent = TRUE;
		tuyaLight_onoff(ZCL_CMD_ONOFF_OFF);
	}
}

/*********************************************************************
 * @fn      tuyaLight_levelInit
 *
 * @brief
 *
 * @param   None
 *
 * @return  None
 */
void tuyaLight_levelInit(void)
{
	zcl_levelAttr_t *pLevel = zcl_levelAttrGet();

	pLevel->remainingTime = 0;

	levelInfo.stepsRemaining = 0;
	levelInfo.hasTarget = FALSE;
	levelInfo.currentLevel256 = (u16)(pLevel->curLevel) << 8;

	/* A zero counter makes light_applyUpdate() snap and render without
	 * stepping, which is what init wants. */
	light_applyUpdate(&pLevel->curLevel, &levelInfo.currentLevel256, &levelInfo.stepLevel256, &levelInfo.stepsRemaining,
					ZCL_LEVEL_ATTR_MIN_LEVEL, ZCL_LEVEL_ATTR_MAX_LEVEL, FALSE);
}

/*********************************************************************
 * @fn      tuyaLight_updateLevel
 *
 * @brief
 *
 * @param   None
 *
 * @return  None
 */
void tuyaLight_updateLevel(void)
{
	zcl_levelAttr_t *pLevel = zcl_levelAttrGet();

	hwLight_levelUpdate(pLevel->curLevel);
}

/*********************************************************************
 * @fn      tuyaLight_levelTimerEvtCb
 *
 * @brief   timer event to process the level command
 *
 * @param	arg
 *
 * @return  0: timer continue on; -1: timer will be canceled
 */
static s32 tuyaLight_levelTimerEvtCb(void * arg)
{
	if(levelInfo.stepsRemaining){
		tuyaLight_levelApply();
	}

	tuyaLight_levelWithOnOffChk();

	if(levelInfo.stepsRemaining){
		return 0;
	}else{
		levelTimerEvt = NULL;
		return -1;
	}
}

/*********************************************************************
 * @fn      tuyaLight_LevelTimerStop
 *
 * @brief   force to stop the level timer
 *
 * @param
 *
 * @return
 */
static void tuyaLight_LevelTimerStop(void)
{
	if(levelTimerEvt){
		TL_ZB_TIMER_CANCEL(&levelTimerEvt);
	}
}

/*********************************************************************
 * @fn      tuyaLight_levelTransitionCancel
 *
 * @brief   See tuyaLightCtrl.h. Drops the pending steps only; the current
 *          level attribute is left exactly where the fade had reached.
 */
void tuyaLight_levelTransitionCancel(void)
{
	zcl_levelAttr_t *pLevel = zcl_levelAttrGet();

	tuyaLight_LevelTimerStop();
	levelInfo.stepLevel256 = 0;
	levelInfo.stepsRemaining = 0;
	levelInfo.hasTarget = FALSE;
	pLevel->remainingTime = 0;
}

/*********************************************************************
 * @fn      tuyaLight_moveToLevelProcess
 *
 * @brief
 *
 * @param	cmdId
 * @param	cmd
 *
 * @return	None
 */
static void tuyaLight_moveToLevelProcess(u8 cmdId, moveToLvl_t *cmd)
{
	zcl_levelAttr_t *pLevel = zcl_levelAttrGet();

	levelInfo.stepsRemaining = tuyaLight_levelSteps(cmd->transitionTime);
	pLevel->remainingTime = tuyaLight_levelDs(levelInfo.stepsRemaining);

	levelInfo.withOnOff = (cmdId == ZCL_CMD_LEVEL_MOVE_TO_LEVEL_WITH_ON_OFF) ? TRUE : FALSE;
	levelInfo.offSent = FALSE;
	levelInfo.targetLevel = cmd->level;
	levelInfo.hasTarget = TRUE;
	levelInfo.currentLevel256 = (u16)(pLevel->curLevel) << 8;
	levelInfo.stepLevel256 = ((s32)(cmd->level - pLevel->curLevel)) << 8;
	levelInfo.stepLevel256 /= (s32)levelInfo.stepsRemaining;

	tuyaLight_levelApply();

	if(levelInfo.withOnOff){
		/* Move-To-Level-With-On/Off expresses the requested final power state
		 * through the TARGET level, not through the direction of travel. The
		 * old step-sign test left an OFF light dark whenever Zigbee2MQTT asked
		 * it to turn on at the same or a lower non-minimum brightness. That is
		 * the normal state-only ON path because the converter reuses its cached
		 * level. Turn on for every non-minimum target; the transition timer
		 * still turns off only when a move actually reaches minimum. */
		if(cmd->level > ZCL_LEVEL_ATTR_MIN_LEVEL){
			tuyaLight_onoff(ZCL_CMD_ONOFF_ON);
		}else if(pLevel->curLevel == ZCL_LEVEL_ATTR_MIN_LEVEL){
			levelInfo.offSent = TRUE;
			tuyaLight_onoff(ZCL_CMD_ONOFF_OFF);
		}
	}

	if(levelInfo.stepsRemaining){
		tuyaLight_LevelTimerStop();
		levelTimerEvt = TL_ZB_TIMER_SCHEDULE(tuyaLight_levelTimerEvtCb, NULL, ZCL_LEVEL_CHANGE_INTERVAL);
	}else{
		tuyaLight_LevelTimerStop();
	}
}

/*********************************************************************
 * @fn      tuyaLight_moveProcess
 *
 * @brief
 *
 * @param	cmdId
 * @param	cmd
 *
 * @return	None
 */
static void tuyaLight_moveProcess(u8 cmdId, move_t *cmd)
{
	zcl_levelAttr_t *pLevel = zcl_levelAttrGet();

	levelInfo.withOnOff = (cmdId == ZCL_CMD_LEVEL_MOVE_WITH_ON_OFF) ? TRUE : FALSE;
	levelInfo.offSent = FALSE;
	levelInfo.currentLevel256 = (u16)(pLevel->curLevel) << 8;

	u32 rate = (u32)cmd->rate * 100;
	u8 newLevel;
	u8 deltaLevel;
	if(cmd->moveMode == LEVEL_MOVE_UP){
		newLevel = ZCL_LEVEL_ATTR_MAX_LEVEL;
		deltaLevel = ZCL_LEVEL_ATTR_MAX_LEVEL - pLevel->curLevel;
	}else{
		newLevel = ZCL_LEVEL_ATTR_MIN_LEVEL;
		deltaLevel = pLevel->curLevel - ZCL_LEVEL_ATTR_MIN_LEVEL;
	}

	/* cmd->rate is network-reachable and unvalidated. A rate of 0 makes the
	 * divisor zero; on this part the integer division routine polls the
	 * hardware divider's status bit and never checks for a zero divisor, so a
	 * zero rate either hangs the CPU in the busy-wait or (if the divider does
	 * complete) returns a garbage 0xFFFF that leaves a perpetual 100 ms level
	 * timer doing nothing. Treat a zero rate as a single-tick move instead. */
	if(rate == 0){
		/* One tick, exactly as before: an immediate move to the end stop. */
		levelInfo.stepsRemaining = 1;
	}else{
		u32 transitionTimeDs = ((u32)deltaLevel * 1000) / rate;

		/* tuyaLight_levelSteps() turns a zero decisecond result into one tick,
		 * which is what the old "if(remainingTime == 0) remainingTime = 1"
		 * did. deltaLevel is at most 253 so this cannot approach the clamp,
		 * but bound it before the u16 cast regardless. */
		if(transitionTimeDs > 0xFFFEu){
			transitionTimeDs = 0xFFFEu;
		}
		levelInfo.stepsRemaining = tuyaLight_levelSteps((u16)transitionTimeDs);
	}

	pLevel->remainingTime = tuyaLight_levelDs(levelInfo.stepsRemaining);

	levelInfo.targetLevel = newLevel;
	levelInfo.hasTarget = TRUE;
	levelInfo.stepLevel256 = ((s32)(newLevel - pLevel->curLevel)) << 8;
	levelInfo.stepLevel256 /= (s32)levelInfo.stepsRemaining;

	if(cmd->moveMode == LEVEL_MOVE_UP){
		if(levelInfo.withOnOff){
			tuyaLight_onoff(ZCL_CMD_ONOFF_ON);
		}
	}

	tuyaLight_levelApply();

	tuyaLight_levelWithOnOffChk();

	if(levelInfo.stepsRemaining){
		tuyaLight_LevelTimerStop();
		levelTimerEvt = TL_ZB_TIMER_SCHEDULE(tuyaLight_levelTimerEvtCb, NULL, ZCL_LEVEL_CHANGE_INTERVAL);
	}else{
		tuyaLight_LevelTimerStop();
	}
}

/*********************************************************************
 * @fn      tuyaLight_stepProcess
 *
 * @brief
 *
 * @param	cmdId
 * @param	cmd
 *
 * @return	None
 */
static void tuyaLight_stepProcess(u8 cmdId, step_t *cmd)
{
	zcl_levelAttr_t *pLevel = zcl_levelAttrGet();

	s32 target;

	levelInfo.stepsRemaining = tuyaLight_levelSteps(cmd->transitionTime);
	pLevel->remainingTime = tuyaLight_levelDs(levelInfo.stepsRemaining);

	levelInfo.withOnOff = (cmdId == ZCL_CMD_LEVEL_STEP_WITH_ON_OFF) ? TRUE : FALSE;
	levelInfo.offSent = FALSE;
	levelInfo.currentLevel256 = (u16)(pLevel->curLevel) << 8;
	levelInfo.stepLevel256 = (((s32)cmd->stepSize) << 8) / levelInfo.stepsRemaining;

	if(cmd->stepMode == LEVEL_STEP_UP){
		target = (s32)pLevel->curLevel + (s32)cmd->stepSize;
		if(levelInfo.withOnOff){
			tuyaLight_onoff(ZCL_CMD_ONOFF_ON);
		}
	}else{
		target = (s32)pLevel->curLevel - (s32)cmd->stepSize;
		levelInfo.stepLevel256 = -levelInfo.stepLevel256;
	}

	/* Same end stops light_applyUpdate() clamps to, so the landing snap can
	 * never fight the clamp. */
	if(target < (s32)ZCL_LEVEL_ATTR_MIN_LEVEL){ target = (s32)ZCL_LEVEL_ATTR_MIN_LEVEL; }
	if(target > (s32)ZCL_LEVEL_ATTR_MAX_LEVEL){ target = (s32)ZCL_LEVEL_ATTR_MAX_LEVEL; }
	levelInfo.targetLevel = (u8)target;
	levelInfo.hasTarget = TRUE;

	tuyaLight_levelApply();

	tuyaLight_levelWithOnOffChk();

	if(levelInfo.stepsRemaining){
		tuyaLight_LevelTimerStop();
		levelTimerEvt = TL_ZB_TIMER_SCHEDULE(tuyaLight_levelTimerEvtCb, NULL, ZCL_LEVEL_CHANGE_INTERVAL);
	}else{
		tuyaLight_LevelTimerStop();
	}
}

/*********************************************************************
 * @fn      tuyaLight_stopProcess
 *
 * @brief
 *
 * @param	cmdId
 * @param	cmd
 *
 * @return	None
 */
static void tuyaLight_stopProcess(u8 cmdId, stop_t *cmd)
{
	zcl_levelAttr_t *pLevel = zcl_levelAttrGet();

	tuyaLight_LevelTimerStop();
	pLevel->remainingTime = 0;

	levelInfo.stepsRemaining = 0;
	levelInfo.hasTarget = FALSE;
	levelInfo.stepLevel256 = 0;
	levelInfo.currentLevel256 = ((u16)pLevel->curLevel) << 8;
}

/*********************************************************************
 * @fn      tuyaLight_levelCb
 *
 * @brief   Handler for ZCL LEVEL command. This function will set LEVEL attribute first.
 *
 * @param	pAddrInfo
 * @param   cmd - level cluster command id
 * @param   cmdPayload
 *
 * @return  status_t
 */
status_t tuyaLight_levelCb(zclIncomingAddrInfo_t *pAddrInfo, u8 cmdId, void *cmdPayload)
{
	if(pAddrInfo->dstEp == TUYA_LIGHT_ENDPOINT){
		switch(cmdId){
			case ZCL_CMD_LEVEL_MOVE_TO_LEVEL:
			case ZCL_CMD_LEVEL_MOVE_TO_LEVEL_WITH_ON_OFF:
				tuyaLight_moveToLevelProcess(cmdId, (moveToLvl_t *)cmdPayload);
				break;
			case ZCL_CMD_LEVEL_MOVE:
			case ZCL_CMD_LEVEL_MOVE_WITH_ON_OFF:
				tuyaLight_moveProcess(cmdId, (move_t *)cmdPayload);
				break;
			case ZCL_CMD_LEVEL_STEP:
			case ZCL_CMD_LEVEL_STEP_WITH_ON_OFF:
				tuyaLight_stepProcess(cmdId, (step_t *)cmdPayload);
				break;
			case ZCL_CMD_LEVEL_STOP:
			case ZCL_CMD_LEVEL_STOP_WITH_ON_OFF:
				tuyaLight_stopProcess(cmdId, (stop_t *)cmdPayload);
				break;
			default:
				break;
		}
	}

	return ZCL_STA_SUCCESS;
}

#endif	/* ZCL_LEVEL_CTRL */

#endif  /* __PROJECT_TL_DIMMABLE_LIGHT__ */
