/********************************************************************************************************
 * @file    tuyaLightCtrl.c
 *
 * @brief   5-channel extended-colour output stage for the Moes TS0505B
 * (RGBCW). Replaces the upstream RGB-xor-CCT control layer: both colour
 * modes live side by side and the ZCL colourMode attribute decides which
 * drives the output. Pin assignment, active levels, PWM frequency and
 * white-balance trim come from the factory JSON block at boot (fallback
 * to compiled defaults). The effect engine writes the same output stage
 * through moes_outSet().
 *
 * @date    2026 (original: Zigbee Group / doctor64, Apache-2.0)
 *******************************************************************************************************/

#if (__PROJECT_TL_DIMMABLE_LIGHT__)

/**********************************************************************
 * INCLUDES
 */
#include "../common/comm_cfg.h"
#include "tl_common.h"
#include "zcl_include.h"
#include "tuyaLight.h"
#include "tuyaLightCtrl.h"
#include "moes_flashcfg.h"
#include "light_effects.h"
#include "moes_rescue.h"


/**********************************************************************
 * LOCAL CONSTANTS
 */
#define PWM_FULL_DUTYCYCLE				100
#define PMW_MAX_TICK		            (PWM_CLOCK_SOURCE / MOES_PWM_FREQUENCY_DEFAULT)

/**********************************************************************
 * TYPEDEFS
 */


/**********************************************************************
 * GLOBAL VARIABLES
 */

/* runtime channel config, filled from the factory JSON in hwLight_init */
typedef struct {
	u32 gpio;
	u8  pwmChannel;
	u16 pwmMux;
	u8  activeLow;
} moes_chan_t;

static moes_chan_t moes_chan[5];   /* index: 0=R 1=G 2=B 3=CW 4=WW */
static u16 moes_pwmMaxTick = PMW_MAX_TICK;
/* Set while light_fresh() stops a running effect. lightFx_start(STEADY)
 * would otherwise call light_adjust() -> tuyaLight_colorInit(), which resets
 * the same colorInfo transition state the outer ZCL colour handler has just
 * populated. Skipping that re-init keeps the requested fade intact. */
static bool lightFreshStopFx = FALSE;

/**********************************************************************
 * FUNCTIONS
 */

extern void tuyaLight_updateOnOff(void);
extern void tuyaLight_updateLevel(void);
extern void tuyaLight_updateColor(void);

extern void tuyaLight_onOffInit(void);
extern void tuyaLight_levelInit(void);
extern void tuyaLight_colorInit(void);

/*********************************************************************
 * @fn      pwmSetDuty
 *
 * @brief   dutycycle is 0..10000 (level 0..255 * PWM_FULL_DUTYCYCLE-ish);
 * 			kept compatible with the upstream callers which pass
 * 			gammaValue * PWM_FULL_DUTYCYCLE where gammaValue is 0..255.
 */
void pwmSetDuty(u8 ch, u16 dutycycle)
{
	u32 cmp_tick = ((u32)dutycycle * moes_pwmMaxTick) / (ZCL_LEVEL_ATTR_MAX_LEVEL * PWM_FULL_DUTYCYCLE);
	drv_pwm_cfg(ch, (u16)cmp_tick, (u16)moes_pwmMaxTick);
}

void pwmInit(u8 ch, u16 dutycycle)
{
	pwmSetDuty(ch, dutycycle);
}

/*********************************************************************
 * @fn      moes_chanInit
 *
 * @brief   Resolve one channel from the factory JSON pin number, with
 *          compiled-default fallback.
 */
static bool moes_chanInit(moes_chan_t *ch, u8 modulePin, u8 activeLevel,
						  u32 defGpio, u8 defPwmCh, u16 defMux, u8 defActiveLow){
	moes_pinmap_t map;
	if(moes_pinmapLookup(modulePin, &map)){
		ch->gpio = map.gpio;
		ch->pwmChannel = map.pwmChannel;
		ch->pwmMux = map.pwmMux;
		ch->activeLow = (activeLevel == 0);
		return TRUE;
	}
	ch->gpio = defGpio;
	ch->pwmChannel = defPwmCh;
	ch->pwmMux = defMux;
	ch->activeLow = defActiveLow;
	return FALSE;
}

