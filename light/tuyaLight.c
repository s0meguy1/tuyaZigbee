/********************************************************************************************************
 * @file    tuyaLight.c
 *
 * @brief   This is the source file for tuyaLight
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
#include "../common/comm_cfg.h"
#include "tl_common.h"
#include "zb_api.h"
#include "zcl_include.h"
#include "bdb.h"
#include "ota.h"
#include "gp.h"
#include "tuyaLight.h"
#include "tuyaLightCtrl.h"
#include "app_ui.h"
#include "factory_reset.h"
#include "moes_flashcfg.h"
#include "light_effects.h"
#include "moes_rescue.h"
#if ZBHCI_EN
#include "zbhci.h"
#endif
#if ZCL_WWAH_SUPPORT
#include "wwah.h"
#endif

/**********************************************************************
 * LOCAL CONSTANTS
 */


/**********************************************************************
 * TYPEDEFS
 */


/**********************************************************************
 * GLOBAL VARIABLES
 */
app_ctx_t gLightCtx;


#ifdef ZCL_OTA
extern ota_callBack_t tuyaLight_otaCb;

//running code firmware information
ota_preamble_t tuyaLight_otaInfo = {
	.fileVer 			= FILE_VERSION,
	.imageType 			= IMAGE_TYPE,
	.manufacturerCode 	= MANUFACTURER_CODE_TELINK, // Note it is not real Telink ID, just to make SDK happy
};
#endif


//Must declare the application call back function which used by ZDO layer
const zdo_appIndCb_t appCbLst = {
	bdb_zdoStartDevCnf,//start device cnf cb
	NULL,//reset cnf cb
	NULL,//device announce indication cb
	tuyaLight_leaveIndHandler,//leave ind cb
	tuyaLight_leaveCnfHandler,//leave cnf cb
	tuyaLight_nwkUpdateIndicateHandler,//nwk update ind cb
	NULL,//permit join ind cb
	NULL,//nlme sync cnf cb
	NULL,//tc join ind cb
	NULL,//tc detects that the frame counter is near limit
};


/**
 *  @brief Definition for bdb commissioning setting
 */
bdb_commissionSetting_t g_bdbCommissionSetting = {
	.linkKey.tcLinkKey.keyType = SS_GLOBAL_LINK_KEY,
	.linkKey.tcLinkKey.key = (u8 *)tcLinkKeyCentralDefault,       		//can use unique link key stored in NV

	.linkKey.distributeLinkKey.keyType = MASTER_KEY,
	.linkKey.distributeLinkKey.key = (u8 *)linkKeyDistributedMaster,  	//use linkKeyDistributedCertification before testing

	.linkKey.touchLinkKey.keyType = MASTER_KEY,
	.linkKey.touchLinkKey.key = (u8 *)touchLinkKeyMaster,   			//use touchLinkKeyCertification before testing

#if TOUCHLINK_SUPPORT
	.touchlinkEnable = 1,												/* enable touch-link */
#else
	.touchlinkEnable = 0,												/* disable touch-link */
#endif
	.touchlinkChannel = DEFAULT_CHANNEL, 								/* touch-link default operation channel for target */
	.touchlinkLqiThreshold = 0xA0,			   							/* threshold for touch-link scan req/resp command */
};

/**********************************************************************
 * LOCAL VARIABLES
 */
ev_timer_event_t *tuyaLightAttrsStoreTimerEvt = NULL;

#if MOES_TS0505B
static void tuyaLight_reportingTabSanitize(void);
#endif


/**********************************************************************
 * FUNCTIONS
 */

/*********************************************************************
 * @fn      stack_init
 *
 * @brief   This function initialize the ZigBee stack and related profile. If HA/ZLL profile is
 *          enabled in this application, related cluster should be registered here.
 *
 * @param   None
 *
 * @return  None
 */
void stack_init(void)
{
	/* Initialize ZB stack */
	zb_init();

	/* Register stack CB */
    zb_zdoCbRegister((zdo_appIndCb_t *)&appCbLst);
}

