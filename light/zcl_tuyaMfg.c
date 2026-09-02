/********************************************************************************************************
 * @file    zcl_tuyaMfg.c
 *
 * @brief   See zcl_tuyaMfg.h.
 *
 * @date    2026
 *******************************************************************************************************/

#if (__PROJECT_TL_DIMMABLE_LIGHT__)

#include "../common/comm_cfg.h"
#include "tl_common.h"
#include "zb_api.h"
#include "zcl_include.h"
#include "tuyaLight.h"
#include "tuyaLightCtrl.h"
#include "light_effects.h"
#include "moes_fxwire.h"
#include "zcl_tuyaMfg.h"
#include "moes_bootmark.h"

/* ---- ZCL attributes: read straight from the engine's state ---- */
#define ZCL_TUYAFX_ATTR_NUM  4
static const zclAttrInfo_t tuyaFx_attrTbl[ZCL_TUYAFX_ATTR_NUM] = {
	{ ZCL_ATTRID_TUYA_FX_EFFECT, ZCL_DATA_TYPE_UINT8,  ACCESS_CONTROL_READ | ACCESS_CONTROL_REPORTABLE, (u8*)&g_moesFx.effect },
	{ ZCL_ATTRID_TUYA_FX_SPEED,  ZCL_DATA_TYPE_UINT8,  ACCESS_CONTROL_READ | ACCESS_CONTROL_REPORTABLE, (u8*)&g_moesFx.speed  },
	{ ZCL_ATTRID_TUYA_FX_PHASE,  ZCL_DATA_TYPE_UINT16, ACCESS_CONTROL_READ | ACCESS_CONTROL_REPORTABLE, (u8*)&g_moesFx.phase  },
	/* Build 24. Read-only and NOT reportable: it is boot-constant, so a report
	 * configuration on it could never fire and would only waste a table slot. */
	{ ZCL_ATTRID_TUYA_DIAG_LAST_BOOT, ZCL_DATA_TYPE_UINT8, ACCESS_CONTROL_READ, (u8*)&g_moesBootMarkPrev },
};

/* ---- deferred frame (build 36) ----
 *
 * A frame carrying a delay is parsed now and applied delay ms later. A group
 * broadcast reaches every member within a few milliseconds, so members that
 * each schedule "receipt + delay" land together far more tightly than frames
 * that each start on arrival, and a cue can be armed slightly ahead of the
 * moment it is needed. There is one slot: any new frame, delayed or not,
 * replaces a pending one - the newest command always wins. */
static moes_fxFrame_t tuyaFx_pending;
static ev_timer_event_t *tuyaFx_delayTimer = NULL;

/* ---- reports (build 36) ----
 *
 * Reports are a UNICAST feature. A frame addressed to this fixture arms them;
 * a group frame disarms them. A show is driven by group broadcasts, and
 * nineteen fixtures each answering every broadcast would be nineteen unicasts
 * fighting the next cue for airtime, on a transport already measured at
 * ~1.55 frames/s. Reports carry a random 150-1000 ms delay and coalesce, so a
 * burst of unicast writes from the UI costs one report. A dataQuery is always
 * answered, immediately, to whoever asked. */
static bool tuyaFx_reportsArmed = FALSE;
static u16  tuyaFx_reportAddr = 0x0000;
static u8   tuyaFx_reportEp = 1;
static u16  tuyaFx_reportSeq = 0;
static ev_timer_event_t *tuyaFx_reportTimer = NULL;

static void tuyaFx_reportSend(u16 dstAddr, u8 dstEp)
{
	u8 buf[MOES_FX_REPORT_MAX_LEN];
	moes_fxReport_t st;
	epInfo_t dst;
	u32 len;

	lightFx_report(&st);
	len = moes_fxWireReportBuild(buf, sizeof(buf), tuyaFx_reportSeq++, &st);
	if(!len){
		return;
	}

	TL_SETSTRUCTCONTENT(dst, 0);
	dst.dstAddrMode = APS_SHORT_DSTADDR_WITHEP;
	dst.dstAddr.shortAddr = dstAddr;
	dst.dstEp = dstEp;
	dst.profileId = HA_PROFILE_ID;

	/* SERVER-TO-CLIENT direction, and this is not a free choice.
	 *
	 * zigbee-herdsman splits a cluster's commands into `commands` (what a
	 * gateway sends to a device) and `commandsResponse` (what a device sends
	 * back), and it picks the table using the frame's direction bit. For
	 * manuSpecificTuya, dataRequest(0x00) and dataQuery(0x03) are in `commands`
	 * while dataResponse(0x01), dataReport(0x02) and the status reports are in
	 * `commandsResponse`. A report sent with the client-to-server bit therefore
	 * makes herdsman look for command 0x02 in the wrong table, fail to match,
	 * and hand zigbee2mqtt an undecoded `raw` frame - which no fromZigbee
	 * converter can act on, so the report is silently lost.
	 *
	 * Build 36 shipped with the wrong bit and the payload was perfect; the
	 * fixture answered every query and nothing was ever published. Caught on
	 * the first field canary, fixed in build 37, and pinned by a source
	 * contract in tools/build19_hosttest.
	 *
	 * Cluster-specific, no manufacturer code (see zcl_tuyaMfg_register), no
	 * default response wanted. */
	zcl_sendCmd(TUYA_LIGHT_ENDPOINT, &dst, ZCL_CLUSTER_TUYA_EFFECT, MOES_TUYA_CMD_DATA_REPORT,
				TRUE, ZCL_FRAME_SERVER_CLIENT_DIR, TRUE, MANUFACTURER_CODE_NONE, ZCL_SEQ_NUM,
				(u16)len, buf);
}