/*********************************************************************
 * @fn      moes_duty
 *
 * @brief   0..255 input -> PWM duty with active-level inversion.
 */
static u16 moes_duty(moes_chan_t *ch, u8 v){
	if(ch->activeLow){
		v = 255 - v;
	}
	/* pwmSetDuty() divides by (ZCL_LEVEL_ATTR_MAX_LEVEL * PWM_FULL_DUTYCYCLE),
	 * so full scale is v * PWM_FULL_DUTYCYCLE - same as upstream. An earlier
	 * /2 here (a bogus "fits in u16" guard; 255*100 = 25500 fits fine) capped
	 * every channel at 50% duty. */
	return (u16)v * PWM_FULL_DUTYCYCLE;
}

/*********************************************************************
 * @fn      moes_outSet
 *
 * @brief   The single output point for normal control AND the effect
 *          engine. r,g,b,cw,ww are 0..255 pre-gamma? No: raw linear.
 *          Gamma and white-balance trim are applied here.
 */
void moes_outSet(u8 r, u8 g, u8 b, u8 cw, u8 ww)
{
	/* white-balance trim on the RGB channels (stock gmwr/gmwg/gmwb) */
	if(g_moesCfg.valid){
		r = (u16)r * g_moesCfg.gmwr / 100;
		g = (u16)g * g_moesCfg.gmwg / 100;
		b = (u16)b * g_moesCfg.gmwb / 100;
	}

	/* quadratic gamma: matches perceived brightness, as stock/upstream */
	u8 gr = ((u16)r * r) / 255;
	u8 gg = ((u16)g * g) / 255;
	u8 gb = ((u16)b * b) / 255;
	u8 gc = ((u16)cw * cw) / 255;
	u8 gw = ((u16)ww * ww) / 255;

	pwmSetDuty(moes_chan[0].pwmChannel, moes_duty(&moes_chan[0], gr));
	pwmSetDuty(moes_chan[1].pwmChannel, moes_duty(&moes_chan[1], gg));
	pwmSetDuty(moes_chan[2].pwmChannel, moes_duty(&moes_chan[2], gb));
	pwmSetDuty(moes_chan[3].pwmChannel, moes_duty(&moes_chan[3], gc));
	pwmSetDuty(moes_chan[4].pwmChannel, moes_duty(&moes_chan[4], gw));
}

/*********************************************************************
 * @fn      hwLight_init
 */
void hwLight_init(void)
{
	moes_flashCfgLoad();

	moes_chanInit(&moes_chan[0], g_moesCfg.pin_r, g_moesCfg.lv_r, LED_R, PWM_R_CHANNEL, AS_PWM4, 0);
	moes_chanInit(&moes_chan[1], g_moesCfg.pin_g, g_moesCfg.lv_g, LED_G, PWM_G_CHANNEL, AS_PWM1, 0);
	moes_chanInit(&moes_chan[2], g_moesCfg.pin_b, g_moesCfg.lv_b, LED_B, PWM_B_CHANNEL, AS_PWM3, 0);
	moes_chanInit(&moes_chan[3], g_moesCfg.pin_c, g_moesCfg.lv_c, LED_CW, PWM_CW_CHANNEL, AS_PWM5, 0);
	moes_chanInit(&moes_chan[4], g_moesCfg.pin_w, g_moesCfg.lv_w, LED_WW, PWM_WW_CHANNEL, AS_PWM0, 0);

	if(g_moesCfg.valid && g_moesCfg.pwmhz){
		moes_pwmMaxTick = PWM_CLOCK_SOURCE / g_moesCfg.pwmhz;
		if(moes_pwmMaxTick < 0x400){  /* sanity: >= 1024 ticks */
			moes_pwmMaxTick = PMW_MAX_TICK;
		}
	}

	drv_pwm_init();

	for(u8 i = 0; i < 5; i++){
		gpio_set_func(moes_chan[i].gpio, moes_chan[i].pwmMux);
		pwmInit(moes_chan[i].pwmChannel, 0);
		drv_pwm_start(moes_chan[i].pwmChannel);
	}
}

/*********************************************************************
 * @fn      hwLight_onOffUpdate
 *
 * @brief   Hard output gate. If an effect is running, an Off command
 *          stops the show first (handled by light_fresh); a blink
 *          (identify) still needs the raw gate, so we keep it direct.
 */