/*********************************************************************
 * @fn      user_app_init
 *
 * @brief   This function initialize the application(Endpoint) information for this node.
 *
 * @param   None
 *
 * @return  None
 */
void user_app_init(void)
{
	af_nodeDescManuCodeUpdate(MANUFACTURER_CODE_TELINK);

    /* Initialize ZCL layer */
	/* Register Incoming ZCL Foundation command/response messages */
    zcl_init(tuyaLight_zclProcessIncomingMsg);

	/* Register endPoint */
	af_endpointRegister(TUYA_LIGHT_ENDPOINT, (af_simple_descriptor_t *)&tuyaLight_simpleDesc, zcl_rx_handler, NULL);
#if AF_TEST_ENABLE
	/* A sample of AF data handler. */
	af_endpointRegister(SAMPLE_TEST_ENDPOINT, (af_simple_descriptor_t *)&sampleTestDesc, afTest_rx_handler, afTest_dataSendConfirm);
#endif

	/* Initialize or restore attributes, this must before 'zcl_register()' */
	zcl_tuyaLightAttrsInit();
	zcl_reportingTabInit();

	/* Register ZCL specific cluster information */
	zcl_register(TUYA_LIGHT_ENDPOINT, TUYALIGHT_CB_CLUSTER_NUM, (zcl_specClusterInfo_t *)g_tuyaLightClusterList);

#if ZCL_GP_SUPPORT
	/* Initialize GP */
	gp_init(TUYA_LIGHT_ENDPOINT);
#endif

#if ZCL_OTA_SUPPORT
	/* Initialize OTA */
    ota_init(OTA_TYPE_CLIENT, (af_simple_descriptor_t *)&tuyaLight_simpleDesc, &tuyaLight_otaInfo, &tuyaLight_otaCb);
#endif

#if ZCL_WWAH_SUPPORT
    /* Initialize WWAH server */
    wwah_init(WWAH_TYPE_SERVER, (af_simple_descriptor_t *)&tuyaLight_simpleDesc);
#endif

#if MOES_TS0505B
	/* Last: every cluster this image has is now registered, so
	 * zcl_findAttribute() can give a truthful answer. */
	tuyaLight_reportingTabSanitize();
#endif
}



#if MOES_TS0505B
/*********************************************************************
 * @fn      tuyaLight_reportingTabSanitize
 *
 * @brief   Drop restored reporting entries whose attribute this image does
 *          not have.
 *
 * zcl_reportingTabInit() restores the whole table from NV without checking
 * anything, but the only *live* validation is in zcl_configureReporting(),
 * on the inbound command path. Nothing revalidates after a restore. Three
 * places then dereference the result of zcl_findAttribute() on a restored
 * entry - zcl_reporting.c lines 400, 437 and 489 - and each of them calls
 * ZB_EXCEPTION_POST(SYS_EXCEPTTION_ZB_ZCL_ENTRY) if the attribute is gone.
 * That reaches our sys_exceptHandlerRegister() callback, which calls
 * SYSTEM_RESET(). reportNoMinLimit() is driven from app_task() on every idle
 * poll, so the reset is immediate and repeats on every boot: a permanent,
 * unrecoverable loop caused by nothing more than a firmware update that
 * removed an attribute.
 *
 * That is not reachable today - our own previous images have the same
 * attribute set, and the stock Tuya NV is invisible to us (its modules use
 * ids 9/10/11 and nv_sector_read() rejects them on the idName test). It
 * becomes reachable the moment any future image drops a reportable
 * attribute, and v1.2 has just added three of them on cluster 0xEF00. Twenty
 * lines here retire the whole class.
 *
 * MUST run after every zcl_register()/ota_init()/gp_init(), because
 * zcl_findAttribute() can only answer for clusters that exist.
 */