static s32 tuyaFx_reportTimerCb(void *arg)
{
	(void)arg;
	tuyaFx_reportTimer = NULL;
	tuyaFx_reportSend(tuyaFx_reportAddr, tuyaFx_reportEp);
	return -1;
}

static void tuyaFx_reportSchedule(void)
{
	if(tuyaFx_reportTimer){
		return;   /* coalesce */
	}
	tuyaFx_reportTimer = TL_ZB_TIMER_SCHEDULE(tuyaFx_reportTimerCb, NULL,
											  150u + (u32)(zb_random() % 850u));
}

void tuyaFx_stateChanged(void)
{
	if(tuyaFx_reportsArmed){
		tuyaFx_reportSchedule();
	}
}

/* ---- frame application ---- */

static s32 tuyaFx_delayTimerCb(void *arg)
{
	(void)arg;
	tuyaFx_delayTimer = NULL;
	lightFx_applyFrame(&tuyaFx_pending);
	tuyaFx_stateChanged();
	return -1;
}

static void tuyaFx_pendingCancel(void)
{
	if(tuyaFx_delayTimer){
		TL_ZB_TIMER_CANCEL(&tuyaFx_delayTimer);
		tuyaFx_delayTimer = NULL;
	}
}

static int tuyaFx_cueLoadCb(void *ctx, u8 start, const u8 *entries, u8 n)
{
	(void)ctx;
	return lightFx_cueLoad(start, entries, n) ? 1 : 0;
}

/* ---- Tuya datapoint frame: [seq u16][dpid][type][len16BE][value]... ---- */
static status_t tuyaMfg_cmdHandler(zclIncoming_t *pInMsg)
{
	moes_fxFrame_t f;
	moes_fxWireStatus_e st;
	bool unicast = (pInMsg->msg->indInfo.dst_addr_mode != APS_SHORT_GROUPADDR_NOEP);

	if(pInMsg->hdr.cmd == MOES_TUYA_CMD_DATA_QUERY){
		/* "Report everything, now, to whoever asked." */
		tuyaFx_reportSend(pInMsg->msg->indInfo.src_short_addr, pInMsg->msg->indInfo.src_ep);
		return ZCL_STA_SUCCESS;
	}
	if(pInMsg->hdr.cmd != MOES_TUYA_CMD_DATA_REQUEST && pInMsg->hdr.cmd != MOES_TUYA_CMD_DATA_RESPONSE){
		return ZCL_STA_UNSUP_CLUSTER_COMMAND;
	}

	/* The whole frame is parsed and validated before anything is applied, so a
	 * frame either lands entirely or not at all. Cue-list uploads are the one
	 * thing applied during parsing: they are data, not actions. */
	st = moes_fxWireParse(pInMsg->pData, pInMsg->dataLen, MOES_EF_MAX, &f, tuyaFx_cueLoadCb, NULL);
	if(st == MOES_FXW_MALFORMED){
		return ZCL_STA_MALFORMED_COMMAND;
	}
	if(st == MOES_FXW_INVALID){
		return ZCL_STA_INVALID_VALUE;
	}

	/* Arm or disarm reports by who sent this (see the note above). */
	if(unicast){
		tuyaFx_reportsArmed = TRUE;
		tuyaFx_reportAddr = pInMsg->msg->indInfo.src_short_addr;
		tuyaFx_reportEp = pInMsg->msg->indInfo.src_ep;
	}else{
		tuyaFx_reportsArmed = FALSE;
	}

	if(st == MOES_FXW_EMPTY){
		/* Only uploads: nothing to defer or apply. */
		if(unicast){ tuyaFx_reportSchedule(); }
		return ZCL_STA_SUCCESS;
	}

	/* Newest command wins: a fresh frame, delayed or not, replaces a pending one. */
	tuyaFx_pendingCancel();

	if((f.present & MOES_FXF_DELAY) && f.delay){
		tuyaFx_pending = f;
		tuyaFx_delayTimer = TL_ZB_TIMER_SCHEDULE(tuyaFx_delayTimerCb, NULL, f.delay);
		if(!tuyaFx_delayTimer){
			return ZCL_STA_INSUFFICIENT_SPACE;
		}
		return ZCL_STA_SUCCESS;
	}

	if(!lightFx_applyFrame(&f)){
		return ZCL_STA_INVALID_VALUE;
	}
	if(unicast){
		tuyaFx_reportSchedule();
	}
	return ZCL_STA_SUCCESS;
}

status_t zcl_tuyaMfg_register(u8 endpoint, u16 manuCode, u8 attrNum,
							  const zclAttrInfo_t attrTbl[], cluster_forAppCb_t cb){
	(void)attrNum; (void)attrTbl; (void)cb; (void)manuCode;
	/* MANUFACTURER_CODE_NONE, not the Tuya code: zigbee-herdsman defines
	 * cluster 0xEF00 with manufacturerCode undefined and therefore sends
	 * plain (non manufacturer-specific) cluster commands. zcl.c rejects the
	 * frame unless pCluster->manuCode equals the frame's manufacturer code. */
	return zcl_registerCluster(endpoint, ZCL_CLUSTER_TUYA_EFFECT,
							   MANUFACTURER_CODE_NONE,
							   ZCL_TUYAFX_ATTR_NUM, tuyaFx_attrTbl,
							   tuyaMfg_cmdHandler, NULL);
}

#endif  /* __PROJECT_TL_DIMMABLE_LIGHT__ */
