/********************************************************************************************************
 * @file    zb_appCb.c
 *
 * @brief   This is the source file for zb_appCb
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
#include "bdb.h"
#include "ota.h"
#include "tuyaLight.h"
#include "tuyaLightCtrl.h"
#include "moes_rescue.h"
#include "moes_liveness.h"

/**********************************************************************
 * LOCAL CONSTANTS
 */
#define DEBUG_HEART		0

#if MOES_TS0505B
/* A recent boot storm asks for an OTA image far more often than the routine
 * six-hour cadence. This is advisory only: it never changes light behaviour. */
#define TUYALIGHT_OTA_QUERY_INTERVAL(dflt)	\
			(moes_rescueActive() ? MOES_RESCUE_OTA_QUERY_SECONDS : (dflt))

/* Delay for the one-shot OTA kick a storm-flagged light gets on join (see
 * zbdemo_bdbCommissioningCb). Kept non-zero so the just-completed join can
 * settle before the query frame goes out. */
#define TUYALIGHT_OTA_JOIN_KICK_DELAY_MS	1000
#else
#define TUYALIGHT_OTA_QUERY_INTERVAL(dflt)	(dflt)
#endif

/**********************************************************************
 * TYPEDEFS
 */


/**********************************************************************
 * LOCAL FUNCTIONS
 */
void zbdemo_bdbInitCb(u8 status, u8 joinedNetwork);
void zbdemo_bdbCommissioningCb(u8 status, void *arg);
void zbdemo_bdbIdentifyCb(u8 endpoint, u16 srcAddr, u16 identifyTime);


/**********************************************************************
 * GLOBAL VARIABLES
 */
bdb_appCb_t g_zbDemoBdbCb = {zbdemo_bdbInitCb, zbdemo_bdbCommissioningCb, zbdemo_bdbIdentifyCb, NULL};

#ifdef ZCL_OTA
ota_callBack_t tuyaLight_otaCb =
{
	tuyaLight_otaProcessMsgHandler,
};

#if MOES_TS0505B
/* The SDK's periodic OTA-query timer is a single ev_timer_event_t defined in
 * ota.c (otaTimer), not a TL_ZB_TIMER pointer. It is non-static but not
 * declared in ota.h; the storm-flag join kick below reaches it through this
 * extern. */
extern ev_timer_event_t otaTimer;
#endif
#endif

/**********************************************************************
 * LOCAL VARIABLES
 */
u32 heartInterval = 0;

#if MOES_TS0505B
/* The stock SDK skips the ZDO device_announce on the very first,
 * factory-new join (empty NV). Set on a factory-new boot and consumed on the
 * first successful commissioning so a conversion announces exactly once and
 * rejoins are never double-announced. */
static bool s_firstJoin = FALSE;
#endif

#if DEBUG_HEART
ev_timer_event_t *heartTimerEvt = NULL;
#endif

/**********************************************************************
 * FUNCTIONS
 */
#if DEBUG_HEART
static s32 heartTimerCb(void *arg){
	if(heartInterval == 0){
		heartTimerEvt = NULL;
		return -1;
	}

	gpio_toggle(LED_POWER);

	return heartInterval;
}
#endif

s32 tuyaLight_bdbNetworkSteerStart(void *arg){
	bdb_networkSteerStart();

	return -1;
}

#if FIND_AND_BIND_SUPPORT
s32 tuyaLight_bdbFindAndBindStart(void *arg){
	bdb_findAndBindStart(BDB_COMMISSIONING_ROLE_TARGET);

	return -1;
}
#endif

/*********************************************************************
 * @fn      zbdemo_bdbInitCb
 *
 * @brief   application callback for bdb initiation
 *
 * @param   status - the status of bdb init BDB_INIT_STATUS_SUCCESS or BDB_INIT_STATUS_FAILURE
 *
 * @param   joinedNetwork  - 1: node is on a network, 0: node isn't on a network
 *
 * @return  None
 */
