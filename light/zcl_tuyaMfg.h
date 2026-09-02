/********************************************************************************************************
 * @file    zcl_tuyaMfg.h
 *
 * @brief   Tuya manufacturer cluster 0xEF00 server - light-show control.
 *
 * Wire format, datapoint ids and the report layout are in moes_fxwire.h; the
 * engine they drive is light_effects.h. This file is the glue: it parses a
 * frame, defers it when it carries a delay, applies it, and reports.
 *
 * Attributes 0x0001..0x0003 mirror effect/speed/phase and are reportable
 * through the normal ZCL reporting table; 0x0004 is a boot diagnostic. From
 * build 36 the full state is also readable as Tuya datapoints (a dataQuery,
 * command 0x03, is answered with a dataReport, command 0x02).
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
/* Diagnostic, read-only: why the PREVIOUS boot ended. 0 = not one of our
 * exceptions (a power cycle, or a watchdog/hardware reset); otherwise the
 * SYS_EXCEPTTION_* code + 1. See light/moes_bootmark.h. */
#define ZCL_ATTRID_TUYA_DIAG_LAST_BOOT 0x0004

status_t zcl_tuyaMfg_register(u8 endpoint, u16 manuCode, u8 attrNum,
							  const zclAttrInfo_t attrTbl[], cluster_forAppCb_t cb);

#if defined(__cplusplus)
}
#endif
