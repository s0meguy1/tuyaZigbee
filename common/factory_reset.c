/********************************************************************************************************
 * @file    factory_reset.c
 *
 * @brief   This is the source file for factory_reset
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

#include "tl_common.h"
#include "factory_reset.h"
#include "zb_api.h"

/* device_config may override both (Moes TS0505B: stock rstnum:3 gesture) */
#ifndef FACTORY_RESET_POWER_CNT_THRESHOLD
#define FACTORY_RESET_POWER_CNT_THRESHOLD		10	//times
#endif
#ifndef FACTORY_RESET_TIMEOUT
#define FACTORY_RESET_TIMEOUT					2	//second
#endif

ev_timer_event_t *factoryRst_timerEvt = NULL;
u8 factoryRst_powerCnt = 0;
bool factoryRst_exist = FALSE;

nv_sts_t factoryRst_powerCntSave(void){
	nv_sts_t st = NV_SUCC;
#if NV_ENABLE
	st = nv_flashWriteNew(1, NV_MODULE_APP, NV_ITEM_APP_POWER_CNT, 1, &factoryRst_powerCnt);
#else
	st = NV_ENABLE_PROTECT_ERROR;
#endif
	return st;
}

nv_sts_t factoryRst_powerCntRestore(void){
	nv_sts_t st = NV_SUCC;
#if NV_ENABLE
	st = nv_flashReadNew(1, NV_MODULE_APP, NV_ITEM_APP_POWER_CNT, 1, &factoryRst_powerCnt);
#else
	st = NV_ENABLE_PROTECT_ERROR;
#endif
	return st;
}

static s32 factoryRst_timerCb(void *arg){
	if(factoryRst_powerCnt >= FACTORY_RESET_POWER_CNT_THRESHOLD){
		/* here is just a mark, wait for device announce and then perform factory reset. */
		factoryRst_exist = TRUE;
	}

	factoryRst_powerCnt = 0;
	factoryRst_powerCntSave();

	factoryRst_timerEvt = NULL;
	return -1;
}

void factoryRst_handler(void){
	if(factoryRst_exist){
		factoryRst_exist = FALSE;

		/* MOES: a completed 3-power-cycle gesture is an unambiguous, human,
		 * deliberate act - and it necessarily produced several quick boots.
		 * Zero the rescue-mode probation counter here so performing the
		 * pairing gesture can never be mistaken for a light that cannot stay
		 * up. (moes_rescue.h keeps the threshold above 3 as well; these are
		 * two independent defences and both are cheap.) */
		{
			extern void moes_rescueClear(void);
			moes_rescueClear();
		}

		zb_factoryReset();
	}
}

void factoryRst_init(void){
	factoryRst_powerCntRestore();

	/* HARDENING: never trust the restored counter. If NV returns garbage (or
	 * is read before nv_init(), which used to happen), a bogus count >= the
	 * threshold makes factoryRst_handler() call zb_factoryReset() a couple of
	 * seconds after every boot - a self-inflicted reset loop on a device that
	 * can only be repaired over the air. A count above the threshold is never
	 * legitimate: the timer clears it to 0 two seconds into every boot.
	 *
	 * The comparison is '>=', not '>'. The largest value a legitimate power
	 * cycle can leave behind is THRESHOLD-1: on the cycle that reaches
	 * THRESHOLD the timer sets factoryRst_exist and immediately writes 0
	 * back. A restored value of exactly THRESHOLD therefore cannot come from
	 * a user gesture - but with '>' it survived the clamp, was incremented to
	 * THRESHOLD+1, and tripped factoryRst_timerCb()'s '>=' test on the very
	 * next boot. One byte of NV garbage in 256 was still a boot loop. */
	if(factoryRst_powerCnt >= FACTORY_RESET_POWER_CNT_THRESHOLD){
		factoryRst_powerCnt = 0;
	}

	/* MOES: an OTA install reboots the device on purpose -
	 * moes_resetSkipNextBoot() marks those so this counter never mistakes a
	 * firmware reboot for a user power cycle. Clear the count as well as
	 * skipping the increment, otherwise a stale value survives the reboot
	 * and a single later power cycle could trip the factory reset. */
	{
		extern bool moes_resetConsumeSkipFlag(void);
		if(moes_resetConsumeSkipFlag()){
			factoryRst_powerCnt = 0;
			factoryRst_powerCntSave();
			return;
		}
	}

	factoryRst_powerCnt++;
	factoryRst_powerCntSave();

	if(factoryRst_timerEvt){
		TL_ZB_TIMER_CANCEL(&factoryRst_timerEvt);
	}
	factoryRst_timerEvt = TL_ZB_TIMER_SCHEDULE(factoryRst_timerCb, NULL, FACTORY_RESET_TIMEOUT * 1000);
}