static void tuyaLight_reportingTabSanitize(void)
{
	u8 dropped = 0;

	for(u8 i = 0; i < ZCL_REPORTING_TABLE_NUM; i++){
		reportCfgInfo_t *pEntry = &reportingTab.reportCfgInfo[i];

		if(!pEntry->used){
			continue;
		}

		if(!zcl_findAttribute(pEntry->endPoint, pEntry->clusterID, pEntry->attrID)){
			zcl_reportCfgInfoEntryClear(pEntry);
			dropped++;
		}
	}

	if(dropped){
		reportingTab.reportNum = (reportingTab.reportNum > dropped)
								 ? (reportingTab.reportNum - dropped) : 0;
		/* Persist immediately: if we crash before the next save the same
		 * stale entries come back on the next boot. */
		zcl_reportingTab_save();
	}
}
#endif

s32 tuyaLightAttrsStoreTimerCb(void *arg)
{
	zcl_onOffAttr_save();
	zcl_levelAttr_save();
	zcl_colorCtrlAttr_save();

	tuyaLightAttrsStoreTimerEvt = NULL;
	return -1;
}

void tuyaLightAttrsStoreTimerStart(void)
{
	if(tuyaLightAttrsStoreTimerEvt){
		TL_ZB_TIMER_CANCEL(&tuyaLightAttrsStoreTimerEvt);
	}
	tuyaLightAttrsStoreTimerEvt = TL_ZB_TIMER_SCHEDULE(tuyaLightAttrsStoreTimerCb, NULL, 1000);//200);
}

void tuyaLightAttrsChk(void)
{
	if(gLightCtx.lightAttrsChanged){
		gLightCtx.lightAttrsChanged = FALSE;
		if(zb_isDeviceJoinedNwk()){
			tuyaLightAttrsStoreTimerStart();
		}
	}
}

void report_handler(void)
{
	if(zb_isDeviceJoinedNwk()){
		if(zcl_reportingEntryActiveNumGet()){
			u16 second = 1;//TODO: fix me

			reportNoMinLimit();

			//start report timer
			reportAttrTimerStart(second);
		}else{
			//stop report timer
			reportAttrTimerStop();
		}
	}
}

void app_task(void)
{
#if HAVE_NET_BUTTON
	/* MOES: belt-and-braces with the guard inside app_key_handler().
	 * This board has no button; running the scanner on it synthesises a
	 * permanently-held VK_SW1 and factory-resets the light every 5 s. */
	app_key_handler();
#endif
	localPermitJoinState();
	if(BDB_STATE_GET() == BDB_STATE_IDLE){
#if MOES_TS0505B
		/* Build 19: the storm flag is advisory only. Factory-reset handling,
		 * reporting and persistence stay fully active whether it is set or not;
		 * only zb_appCb.c may consult it for OTA cadence and join blink. */
		factoryRst_handler();
		report_handler();
		tuyaLightAttrsChk();
#else
		//factroyRst_handler();

		report_handler();

#if 1/* NOTE: If set to '1', the latest status of lighting will be stored. */
		tuyaLightAttrsChk();
#endif
#endif
	}
}

static void tuyaLightSysException(void)
{
#if MOES_TS0505B
	/* MOES: reset immediately, write nothing.
	 *
	 * Upstream saved on/off, level and colour to NV here. Three reasons not
	 * to on this device:
	 *
	 * 1. sys_exceptionPost() is called synchronously from wherever the fault
	 *    was detected - including from inside the NV layer itself
	 *    (drv_nv.c nv_itemLengthCheckAdd) and from the ev_buffer free path.
	 *    Re-entering nv_flashWriteNew() from there can leave a sector
	 *    half-written, which is a *worse* failure than the one we are
	 *    reacting to.
	 * 2. If the fault repeats every boot, so do the flash writes. A reset
	 *    loop at ~10 s/cycle would then be grinding the APP/ZCL NV sectors
	 *    at ~360 writes an hour.
	 * 3. It buys nothing: tuyaLightAttrsChk() already persists this state
	 *    one second after any change during normal operation.
	 *
	 * Getting back to a state where an OTA can land is the only thing that
	 * matters here. Nothing needs to be recorded: the reset itself is what
	 * moes_rescue.c counts, on the next boot, from a context where writing
	 * NV is safe. */
	SYSTEM_RESET();
#else
	zcl_onOffAttr_save();
	zcl_levelAttr_save();
	zcl_colorCtrlAttr_save();

	SYSTEM_RESET();
#endif
}

