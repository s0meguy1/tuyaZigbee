/********************************************************************************************************
 * @file    moes_bench_main.c
 *
 * @brief   Minimal startup for the BENCH ONLY TS0505B self-test image.
 *
 * Unlike the SDK's apps/common/main.c, this entry point does not call
 * drv_platform_init() (which starts radio support), os_init(), user_init(),
 * ev_main(), or tl_zbTaskProcedure(). It does only the B85 reset/clock/GPIO
 * setup required for direct PWM and the existing PB1 software UART.
 *******************************************************************************************************/

#include "../common/comm_cfg.h"
#include "tl_common.h"
#include "moes_bench_selftest.h"

#if MOES_BENCH_SELFTEST

#if !defined(BUILD_TS0505B)
#error "MOES_BENCH_SELFTEST is only valid for BUILD_TS0505B"
#endif

static void moes_bench_platform_init(void)
{
	irq_disable();
	irq_disable_type(FLD_IRQ_ALL);

	/* This mirrors only the TLSR8258 reset/clock/GPIO portion of the SDK
 * platform initializer. In particular it deliberately omits
 * drv_platform_init() because that helper starts ZB_RADIO_INIT() and
 * ZB_TIMER_INIT(). cpu_wakeup_init() may read the vendor VDD calibration and
 * applies it to an analog register; this path performs no flash/NV writes,
 * BDB, OTA, or RF initialization. */
	cpu_wakeup_init();
	clock_init(SYS_CLK_48M_Crystal);
	gpio_init(TRUE);
	DEBUG_TX_PIN_INIT();
}

int main(void)
{
	moes_bench_platform_init();
	moes_bench_selftest_init();

	irq_enable();
#if MODULE_WATCHDOG_ENABLE
	wd_set_interval_ms(600);
	wd_start();
#endif

	while(1){
		moes_bench_selftest_task();
#if MODULE_WATCHDOG_ENABLE
		wd_clear();
#endif
	}
}

#endif  /* MOES_BENCH_SELFTEST */