void hwLight_onOffUpdate(u8 onOff)
{
	if(onOff){
		for(u8 i = 0; i < 5; i++){
			drv_pwm_start(moes_chan[i].pwmChannel);
		}
	}else{
		moes_outSet(0, 0, 0, 0, 0);
	}
}

/*********************************************************************
 * @fn      hwLight_levelUpdate
 *
 * @brief   Level alone (colour-mode unaware callers): route by the
 *          current colour mode so the right channels dim.
 */
void hwLight_levelUpdate(u8 level)
{
	zcl_lightColorCtrlAttr_t *pColor = zcl_colorAttrGet();

	if(pColor->colorMode == ZCL_COLOR_MODE_COLOR_TEMPERATURE_MIREDS){
		hwLight_colorUpdate_colorTemperature(pColor->colorTemperatureMireds, level);
	}else{
		hwLight_colorUpdate_HSV2RGB(pColor->currentHue, pColor->currentSaturation, level);
	}
}

/*********************************************************************
 * @fn      temperatureToCW
 */
void temperatureToCW(u16 temperatureMireds, u8 level, u8 *C, u8 *W)
{
	zcl_lightColorCtrlAttr_t *pColor = zcl_colorAttrGet();

	if(temperatureMireds < pColor->colorTempPhysicalMinMireds){
		temperatureMireds = pColor->colorTempPhysicalMinMireds;
	}
	if(temperatureMireds > pColor->colorTempPhysicalMaxMireds){
		temperatureMireds = pColor->colorTempPhysicalMaxMireds;
	}

	*W = (u8)(((u32)(temperatureMireds - pColor->colorTempPhysicalMinMireds) * level) /
			  (pColor->colorTempPhysicalMaxMireds - pColor->colorTempPhysicalMinMireds));
	*C = level - (*W);
}

/*********************************************************************
 * @fn      hwLight_colorUpdate_colorTemperature
 */
void hwLight_colorUpdate_colorTemperature(u16 colorTemperatureMireds, u8 level)
{
	u8 C = 0;
	u8 W = 0;

	if(level < 0x0A){ level = 0x0A; }

	temperatureToCW(colorTemperatureMireds, level, &C, &W);
	moes_outSet(0, 0, 0, C, W);
}

/*********************************************************************
 * @fn      hsvToRGB (kept API: upstream integer HSV)
 */
void hsvToRGB(u8 hue, u8 saturation, u8 level, u8 *R, u8 *G, u8 *B)
{
	u8 region;
	u8 remainder;
	u8 p, q, t;

	u16 rHue = (u16)hue * 360 / ZCL_COLOR_ATTR_HUE_MAX;
	u8 rS = saturation;
	u8 rV = level;

	if(saturation == 0){
		*R = rV; *G = rV; *B = rV;
		return;
	}

	region = (rHue < 360) ? (rHue / 60) : 0;
	remainder = (rHue - (region * 60)) * 4;

	p = (u8)(((u16)rV * (255 - rS)) >> 8);
	q = (u8)(((u16)rV * (255 - ((u16)rS * remainder >> 8))) >> 8);
	t = (u8)(((u16)rV * (255 - ((u16)rS * (255 - remainder) >> 8))) >> 8);

	switch(region){
	case 0:  *R = rV; *G = t;  *B = p;  break;
	case 1:  *R = q;  *G = rV; *B = p;  break;
	case 2:  *R = p;  *G = rV; *B = t;  break;
	case 3:  *R = p;  *G = q;  *B = rV; break;
	case 4:  *R = t;  *G = p;  *B = rV; break;
	default: *R = rV; *G = p;  *B = q;  break;
	}
}

/*********************************************************************
 * @fn      hwLight_colorUpdate_HSV2RGB
 */
void hwLight_colorUpdate_HSV2RGB(u8 hue, u8 saturation, u8 level)
{
	u8 R = 0, G = 0, B = 0;

	if(level < 0x0A){ level = 0x0A; }

	hsvToRGB(hue, saturation, level, &R, &G, &B);
	moes_outSet(R, G, B, 0, 0);
}

/*********************************************************************
 * @fn      light_adjust / light_fresh
 */