void zbdemo_bdbInitCb(u8 status, u8 joinedNetwork){
//	printf("bdbInitCb: sta = %x, joined = %x\n", status, joinedNetwork);

	if(status == BDB_INIT_STATUS_SUCCESS){
#if MOES_TS0505B
		/* Build 19: this is the first point at which BDB/ev_timer are known
		 * ready. Arm every successful BDB boot, including factory-new boots:
		 * a healthy commissioning scan keeps the cooperative ticker moving,
		 * while a pre-join timer/scheduler wedge now reaches the hardware fuse. */
		moes_livenessBooted();
#endif
		/*
		 * start bdb commissioning
		 * */
		if(joinedNetwork){
			heartInterval = 1000;

#if MOES_TS0505B
			/* Already on a network at boot: start the "stayed joined long
			 * enough to be healthy" clock now. */
			moes_rescueStableTimerStart();

			/* The Build 19 BDB-success arm above already covers this rejoin
			 * boot as well as factory-new commissioning. */
#endif

#ifdef ZCL_OTA
			/* Build 18: a storm-flagged light (moes_rescue.h) phones home
			 * at the rescue cadence instead of the 6 h routine - a light
			 * that keeps dying gets an OTA shot every cycle. Advisory
			 * only; the query machinery is identical either way. */
			ota_queryStart(TUYALIGHT_OTA_QUERY_INTERVAL(MY_OTA_PERIODIC_QUERY_INTERVAL));
#endif
		}else{
#if MOES_TS0505B
			/* Empty NV at boot: this boot will be a factory-new first join. */
			s_firstJoin = TRUE;
#endif
			heartInterval = 500;

#if	(!ZBHCI_EN)
			u16 jitter = 0;
			do{
				jitter = zb_random() % 0x0fff;
			}while(jitter == 0);
			TL_ZB_TIMER_SCHEDULE(tuyaLight_bdbNetworkSteerStart, NULL, jitter);
#endif
		}
	}else{
		heartInterval = 200;
	}

#if DEBUG_HEART
	if(heartTimerEvt){
		TL_ZB_TIMER_CANCEL(&heartTimerEvt);
	}
	heartTimerEvt = TL_ZB_TIMER_SCHEDULE(heartTimerCb, NULL, heartInterval);
#endif
}

/*********************************************************************
 * @fn      zbdemo_bdbCommissioningCb
 *
 * @brief   application callback for bdb commissioning
 *
 * @param   status - the status of bdb commissioning
 *
 * @param   arg
 *
 * @return  None
 */
void zbdemo_bdbCommissioningCb(u8 status, void *arg){
//	printf("bdbCommCb: sta = %x\n", status);

#if MOES_TS0505B
	/* Any commissioning callback at all is proof the stack's state machine
	 * is alive; the liveness monitor only fires on silence while unjoined. */
	moes_livenessStackActivity();
#endif

	switch(status){
		case BDB_COMMISSION_STA_SUCCESS:
			heartInterval = 1000;

#if MOES_TS0505B
			/* On the network: arm the liveness monitor. */
			moes_livenessJoined();

			if(s_firstJoin){
				/* The stack deliberately skips the announce on a factory-new
				 * first join; send it ourselves so the announce gate can see
				 * the conversion. Clear first so rejoins never double-announce. */
				s_firstJoin = FALSE;
				zb_zdoSendDevAnnance();
			}

			/* Joined. Start (or leave running) the health clock. */
			moes_rescueStableTimerStart();

			/* Build 18: the join blink always runs - output state is never
			 * frozen. A storm-flagged light (moes_rescue.h) blinks 5 short
			 * pulses instead of 2, so a human in the room can tell it
			 * storm-counted recently without losing a single function. */
#endif
#if MOES_TS0505B
			light_blink_start(moes_rescueActive() ? 5 : 2,
							  moes_rescueActive() ? 150 : 200,
							  moes_rescueActive() ? 150 : 200);
#else
			light_blink_start(2, 200, 200);
#endif

#ifdef ZCL_OTA
			ota_queryStart(TUYALIGHT_OTA_QUERY_INTERVAL(OTA_PERIODIC_QUERY_INTERVAL));

#if MOES_TS0505B
			if(moes_rescueActive() &&
			   zcl_attr_imageUpgradeStatus == IMAGE_UPGRADE_STATUS_NORMAL){
				/* A storm-flagged light may be short-lived. If it rejoins and then
				 * wedges again ~90 s later (boothang_stack.md), a query that waits
				 * out the first 10 min interval may never happen - so kick the
				 * already-scheduled periodic query now. ev_on_timer re-arms otaTimer
				 * to fire in TUYALIGHT_OTA_JOIN_KICK_DELAY_MS; the periodic callback
				 * then restores the normal storm cadence. This is a one-shot cadence
				 * adjustment, not a poll-rate change to any other subsystem.
				 *
				 * Guarded on IMAGE_UPGRADE_STATUS_NORMAL so a mid-download
				 * rejoin can never re-arm otaTimer while it is being used as an
				 * image-block/countdown wait timer. */
				ev_on_timer(&otaTimer, TUYALIGHT_OTA_JOIN_KICK_DELAY_MS);
			}
#endif
#endif

#if FIND_AND_BIND_SUPPORT
			if(!gLightCtx.bdbFindBindFlg){
				gLightCtx.bdbFindBindFlg = TRUE;
				TL_ZB_TIMER_SCHEDULE(tuyaLight_bdbFindAndBindStart, NULL, 1000);
			}
#endif
			break;
		case BDB_COMMISSION_STA_IN_PROGRESS:
			break;
		case BDB_COMMISSION_STA_NOT_AA_CAPABLE:
			break;
		case BDB_COMMISSION_STA_NO_NETWORK:
		case BDB_COMMISSION_STA_TCLK_EX_FAILURE:
		case BDB_COMMISSION_STA_TARGET_FAILURE:
			{
				u16 jitter = 0;
				do{
					jitter = zb_random() % 0x2710;
				}while(jitter < 5000);
				TL_ZB_TIMER_SCHEDULE(tuyaLight_bdbNetworkSteerStart, NULL, jitter);
			}
			break;
		case BDB_COMMISSION_STA_FORMATION_FAILURE:
			break;
		case BDB_COMMISSION_STA_NO_IDENTIFY_QUERY_RESPONSE:
			break;
		case BDB_COMMISSION_STA_BINDING_TABLE_FULL:
			break;
		case BDB_COMMISSION_STA_NO_SCAN_RESPONSE:
			break;
		case BDB_COMMISSION_STA_NOT_PERMITTED:
			break;
		case BDB_COMMISSION_STA_REJOIN_FAILURE:
			zb_rejoinReq(zb_apsChannelMaskGet(), g_bdbAttrs.scanDuration);
			break;
		case BDB_COMMISSION_STA_FORMATION_DONE:
#ifndef ZBHCI_EN
			tl_zbMacChannelSet(DEFAULT_CHANNEL);  //set default channel
#endif
			break;
		default:
			break;
	}
}