/*********************************************************************
 * @fn      user_init
 *
 * @brief   User level initialization code.
 *
 * @param   isRetention - if it is waking up with ram retention.
 *
 * @return  None
 */
void user_init(bool isRetention)
{
	(void)isRetention;

	/* Initialize LEDs*/
	led_init();
	hwLight_init();   /* also loads the Tuya factory JSON + MAC fallback data */

#if MOES_TS0505B
	lightFx_init();   /* plain memset, no stack/NV dependency */
#endif

	/* Initialize Stack */
	stack_init();

#if MOES_TS0505B
	/* MUST come after stack_init(): nv_init() lives inside the prebuilt
	 * stack library and is called from zb_init(), so any NV access before
	 * this point runs against an uninitialized NV subsystem. factoryRst_init()
	 * does three NV operations and schedules a TL_ZB_TIMER; calling it early
	 * crashed on every boot, and the registered exception handler turned that
	 * into SYSTEM_RESET() - i.e. a permanent boot loop on a device that can
	 * only be fixed over the air. Cost one ceiling light to learn.
	 *
	 * moes_rescueBootCheck() is the first NV user after stack_init() for the
	 * same reason, and because everything below needs to know the answer.
	 * It uses two one-byte application items (probation and the boot-young
	 * marker); their bounded writes are documented in moes_rescue.h. */
	moes_rescueBootCheck();
#endif

	/* Initialize user application */
	user_app_init();

#if MOES_TS0505B
	/* Build 18: run unconditionally. The build-16-and-earlier rescue mode
	 * skipped this in its minimal path; with rescue now an advisory flag
	 * (moes_rescue.h) there is no minimal path and every boot must count
	 * power cycles like a normal light. */
	factoryRst_init();
#endif

	/* Register except handler for test */
	sys_exceptHandlerRegister(tuyaLightSysException);

	/* Adjust light state to default attributes.
	 * Build 18 (MOES): full state restore on EVERY boot - a light that has
	 * just power-cycled after an outage must come up at its previous state
	 * and answer commands immediately, storm flag or not. The advisory
	 * flag's only visible effects (5-pulse join blink, faster OTA cadence)
	 * live in zb_appCb.c where the join event fires. */
	light_adjust();

	/* User's Task */
#if ZBHCI_EN
	zbhciInit();
	ev_on_poll(EV_POLL_HCI, zbhciTask);
#endif
	ev_on_poll(EV_POLL_IDLE, app_task);

    /* Read the pre-install code from NV */
	if(bdb_preInstallCodeLoad(&gLightCtx.tcLinkKey.keyType, gLightCtx.tcLinkKey.key) == RET_OK){
		g_bdbCommissionSetting.linkKey.tcLinkKey.keyType = gLightCtx.tcLinkKey.keyType;
		g_bdbCommissionSetting.linkKey.tcLinkKey.key = gLightCtx.tcLinkKey.key;
	}

    /* Set default reporting configuration */
    u8 reportableChange = 0x00;
    bdb_defaultReportingCfg(TUYA_LIGHT_ENDPOINT, HA_PROFILE_ID, ZCL_CLUSTER_GEN_ON_OFF, ZCL_ATTRID_ONOFF,
    						0x0000, 0x003c, (u8 *)&reportableChange);

    /* Initialize BDB */
	bdb_init((af_simple_descriptor_t *)&tuyaLight_simpleDesc, &g_bdbCommissionSetting, &g_zbDemoBdbCb, 1);
}

#endif  /* __PROJECT_TL_DIMMABLE_LIGHT__ */