void light_adjust(void)
{
	if(lightFreshStopFx){
		return;
	}

#ifdef ZCL_LIGHT_COLOR_CONTROL
	tuyaLight_colorInit();
#else
#ifdef ZCL_LEVEL_CTRL
	tuyaLight_levelInit();
#endif
#endif
	tuyaLight_onOffInit();
}

void light_fresh(void)
{
	/* Re-entry guard. light_adjust() -> tuyaLight_colorInit() ->
	 * light_applyUpdate() -> light_fresh() is a real cycle when light_adjust()
	 * runs outside the effect-stop path (boot, or lightFx_start(STEADY) from
	 * the effect command). The effect-stop path below sets lightFreshStopFx so
	 * lightFx_start(STEADY) does not re-enter through light_adjust(), but keep
	 * the explicit bound for the remaining paths instead of relying on
	 * statement order in another file. */
	static u8 inLightFresh = 0;

	if(inLightFresh >= 2){
		return;
	}

#if MOES_TS0505B
	/* Rescue mode owns the output stage. This is the single choke point for
	 * every ZCL-driven update - on/off, level, hue, saturation, colour
	 * temperature, scene recall - so blocking it here is what makes the claim
	 * in FALLBACK_DESIGN.md S2 true: in rescue mode the output is written
	 * exactly once, at boot, and never again. Otherwise a "turn on" from
	 * zigbee2mqtt would walk straight back into the colour path, which is
	 * precisely the code rescue mode exists not to depend on.
	 *
	 * The ZCL attributes still update normally - reads and reports are
	 * truthful, the light simply does not act on them. */
	if(moes_rescueActive()){
		return;
	}
#endif

	inLightFresh++;

	/* Any ZCL-driven update is an explicit user action: it takes the
	 * output back from a running effect. Stop the effect quietly: the
	 * transition state in colorInfo belongs to the colour command that is
	 * being processed, so light_adjust() must not re-init it here. */
	if(lightFx_active()){
		lightFreshStopFx = TRUE;
		lightFx_start(MOES_EF_STEADY, g_moesFx.speed, g_moesFx.phase);
		lightFreshStopFx = FALSE;
	}

#ifdef ZCL_LIGHT_COLOR_CONTROL
	tuyaLight_updateColor();
#else
#ifdef ZCL_LEVEL_CTRL
	tuyaLight_updateLevel();
#else
	pwmSetDuty(moes_chan[3].pwmChannel, ZCL_LEVEL_ATTR_MAX_LEVEL * PWM_FULL_DUTYCYCLE);
#endif
#endif
	tuyaLight_updateOnOff();

	gLightCtx.lightAttrsChanged = TRUE;

	inLightFresh--;
}

/*********************************************************************
 * @fn      light_applyUpdate (upstream, unchanged)
 */
void light_applyUpdate(u8 *curLevel, u16 *curLevel256, s32 *stepLevel256, u16 *remainingTime, u8 minLevel, u8 maxLevel, bool wrap)
{
	if((*stepLevel256 > 0) && ((((s32)*curLevel256 + *stepLevel256) / 256) > maxLevel)){
		*curLevel256 = (wrap) ? ((u16)minLevel * 256 + ((*curLevel256 + *stepLevel256) - (u16)maxLevel * 256) - 256)
							  : ((u16)maxLevel * 256);
	}else if((*stepLevel256 < 0) && ((((s32)*curLevel256 + *stepLevel256) / 256) < minLevel)){
		*curLevel256 = (wrap) ? ((u16)maxLevel * 256 - ((u16)minLevel * 256 - ((s32)*curLevel256 + *stepLevel256)) + 256)
							  : ((u16)minLevel * 256);
	}else{
		*curLevel256 += *stepLevel256;
	}

	if(*stepLevel256 > 0){
		*curLevel = (*curLevel256 + 127) / 256;
	}else{
		*curLevel = *curLevel256 / 256;
	}

	if(*remainingTime == 0){
		*curLevel256 = ((u16)*curLevel) * 256;
		*stepLevel256 = 0;
	}else if(*remainingTime != 0xFFFF){
		*remainingTime = *remainingTime -1;
	}

	light_fresh();
}

