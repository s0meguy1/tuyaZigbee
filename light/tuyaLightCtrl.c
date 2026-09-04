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
#include "moes_dim.h"
#include "moes_flashcfg.h"
#include "light_effects.h"
#include "moes_color.h"


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
/* Set while light_fresh() stops a running effect. lightFx_stop()
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
 * @fn      moes_duty256
 *
 * @brief   Wide-domain input -> PWM duty with active-level inversion.
 *
 *          Build 38: the inversion happens in the wide domain so it cannot
 *          re-quantize what the curve just produced. The u8 version also had
 *          to scale by PWM_FULL_DUTYCYCLE to survive pwmSetDuty()'s divisor;
 *          nothing in the wide path needs that, so an earlier /2 bug of the
 *          same family cannot recur.
 */
static u32 moes_duty256(moes_chan_t *ch, u32 v256){
	return moes_dimInvert256(v256, ch->activeLow);
}

/* Stock's brightness curve.
 *
 * The factory config block at 0xF8000 carries brightmin:1 and brightmax:100,
 * and stock maps the ZCL level linearly into that percent span before driving
 * PWM. We previously squared the level instead, described in a comment as
 * "matches perceived brightness, as stock/upstream" - the perceptual claim is
 * true, but the claim about stock is not, and the difference is severe at the
 * bottom of the range where a downlight actually lives:
 *
 *     level  ours (squared)   stock (linear)
 *       51        4%              21%          5x, plainly visible
 *      191       56%              75%          1.3x, not visible
 *
 * Measured on hardware against stock fixtures in the same fixture group: a
 * converted bulb at level 117 renders identically to stock bulbs at level 51,
 * and 117 is exactly the level our squared curve needed to reach stock's 21%.
 * Both the matching and the mismatching case were confirmed by eye.
 *
 * The constants are compiled in rather than parsed. The config block is
 * byte-identical across every unit examined, down to its crc, so there is
 * nothing per-unit to read, and moes_flashCfgLoad() deliberately keeps a JSON
 * parser off the boot path.
 *
 * Zero must stay zero: brightmin is a dimming floor for a lit channel, not an
 * output floor, and hwLight_onOffUpdate() drives Off through this path.
 *
 * Build 38: the curve itself now lives in moes_dim.c, so tools/
 * level_curve_hosttest executes the real function rather than a copy, and it
 * is evaluated in 8.8 instead of through whole percents. The SHAPE is
 * unchanged - the same linear brightmin..brightmax ramp - but it is evaluated
 * exactly where the old code rounded twice, so 256 inputs no longer collapse
 * onto 101 outputs. Rendered duty moves by at most 3/255 of full scale at any
 * one level, which is under the visible threshold at a steady brightness and
 * is a move toward the design intent, not away from it. Do NOT make it
 * perceptual; see above for what that cost last time.
 */

/*********************************************************************
 * @fn      pwmSetDuty256
 *
 * @brief   The single remaining quantizer: wide duty -> hardware compare
 *          ticks. moes_pwmMaxTick is 12000 on this part (48 MHz system clock,
 *          4 kHz PWM), so the output stage resolves ~12000 steps where the u8
 *          chain resolved 101.
 */
static void pwmSetDuty256(u8 ch, u32 v256)
{
	u32 cmp_tick = moes_dimCmpTick(v256, moes_pwmMaxTick);
	drv_pwm_cfg(ch, (u16)cmp_tick, (u16)moes_pwmMaxTick);
}

/*********************************************************************
 * @fn      moes_outSet256
 *
 * @brief   The single 5-channel output point, in the wide domain. Each
 *          argument is an 0..255 channel level carried in 8.8 fixed point,
 *          i.e. 0..MOES_DIM_MAX256. Gamma and white-balance trim are applied
 *          here, both without an intermediate u8.
 */
