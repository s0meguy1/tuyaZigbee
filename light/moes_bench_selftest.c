/********************************************************************************************************
 * @file    moes_bench_selftest.c
 *
 * @brief   BENCH ONLY TS0505B output and UART self-test.
 *
 * The image that contains this file has no Zigbee stack startup, BDB, OTA,
 * factory-reset, or NV initialization path. It configures the five compiled
 * PWM pads directly and sends soft-UART diagnostics on PB1 at 1 Mbps.
 *
 * DO NOT INSTALL ON A LIGHT. This is for a loose, sacrificial ZT3L only.
 *******************************************************************************************************/

#include "../common/comm_cfg.h"
#include "tl_common.h"
#include "moes_bench_selftest.h"

#if MOES_BENCH_SELFTEST

#if !defined(BUILD_TS0505B)
#error "MOES_BENCH_SELFTEST is only valid for BUILD_TS0505B"
#endif

#define MOES_BENCH_PWM_HZ             4000U
#define MOES_BENCH_PWM_PERIOD          (PWM_CLOCK_SOURCE / MOES_BENCH_PWM_HZ)
#define MOES_BENCH_PWM_DUTY_PERCENT    50U
#define MOES_BENCH_PWM_DUTY_TICKS      ((MOES_BENCH_PWM_PERIOD * MOES_BENCH_PWM_DUTY_PERCENT) / 100U)
#define MOES_BENCH_SYNC_US             500000U
#define MOES_BENCH_CHANNEL_US          1000000U

typedef struct {
	const char *name;
	u32 gpio;
	u8 pwmChannel;
	u16 pwmMux;
} moes_bench_channel_t;

/* Keep this table tied to the compiled TS0505B GPIO/PWM definitions. The
 * test intentionally does not consult the factory JSON or any NV state. */
static const moes_bench_channel_t moes_bench_channels[] = {
	{ "RED",        LED_R,  PWM_R_CHANNEL,  AS_PWM4 },
	{ "GREEN",      LED_G,  PWM_G_CHANNEL,  AS_PWM1 },
	{ "BLUE",       LED_B,  PWM_B_CHANNEL,  AS_PWM3 },
	{ "COOL_WHITE", LED_CW, PWM_CW_CHANNEL, AS_PWM5 },
	{ "WARM_WHITE", LED_WW, PWM_WW_CHANNEL, AS_PWM0 },
};

#define MOES_BENCH_CHANNEL_COUNT \
	(sizeof(moes_bench_channels) / sizeof(moes_bench_channels[0]))

/* step 0 is the conspicuous all-off synchronisation window. */
static u8 moes_bench_step;
static u16 moes_bench_cycle;
static u32 moes_bench_step_started;

static void moes_bench_all_off(void)
{
	for(u8 i = 0; i < MOES_BENCH_CHANNEL_COUNT; i++){
		drv_pwm_cfg(moes_bench_channels[i].pwmChannel, 0, MOES_BENCH_PWM_PERIOD);
	}
}

static void moes_bench_log_step(void)
{
	if(moes_bench_step == 0){
		printf("BENCH SELFTEST cycle=%d channel=SYNC_ALL_OFF duty=0%%\r\n", moes_bench_cycle);
	}else{
		printf("BENCH SELFTEST cycle=%d channel=%s duty=%d%% pwm=%dHz\r\n",
			   moes_bench_cycle,
			   moes_bench_channels[moes_bench_step - 1].name,
			   MOES_BENCH_PWM_DUTY_PERCENT,
			   MOES_BENCH_PWM_HZ);
	}
}

static void moes_bench_apply_step(void)
{
	moes_bench_all_off();
	if(moes_bench_step != 0){
		drv_pwm_cfg(moes_bench_channels[moes_bench_step - 1].pwmChannel,
					MOES_BENCH_PWM_DUTY_TICKS, MOES_BENCH_PWM_PERIOD);
	}
	moes_bench_log_step();
}

void moes_bench_selftest_init(void)
{
	drv_pwm_init();
	/* The PWM reset state is not a safe output contract. Program every
	 * compare register to off first, start the known-off generators while the
	 * pins are still GPIOs, and only then expose the PWM mux. This prevents a
	 * boot-edge pulse on any LED channel. */
	moes_bench_all_off();
	for(u8 i = 0; i < MOES_BENCH_CHANNEL_COUNT; i++){
		drv_pwm_start(moes_bench_channels[i].pwmChannel);
	}
	for(u8 i = 0; i < MOES_BENCH_CHANNEL_COUNT; i++){
		gpio_set_func(moes_bench_channels[i].gpio, moes_bench_channels[i].pwmMux);
	}

	/* PB1's existing UART_PRINTF_MODE implementation is a 1 Mbps soft UART.
	 * The messages take about 1 ms each, then this function returns; timing of
	 * the output sequence itself is cooperative and never waits in a loop. */
	printf("\r\n*** BENCH ONLY - DO NOT INSTALL ON LIGHT ***\r\n");
	printf("ZT3L TS0505B BENCH SELFTEST: PB1 1Mbps 8N1; PWM %dHz, %d%%\r\n",
		   MOES_BENCH_PWM_HZ, MOES_BENCH_PWM_DUTY_PERCENT);
	printf("No Zigbee commissioning, NV, OTA, or factory-reset path is started.\r\n");

	moes_bench_step = 0;
	moes_bench_cycle = 0;
	moes_bench_step_started = clock_time();
	moes_bench_apply_step();
}

void moes_bench_selftest_task(void)
{
	u32 duration = (moes_bench_step == 0) ? MOES_BENCH_SYNC_US : MOES_BENCH_CHANNEL_US;

	if(!clock_time_exceed(moes_bench_step_started, duration)){
		return;
	}

	moes_bench_step_started = clock_time();
	moes_bench_step++;
	if(moes_bench_step > MOES_BENCH_CHANNEL_COUNT){
		moes_bench_step = 0;
		moes_bench_cycle++;
		/* printf() is a blocking 1 Mbps soft-UART transfer.  Make the
		 * rollover electrically safe before it starts so WARM_WHITE cannot
		 * remain driven for the heartbeat duration.  moes_bench_apply_step()
		 * below deliberately writes the known-off state again before logging
		 * SYNC_ALL_OFF. */
		moes_bench_all_off();
		printf("BENCH SELFTEST heartbeat cycle=%d\r\n", moes_bench_cycle);
	}
	moes_bench_apply_step();
}

#endif  /* MOES_BENCH_SELFTEST */