extern void tuyaLight_zclIdentifyCmdHandler(u8 endpoint, u16 srcAddr, u16 identifyTime);
void zbdemo_bdbIdentifyCb(u8 endpoint, u16 srcAddr, u16 identifyTime){
#if FIND_AND_BIND_SUPPORT
	tuyaLight_zclIdentifyCmdHandler(endpoint, srcAddr, identifyTime);
#endif
}



#ifdef ZCL_OTA
void tuyaLight_otaProcessMsgHandler(u8 evt, u8 status)
{
	if(evt == OTA_EVT_START){
		if(status == ZCL_STA_SUCCESS){

		}else{

		}
	}else if(evt == OTA_EVT_COMPLETE){
		if(status == ZCL_STA_SUCCESS){
#if MOES_TS0505B
			/* mark the reboot so the 3-power-cycle reset counter stays quiet */
			{
				extern void moes_resetSkipNextBoot(void);
				moes_resetSkipNextBoot();
			}
#endif
#if defined(MOES_NOBOOT_MIGRATION)
			/* future: install the staged image into the 0x40000 bank and
			 * retire the bootloader. Not used in v1 - the stock Tuya
			 * bootloader picks the staged image up from 0x70000 itself. */
			{
				extern void moes_otaBankInstall(void);
				moes_otaBankInstall();
			}
#endif
			ota_mcuReboot();
		}else{
			ota_queryStart(OTA_PERIODIC_QUERY_INTERVAL);
		}
	}
}
#endif

s32 tuyaLight_softReset(void *arg){
#if MOES_TS0505B
	/* MOES_EDITING_GUIDE S4.5: every *deliberate* reboot must mark itself so
	 * the 3-power-cycle gesture never mistakes it for a user power cycle.
	 * This one (a network leave) was missing the mark, so a leave burned one
	 * count and left it in NV. */
	{
		extern void moes_resetSkipNextBoot(void);
		moes_resetSkipNextBoot();
	}
#endif
	SYSTEM_RESET();

	return -1;
}

/*********************************************************************
 * @fn      tuyaLight_leaveCnfHandler
 *
 * @brief   Handler for ZDO Leave Confirm message.
 *
 * @param   pRsp - parameter of leave confirm
 *
 * @return  None
 */
void tuyaLight_leaveCnfHandler(nlme_leave_cnf_t *pLeaveCnf)
{
    if(pLeaveCnf->status == SUCCESS){
    	light_blink_start(3, 200, 200);

    	//waiting blink over
    	TL_ZB_TIMER_SCHEDULE(tuyaLight_softReset, NULL, 2 * 1000);
    }
}

/*********************************************************************
 * @fn      tuyaLight_leaveIndHandler
 *
 * @brief   Handler for ZDO leave indication message.
 *
 * @param   pInd - parameter of leave indication
 *
 * @return  None
 */
void tuyaLight_leaveIndHandler(nlme_leave_ind_t *pLeaveInd)
{

}

bool tuyaLight_nwkUpdateIndicateHandler(nwkCmd_nwkUpdate_t *pNwkUpdate){
	return FAILURE;
}

#endif  /* __PROJECT_TL_DIMMABLE_LIGHT__ */