void moes_outSet256(u16 r, u16 g, u16 b, u16 cw, u16 ww)
{
	u32 r32 = r;
	u32 g32 = g;
	u32 b32 = b;

	/* white-balance trim on the RGB channels (stock gmwr/gmwg/gmwb) */
	if(g_moesCfg.valid){
		r32 = (r32 * g_moesCfg.gmwr) / 100;
		g32 = (g32 * g_moesCfg.gmwg) / 100;
		b32 = (b32 * g_moesCfg.gmwb) / 100;
	}

	pwmSetDuty256(moes_chan[0].pwmChannel, moes_duty256(&moes_chan[0], moes_dimCurve256(r32)));
	pwmSetDuty256(moes_chan[1].pwmChannel, moes_duty256(&moes_chan[1], moes_dimCurve256(g32)));
	pwmSetDuty256(moes_chan[2].pwmChannel, moes_duty256(&moes_chan[2], moes_dimCurve256(b32)));
	pwmSetDuty256(moes_chan[3].pwmChannel, moes_duty256(&moes_chan[3], moes_dimCurve256(cw)));
	pwmSetDuty256(moes_chan[4].pwmChannel, moes_duty256(&moes_chan[4], moes_dimCurve256(ww)));
}

/*********************************************************************
 * @fn      moes_outSet
 *
 * @brief   u8 entry point, unchanged for every existing caller - the
 *          light-show engine included. Widening is exact: 255 maps to
 *          MOES_DIM_MAX256, so full scale stays full scale.
 */
void moes_outSet(u8 r, u8 g, u8 b, u8 cw, u8 ww)
{
	moes_outSet256((u16)r << 8, (u16)g << 8, (u16)b << 8, (u16)cw << 8, (u16)ww << 8);
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

	/* NOT REACHED on a colour light, verified on hardware 2026-09-01.
	 *
	 * The only caller is tuyaLight_updateLevel(), which light_fresh() invokes
	 * solely in the #else of #ifdef ZCL_LIGHT_COLOR_CONTROL. That symbol is
	 * defined for the TS0505B, so this function has no caller here; the render
	 * path is tuyaLight_updateColor() -> hwLight_colorUpdate_*().
	 *
	 * Recorded because build 35 briefly added an on/off guard here on the
	 * theory that a scene stored OFF came back lit and that a group level ramp
	 * woke dark fixtures. Both were tested on real fixtures against unpatched
	 * neighbours and neither reproduced - because light_fresh() applies
	 * tuyaLight_updateOnOff() LAST, so an off lamp is re-zeroed after any
	 * level or colour work. Do not re-add a guard here expecting a behaviour
	 * change; fix the reachable render path instead. */
	if(pColor->colorMode == ZCL_COLOR_MODE_COLOR_TEMPERATURE_MIREDS){
		hwLight_colorUpdate_colorTemperature(pColor->colorTemperatureMireds, level);
	}else{
		hwLight_colorUpdate_HSV2RGB(pColor->currentHue, pColor->currentSaturation, level);
	}
}

/*********************************************************************
 * @fn      temperatureToCW256
 *
 * @brief   Build 38. Carries the 8.8 level into the cool/warm split.
 *
 *          The u8 version divided a u8 level into u8 C and W with integer
 *          division, which is where the bottom of a fade fell apart: at
 *          230 mireds, ZCL levels 3,4,5 and 6 all produced the same pair and
 *          therefore the same PWM output, so a fade-to-off ended in four
 *          visible jumps instead of a ramp.
 */
void temperatureToCW256(u16 temperatureMireds, u16 level256, u16 *C256, u16 *W256)
{
	zcl_lightColorCtrlAttr_t *pColor = zcl_colorAttrGet();

	moes_dimSplitCW256(temperatureMireds,
					   pColor->colorTempPhysicalMinMireds, pColor->colorTempPhysicalMaxMireds,
					   level256, C256, W256);
}

/*********************************************************************
 * @fn      hwLight_colorUpdate_colorTemperature
 */
