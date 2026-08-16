/********************************************************************************************************
 * @file    app_ui.c
 *
 * @brief   This is the source file for app_ui
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
#include "tuyaLight.h"
#include "app_ui.h"
#include "gp.h"
/**********************************************************************
 * LOCAL CONSTANTS
 */


/**********************************************************************
 * TYPEDEFS
 */


/**********************************************************************
 * GLOBAL VARIABLES
 */


/**********************************************************************
 * LOCAL FUNCTIONS
 */
void led_on(u32 pin){
	drv_gpio_write(pin, LED_ON);
}

void led_off(u32 pin){
	drv_gpio_write(pin, LED_OFF);
}

void led_init(void){
	led_off(LED_POWER);
	led_off(LED_PERMIT);
}

void localPermitJoinState(void){
	static bool assocPermit = 0;
	if(assocPermit != zb_getMacAssocPermit()){
		assocPermit = zb_getMacAssocPermit();
		if(assocPermit){
			led_on(LED_PERMIT);
		}else{
			led_off(LED_PERMIT);
		}
	}
}

void buttonKeepPressed(u8 btNum){
	if(btNum == VK_SW1){
		gLightCtx.state = APP_FACTORY_NEW_DOING;
		zb_factoryReset();
	}else if(btNum == VK_SW2){

	}
}

void buttonShortPressed(u8 btNum){
	if(btNum == VK_SW1){
		if(zb_isDeviceJoinedNwk()){
			gLightCtx.sta = !gLightCtx.sta;
			if(gLightCtx.sta){
				tuyaLight_onoff(ZCL_ONOFF_STATUS_ON);
			}else{
				tuyaLight_onoff(ZCL_ONOFF_STATUS_OFF);
			}
		}
	}else if(btNum == VK_SW2){
		/* toggle local permit Joining */
		u8 duration = zb_getMacAssocPermit() ? 0 : 180;
		zb_nlmePermitJoiningRequest(duration);

		gpsCommissionModeInvork();
	}
}

void keyScan_keyPressedCB(kb_data_t *kbEvt){
//	u8 toNormal = 0;
	u8 keyCode = kbEvt->keycode[0];
//	static u8 lastKeyCode = 0xff;

	buttonShortPressed(keyCode);

	if(keyCode == VK_SW1){
		gLightCtx.keyPressedTime = clock_time();
		gLightCtx.state = APP_FACTORY_NEW_SET_CHECK;
	}
}


void keyScan_keyReleasedCB(u8 keyCode){
	gLightCtx.state = APP_STATE_NORMAL;
}

volatile u8 T_keyPressedNum = 0;
void app_key_handler(void){
#if !HAVE_NET_BUTTON
	/* MOES: boards with no button must never run the key scanner.
	 *
	 * kb_key_pressed() treats a scan pin reading LOW as "pressed"
	 * (KB_LINE_HIGH_VALID == 0). A scan pin that is not explicitly given
	 * PXX_INPUT_ENABLE 1 + PULL_WAKEUP_SRC_PXX has its input buffer disabled
	 * and no pull resistor, so gpio_read_all() returns 0 for it - i.e. a
	 * permanently held key from the second poll after boot. Five seconds
	 * later buttonKeepPressed(VK_SW1) calls zb_factoryReset(), the device
	 * reboots, and it does it again. That is an unrecoverable reset loop on
	 * a device that can only be repaired over the air.
	 *
	 * The scanner stays compiled (drv_keyboard.c needs KB_SCAN_PINS to exist
	 * for kb_event/kb_scan_key to link) but is never entered. */
	return;
#else
	static u8 valid_keyCode = 0xff;

	if(gLightCtx.state == APP_FACTORY_NEW_SET_CHECK){
		if(clock_time_exceed(gLightCtx.keyPressedTime, 5*1000*1000)){
			buttonKeepPressed(VK_SW1);
		}
	}

	if(kb_scan_key(0 , 1)){
		T_keyPressedNum++;
		if(kb_event.cnt){
			keyScan_keyPressedCB(&kb_event);
			if(kb_event.cnt == 1){
				valid_keyCode = kb_event.keycode[0];
			}
		}else{
			keyScan_keyReleasedCB(valid_keyCode);
			valid_keyCode = 0xff;
		}
	}
#endif	/* HAVE_NET_BUTTON */
}

#endif  /* __PROJECT_TL_DIMMABLE_LIGHT__ */