void light_applyUpdate_16(u16 *curLevel, u32 *curLevel256, s32 *stepLevel256, u16 *remainingTime, u16 minLevel, u16 maxLevel, bool wrap)
{
	if((*stepLevel256 > 0) && ((((s32)*curLevel256 + *stepLevel256) / 256) > maxLevel)){
		*curLevel256 = (wrap) ? ((u32)minLevel * 256 + ((*curLevel256 + *stepLevel256) - (u32)maxLevel * 256) - 256)
							  : ((u32)maxLevel * 256);
	}else if((*stepLevel256 < 0) && ((((s32)*curLevel256 + *stepLevel256) / 256) < minLevel)){
		*curLevel256 = (wrap) ? ((u32)maxLevel * 256 - ((u32)minLevel * 256 - ((s32)*curLevel256 + *stepLevel256)) + 256)
							  : ((u32)minLevel * 256);
	}else{
		*curLevel256 += *stepLevel256;
	}

	if(*stepLevel256 > 0){
		*curLevel = (*curLevel256 + 127) / 256;
	}else{
		*curLevel = *curLevel256 / 256;
	}

	if(*remainingTime == 0){
		*curLevel256 = ((u32)*curLevel) * 256;
		*stepLevel256 = 0;
	}else if(*remainingTime != 0xFFFF){
		*remainingTime = *remainingTime -1;
	}

	light_fresh();
}

/*********************************************************************
 * @fn      light_blink (upstream, unchanged apart from NULL-safe LEDs)
 */
s32 light_blink_TimerEvtCb(void *arg)
{
	u32 interval = 0;

	if(gLightCtx.sta == gLightCtx.oriSta){
		if(gLightCtx.times){
			gLightCtx.times--;
			if(gLightCtx.times <= 0){
				if(gLightCtx.oriSta){
					hwLight_onOffUpdate(ZCL_CMD_ONOFF_ON);
				}else{
					hwLight_onOffUpdate(ZCL_CMD_ONOFF_OFF);
				}

				gLightCtx.timerLedEvt = NULL;
				return -1;
			}
		}
	}

	gLightCtx.sta = !gLightCtx.sta;
	if(gLightCtx.sta){
		hwLight_onOffUpdate(ZCL_CMD_ONOFF_ON);
		interval = gLightCtx.ledOnTime;
	}else{
		hwLight_onOffUpdate(ZCL_CMD_ONOFF_OFF);
		interval = gLightCtx.ledOffTime;
	}

	return interval;
}

void light_blink_start(u8 times, u16 ledOnTime, u16 ledOffTime)
{
	u32 interval = 0;
	zcl_onOffAttr_t *pOnoff = zcl_onoffAttrGet();

#if MOES_TS0505B
	/* Identify would schedule a timer and drive hwLight_onOffUpdate() behind
	 * light_fresh()'s back. In rescue mode the steady dim-white output *is*
	 * the diagnostic, so keep it steady. */
	if(moes_rescueActive()){
		return;
	}
#endif

	gLightCtx.oriSta = pOnoff->onOff;
	gLightCtx.times = times;

	if(!gLightCtx.timerLedEvt){
		if(gLightCtx.oriSta){
			hwLight_onOffUpdate(ZCL_CMD_ONOFF_OFF);
			gLightCtx.sta = 0;
			interval = ledOffTime;
		}else{
			hwLight_onOffUpdate(ZCL_CMD_ONOFF_ON);
			gLightCtx.sta = 1;
			interval = ledOnTime;
		}
		gLightCtx.ledOnTime = ledOnTime;
		gLightCtx.ledOffTime = ledOffTime;

		gLightCtx.timerLedEvt = TL_ZB_TIMER_SCHEDULE(light_blink_TimerEvtCb, NULL, interval);
	}
}

void light_blink_stop(void)
{
	if(gLightCtx.timerLedEvt){
		TL_ZB_TIMER_CANCEL(&gLightCtx.timerLedEvt);

		gLightCtx.times = 0;
		if(gLightCtx.oriSta){
			hwLight_onOffUpdate(ZCL_CMD_ONOFF_ON);
		}else{
			hwLight_onOffUpdate(ZCL_CMD_ONOFF_OFF);
		}
	}
}

#endif	/* __PROJECT_TL_DIMMABLE_LIGHT__ */
