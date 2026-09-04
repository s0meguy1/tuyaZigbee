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

/* Build 38. The rate at which a ZCL level or colour transition is rendered.
 *
 * A transition used to advance once per decisecond, because the ZCL
 * RemainingTime attribute doubled as the loop counter. That made a 5 s fade
 * 50 output updates, which reads as visibly choppy - the observation that
 * started build 38. The ramps now run on their own tick and RemainingTime is
 * derived from it, so the attribute keeps its ZCL meaning (1/10 s) while the
 * output moves five times as often.
 *
 * 20 ms is not a guess: the light-show engine (MOES_EFFECT_TICK_MS) already
 * sustains exactly this rate through the same moes_outSet() -> 5x
 * drv_pwm_cfg() path, on this part, in the field. Do not raise it without
 * measuring, and note that a level ramp and a colour ramp run as two separate
 * timers out of a 24-entry TL_ZB_TIMER pool. */
#define MOES_RAMP_TICK_MS               20
#define MOES_RAMP_STEPS_PER_DS          (100 / MOES_RAMP_TICK_MS)


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
 * level are applied here, for both ZCL control and the effect engine.
 *
 * moes_outSet() is the u8 form every existing caller uses; moes_outSet256()
 * is the same thing in the wide 8.8 domain, which is what a transition needs
 * so it does not throw away 99% of the PWM resolution on the way out. */
void moes_outSet(u8 r, u8 g, u8 b, u8 cw, u8 ww);
void moes_outSet256(u16 r, u16 g, u16 b, u16 cw, u16 ww);
void temperatureToCW256(u16 temperatureMireds, u16 level256, u16 *C256, u16 *W256);
void pwmSetDuty(u8 ch, u16 dutycycle);

/* Build 38. The live level in 8.8, for the render path.
 *
 * `level` is the u8 the caller is about to render. If the level module's 8.8
 * accumulator rounds to that same level it is returned, giving the render path
 * the sub-level precision a transition carries; otherwise the accumulator is
 * stale - something wrote curLevel directly, as scene recall, the Tuya
 * datapoints and the effect-stop path all do - and the plain attribute is
 * returned instead. Defined in zcl_levelCb.c. */
u16 tuyaLight_levelWiden(u8 level);

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
