/********************************************************************************************************
 * @file    tuyaLightCtrl.h
 *
 * @brief   This is the header file for tuyaLightCtrl
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

#ifndef _TUYA_LIGHT_CTRL_H_
#define _TUYA_LIGHT_CTRL_H_


/**********************************************************************
 * CONSTANT
 */


/**********************************************************************
 * FUNCTIONS
 */
void hwLight_init(void);
void hwLight_onOffUpdate(u8 onOff);
void hwLight_levelUpdate(u8 level);
void hwLight_colorUpdate_colorTemperature(u16 colorTemperatureMireds, u8 level);
void hwLight_colorUpdate_HSV2RGB(u8 hue, u8 saturation, u8 level);

/* integer HSV -> RGB, 0..255 per channel */
void hsvToRGB(u8 hue, u8 saturation, u8 level, u8 *R, u8 *G, u8 *B);

/* CIE xy -> HSV, so an XY colour command can drive the HSV output path. */
void xyToHueSat(u16 x, u16 y, u8 *hue, u8 *saturation);
/* the single 5-channel output point: gamma + white-balance trim + active
 * level are applied here, for both ZCL control and the effect engine */
void moes_outSet(u8 r, u8 g, u8 b, u8 cw, u8 ww);
void temperatureToCW(u16 temperatureMireds, u8 level, u8 *C, u8 *W);
void pwmSetDuty(u8 ch, u16 dutycycle);

void light_adjust(void);
void light_fresh(void);

/* Build 25. End any in-flight level/colour transition WITHOUT touching the
 * output, so no further transition step can run.
 *
 * light_fresh() stops a running effect - correct, a user command should take
 * the output back - but light_applyUpdate() calls light_fresh() on EVERY step
 * of a transition, so a 3 s fade calls it ~30 times over 3 s. An effect started
 * one second after a fade was therefore killed by the tail of that fade, about
 * 100 ms later. It looked random: start the effect after the fade finishes and
 * it survives, start it during and it dies.
 *
 * lightFx_start() calls these so that starting an effect takes ownership of the
 * output. A NEW level/colour command still stops the effect, as intended; an
 * OLD one can no longer reach forward in time to kill it. */
void tuyaLight_levelTransitionCancel(void);
void tuyaLight_colorTransitionCancel(void);
void light_applyUpdate(u8 *curLevel, u16 *curLevel256, s32 *stepLevel256, u16 *remainingTime, u8 minLevel, u8 maxLevel, bool wrap);
void light_applyUpdate_16(u16 *curLevel, u32 *curLevel256, s32 *stepLevel256, u16 *remainingTime, u16 minLevel, u16 maxLevel, bool wrap);

void light_blink_start(u8 times, u16 ledOnTime, u16 ledOffTime);
void light_blink_stop(void);

#endif	/* _TUYA_LIGHT_CTRL_H_ */
