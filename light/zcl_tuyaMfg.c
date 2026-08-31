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
#include "zcl_include.h"
#include "tuyaLight.h"
#include "tuyaLightCtrl.h"
#include "light_effects.h"
#include "zcl_tuyaMfg.h"
#include "moes_bootmark.h"
#include "moes_color.h"

/* ---- reportable state mirrored from the engine ---- */
typedef struct {
	u8  effect;
	u8  speed;
	u16 phase;
}zcl_tuyaFxAttr_t;
zcl_tuyaFxAttr_t g_zcl_tuyaFxAttrs = {0, 50, 0};

#define ZCL_TUYAFX_ATTR_NUM  4
static const zclAttrInfo_t tuyaFx_attrTbl[ZCL_TUYAFX_ATTR_NUM] = {
	{ ZCL_ATTRID_TUYA_FX_EFFECT, ZCL_DATA_TYPE_UINT8,  ACCESS_CONTROL_READ | ACCESS_CONTROL_REPORTABLE, (u8*)&g_zcl_tuyaFxAttrs.effect },
	{ ZCL_ATTRID_TUYA_FX_SPEED,  ZCL_DATA_TYPE_UINT8,  ACCESS_CONTROL_READ | ACCESS_CONTROL_REPORTABLE, (u8*)&g_zcl_tuyaFxAttrs.speed  },
	{ ZCL_ATTRID_TUYA_FX_PHASE,  ZCL_DATA_TYPE_UINT16, ACCESS_CONTROL_READ | ACCESS_CONTROL_REPORTABLE, (u8*)&g_zcl_tuyaFxAttrs.phase  },
	/* Build 24. Read-only and NOT reportable: it is boot-constant, so a report
	 * configuration on it could never fire and would only waste a table slot. */
	{ ZCL_ATTRID_TUYA_DIAG_LAST_BOOT, ZCL_DATA_TYPE_UINT8, ACCESS_CONTROL_READ, (u8*)&g_moesBootMarkPrev },
};

static void tuyaFx_stateReport(void){
	/* v1: the attributes are pollable; reports fire through the normal
	 * reporting table if z2m configures it for the cluster. */
}

/* ---- Tuya datapoint frame: [seq][dpid][type][len16BE][value] ---- */
static status_t tuyaMfg_cmdHandler(zclIncoming_t *pInMsg){
	if(pInMsg->hdr.cmd != 0x00 && pInMsg->hdr.cmd != 0x01){
		return ZCL_STA_UNSUP_CLUSTER_COMMAND;
	}

	u8 *p = pInMsg->pData;
	u16 len = pInMsg->dataLen;

	/* Tuya 0xEF00 dataRequest payload, per zigbee-herdsman
	 * (writeListTuyaDataPointValues):
	 *     seq   u16
	 *     dp    u8
	 *     type  u8
	 *     len   u16 BIG endian
	 *     data  len bytes
	 * NOTE seq is TWO bytes - reading it as one shifted every field. */
	if(len < 6){
		return ZCL_STA_MALFORMED_COMMAND;
	}

	u8 dpid = p[2];
	u8 type = p[3];
	u16 vlen = ((u16)p[4] << 8) | p[5];
	/* No (u16) cast on the sum: vlen is attacker-controlled and 6 + 0xFFFF
	 * truncates to 5, which passes "5 > len" for every len >= 5 and lets the
	 * v32 loop below read up to 4 bytes past the ASDU. Compare in u32. */
	if(((u32)vlen + 6u) > (u32)len){
		return ZCL_STA_MALFORMED_COMMAND;
	}
	u8 *val = p + 6;

	u32 v32 = 0;
	for(u8 i = 0; i < vlen && i < 4; i++){
		v32 = (v32 << 8) | val[i];
	}

	bool applied = TRUE;

	switch(dpid){
	case 0x6E:   /* effect */
		if(v32 >= MOES_EF_MAX){
			applied = FALSE;
			break;
		}
		lightFx_start((u8)v32, g_zcl_tuyaFxAttrs.speed, g_zcl_tuyaFxAttrs.phase);
		g_zcl_tuyaFxAttrs.effect = (u8)v32;
		break;

	case 0x6F:   /* speed 1..100 */
		if(v32 < 1 || v32 > 100){
			applied = FALSE;
			break;
		}
		g_zcl_tuyaFxAttrs.speed = (u8)v32;
		if(lightFx_active()){
			lightFx_start(g_zcl_tuyaFxAttrs.effect, g_zcl_tuyaFxAttrs.speed, g_zcl_tuyaFxAttrs.phase);
		}
		break;

	case 0x70:   /* phase 0..359 */
		if(v32 > 359){
			applied = FALSE;
			break;
		}
		g_zcl_tuyaFxAttrs.phase = (u16)v32;
		if(lightFx_active()){
			lightFx_start(g_zcl_tuyaFxAttrs.effect, g_zcl_tuyaFxAttrs.speed, g_zcl_tuyaFxAttrs.phase);
		}
		break;

	case 0x71:   /* light-show hue, 0..359 degrees */
	case 0x72:   /* light-show saturation, 0..100 percent */
	{
		/* Build 27: recolour a RUNNING effect without restarting it.
		 *
		 * A normal ZCL colour command cannot do this. Every colour path ends in
		 * light_fresh(), which deliberately stops a running effect so a user
		 * grabbing the colour picker takes the output back - correct, but it
		 * makes "shift the sparks from blue-white to amber mid-scene"
		 * impossible: the show restarts on every colour change.
		 *
		 * These datapoints write the same ZCL attributes the effects render
		 * from (fxCurHsv reads currentHue/currentSaturation every frame) and
		 * then deliberately do NOT call light_fresh() while an effect is
		 * running. The next frame simply picks the new colour up - no restart,
		 * no dropped frames, one command.
		 *
		 * With no effect running they behave like an ordinary colour command
		 * and apply immediately, so they are never silently inert. */
		zcl_lightColorCtrlAttr_t *pColor = zcl_colorAttrGet();

		if(dpid == 0x71){
			if(v32 > 359){
				applied = FALSE;
				break;
			}
			pColor->currentHue = moes_hueDegToZcl((u16)v32);
		}else{
			if(v32 > 100){
				applied = FALSE;
				break;
			}
			pColor->currentSaturation = moes_satPctToZcl((u8)v32);
		}

		/* Effects render from hue/saturation, so leaving the mode at XY would
		 * let a later refresh re-derive the old hue from stale CurrentX/Y and
		 * silently undo this. */
		pColor->colorMode = ZCL_COLOR_MODE_CURRENT_HUE_SATURATION;
		pColor->enhancedColorMode = ZCL_COLOR_MODE_CURRENT_HUE_SATURATION;

		if(!lightFx_active()){
			light_fresh();
		}
		break;
	}

	default:
		applied = FALSE;
		break;
	}

	if(applied){
		tuyaFx_stateReport();
	}

	(void)type;
	return applied ? ZCL_STA_SUCCESS : ZCL_STA_INVALID_VALUE;
}

status_t zcl_tuyaMfg_register(u8 endpoint, u16 manuCode, u8 attrNum,
							  const zclAttrInfo_t attrTbl[], cluster_forAppCb_t cb){
	(void)attrNum; (void)attrTbl; (void)cb;
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