void hwLight_colorUpdate_colorTemperature(u16 colorTemperatureMireds, u8 level)
{
	u16 C256 = 0;
	u16 W256 = 0;
	u16 level256;

	/* Upstream clamped every level below 0x0A up to 0x0A, so ZCL levels 1-9 all
	 * rendered identically at 10. That is a real fidelity loss on this fleet:
	 * porch_lights_night is stored at brightness 3 and was rendering at 10, and
	 * a light-show fade-out stepped off instead of fading. Honour the ZCL
	 * minimum instead; level 0 is Off and is handled by the on/off path. */
	if(level < ZCL_LEVEL_ATTR_MIN_LEVEL){ level = ZCL_LEVEL_ATTR_MIN_LEVEL; }

	/* Build 38: render the live 8.8 level rather than its rounded u8. During a
	 * transition that is the difference between ~50 and ~250 distinct outputs
	 * over a 5 s fade. tuyaLight_levelWiden() falls back to the plain attribute
	 * whenever the two disagree by more than one level, so a scene recall, a
	 * Tuya datapoint or an effect stop - all of which write curLevel directly -
	 * can never leave a stale sub-level on the output. */
#ifdef ZCL_LEVEL_CTRL
	level256 = tuyaLight_levelWiden(level);
#else
	level256 = ((u16)level) << 8;
#endif

	temperatureToCW256(colorTemperatureMireds, level256, &C256, &W256);
	moes_outSet256(0, 0, 0, C256, W256);
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
 * @fn      xyToHueSat
 *
 * @brief   Thin wrapper over the standalone, host-tested colour maths in
 *          moes_color.c. The conversion lives there so tools/color_hosttest
 *          executes the same code this firmware runs, rather than a copy.
 */
void xyToHueSat(u16 x, u16 y, u8 *hue, u8 *saturation)
{
	/* moes_color.h cannot include the ZCL headers without losing its
	 * host-testability, so prove here that its scale constants still match the
	 * ZCL ones the rest of the output stage uses. */
	STATIC_ASSERT(MOES_COLOR_HUE_MAX == ZCL_COLOR_ATTR_HUE_MAX);
	STATIC_ASSERT(MOES_COLOR_SAT_MAX == ZCL_COLOR_ATTR_SATURATION_MAX);

	moes_xyToHueSat(x, y, hue, saturation);
}

/*********************************************************************
 * @fn      hwLight_colorUpdate_HSV2RGB
 */
void hwLight_colorUpdate_HSV2RGB(u8 hue, u8 saturation, u8 level)
{
	u8 R = 0, G = 0, B = 0;

	/* Same upstream 0x0A floor as the colour-temperature path; see the comment
	 * there. Kept symmetric so a dim RGB scene and a dim CCT scene behave the
	 * same way. */
	if(level < ZCL_LEVEL_ATTR_MIN_LEVEL){ level = ZCL_LEVEL_ATTR_MIN_LEVEL; }

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
	 * runs outside the effect-stop path (boot, or lightFx_stop() from
	 * the effect command). The effect-stop path below sets lightFreshStopFx so
	 * lightFx_stop() does not re-enter through light_adjust(), but keep
	 * the explicit bound for the remaining paths instead of relying on
	 * statement order in another file. */
	static u8 inLightFresh = 0;

	if(inLightFresh >= 2){
		return;
	}

	inLightFresh++;

	/* Any ZCL-driven update is an explicit user action: it takes the
	 * output back from a running effect. Stop the effect quietly: the
	 * transition state in colorInfo belongs to the colour command that is
	 * being processed, so light_adjust() must not re-init it here.
	 *
	 * Build 36: unless the show asked to hold the output (takeover policy 1).
	 * Then the ZCL attributes have already been updated by the caller and are
	 * simply not rendered: the effect keeps the LEDs, reads the new values on
	 * its next frame where it follows them, and the fixture lands on them
	 * when the effect stops. Persist them as usual. */
	if(lightFx_active()){
		if(lightFx_holdsOutput()){
			gLightCtx.lightAttrsChanged = TRUE;
			inLightFresh--;
			return;
		}
		lightFreshStopFx = TRUE;
		lightFx_stop(TRUE);
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
