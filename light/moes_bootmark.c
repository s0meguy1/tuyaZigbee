/********************************************************************************************************
 * @file    moes_bootmark.c
 *
 * @brief   See moes_bootmark.h.
 *
 * @date    2026
 *******************************************************************************************************/

#if (__PROJECT_TL_DIMMABLE_LIGHT__)

#include "../common/comm_cfg.h"
#include "tl_common.h"
#include "moes_bootmark.h"

#if MOES_TS0505B

/* Always-on domain, retained across soft and watchdog resets, cleared to 0x00 by
 * a real power cycle. Unclaimed on this target - see the header. */
#define MOES_BOOTMARK_REG   DEEP_ANA_REG1

/* The SDK records the failing code here before invoking our handler
 * (proj/os/ev.c:33). It is not declared in any header. */
extern volatile u16 T_evtExcept[4];

u8 g_moesBootMarkPrev = 0;

void moes_bootMarkInit(void)
{
	g_moesBootMarkPrev = analog_read(MOES_BOOTMARK_REG);

	/* Clear immediately: from here on, a 0 read by the NEXT boot honestly means
	 * "this boot did not reach our exception handler". */
	analog_write(MOES_BOOTMARK_REG, 0);
}

void moes_bootMarkSet(u8 code)
{
	/* Stored raw, not code+1: these are reserved high values that cannot
	 * collide with a SYS_EXCEPTTION_* code, and 0 must keep meaning
	 * "nothing recorded". */
	if(code){
		analog_write(MOES_BOOTMARK_REG, code);
	}
}

void moes_bootMarkException(void)
{
	u8 code = (u8)T_evtExcept[1];

	/* Stored as code+1 so that 0 can mean "nothing recorded". 0xFF saturates
	 * rather than wrapping to 0, which would read as no exception at all. */
	analog_write(MOES_BOOTMARK_REG, (code == 0xFF) ? 0xFF : (u8)(code + 1));
}

#endif  /* MOES_TS0505B */
#endif  /* __PROJECT_TL_DIMMABLE_LIGHT__ */
