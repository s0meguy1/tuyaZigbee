/********************************************************************************************************
 * @file    zcl_tuyaMfg.h
 *
 * @brief   Tuya manufacturer cluster 0xEF00 server - effect control.
 *
 * Datapoints (Tuya frame: ZCL manu-specific cmd 0x00, payload
 * [seq u8][dpid u8][type u8][len u16 BE][value]):
 *
 *   0x6E  effect  enum   0..11  (0 = stop, back to ZCL state)
 *   0x6F  speed   value  1..100
 *   0x70  phase   value  0..359 timeline offset for group shows
 *
 * Attributes 0x0001..0x0003 mirror the state and are reportable so
 * Home Assistant shows what the light is actually running. A group
 * broadcast with per-light phases makes the fleet chase.
 *
 * @date    2026
 *******************************************************************************************************/

#pragma once

#if defined(__cplusplus)
extern "C" {
#endif

#define ZCL_CLUSTER_TUYA_EFFECT        0xEF00
#define TUYA_EFFECT_MANU_CODE          0x1002   /* frames carry the Tuya code */

/* attribute ids on our EF00 cluster */
#define ZCL_ATTRID_TUYA_FX_EFFECT      0x0001
#define ZCL_ATTRID_TUYA_FX_SPEED       0x0002
#define ZCL_ATTRID_TUYA_FX_PHASE       0x0003

status_t zcl_tuyaMfg_register(u8 endpoint, u16 manuCode, u8 attrNum,
							  const zclAttrInfo_t attrTbl[], cluster_forAppCb_t cb);

#if defined(__cplusplus)
}
#endif
