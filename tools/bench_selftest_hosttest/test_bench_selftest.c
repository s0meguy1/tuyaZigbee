/********************************************************************************************************
 * Host validation for the real BENCH ONLY scheduler. It proves the visible
 * requirements without a target: boot warning, output mux, all-off sync,
 * one active PWM channel at a time, each of five channels, and heartbeat.
 *******************************************************************************************************/

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "shim/tl_common.h"
#include "../../light/moes_bench_selftest.h"

#define PWM_PERIOD 12000U
#define PWM_DUTY    6000U

typedef struct {
	u16 cmp;
	u16 period;
} pwm_state_t;

static u32 host_now;
static pwm_state_t pwm[6];
static int pwm_init_calls;
static int pwm_starts[6];
static u32 mux_gpio[5];
static u16 mux_func[5];
static int mux_calls;
static char uart_log[8192];
static size_t uart_log_len;
static int heartbeat_off_assertions;

static void assert_all_off(void);

static void assert_pwm_safe_before_mux(void)
{
	static const u8 channels[] = { 4, 1, 3, 5, 0 };

	for(size_t i = 0; i < sizeof(channels) / sizeof(channels[0]); i++){
		u8 channel = channels[i];
		assert(pwm[channel].period == PWM_PERIOD);
		assert(pwm[channel].cmp == 0);
		assert(pwm_starts[channel] == 1);
	}
}

u32 host_clock_time(void)
{
	return host_now;
}

int host_clock_time_exceed(u32 ref, u32 us)
{
	return (u32)(host_now - ref) > us;
}

void gpio_set_func(u32 gpio, u16 func)
{
	/* Each PWM mux becomes electrically visible only after every channel has
	 * a known-off compare value and its generator is already running. */
	assert(pwm_init_calls == 1);
	assert_pwm_safe_before_mux();
	assert(mux_calls < 5);
	mux_gpio[mux_calls] = gpio;
	mux_func[mux_calls] = func;
	mux_calls++;
}

void drv_pwm_init(void)
{
	pwm_init_calls++;
}

void drv_pwm_cfg(u8 channel, u16 cmp, u16 period)
{
	assert(channel < 6);
	pwm[channel].cmp = cmp;
	pwm[channel].period = period;
}

void host_pwm_start(u8 channel)
{
	assert(channel < 6);
	pwm_starts[channel]++;
}

int host_printf(const char *format, ...)
{
	va_list args;
	int written;

	/* A heartbeat is emitted at the WARM_WHITE -> sync rollover.  The target
	 * soft UART blocks while it transmits, so every PWM compare must already
	 * be zero when logging begins.  This assertion fails with the old
	 * heartbeat-before-all-off ordering. */
	if(strncmp(format, "BENCH SELFTEST heartbeat",
			   sizeof("BENCH SELFTEST heartbeat") - 1) == 0){
		assert_all_off();
		heartbeat_off_assertions++;
	}

	va_start(args, format);
	written = vsnprintf(uart_log + uart_log_len,
					sizeof(uart_log) - uart_log_len, format, args);
	va_end(args);
	assert(written >= 0);
	assert((size_t)written < sizeof(uart_log) - uart_log_len);
	uart_log_len += (size_t)written;
	return written;
}

static void assert_only_channel(u8 expected)
{
	static const u8 channels[] = { 4, 1, 3, 5, 0 };
	int active = 0;

	for(size_t i = 0; i < sizeof(channels) / sizeof(channels[0]); i++){
		u8 channel = channels[i];
		assert(pwm[channel].period == PWM_PERIOD);
		if(pwm[channel].cmp != 0){
			assert(channel == expected);
			assert(pwm[channel].cmp == PWM_DUTY);
			active++;
		}
	}
	assert(active == 1);
}

static void assert_all_off(void)
{
	static const u8 channels[] = { 4, 1, 3, 5, 0 };
	for(size_t i = 0; i < sizeof(channels) / sizeof(channels[0]); i++){
		assert(pwm[channels[i]].period == PWM_PERIOD);
		assert(pwm[channels[i]].cmp == 0);
	}
}

static void advance_us(u32 us)
{
	host_now += us;
	moes_bench_selftest_task();
}

int main(void)
{
	static const u8 expected_channels[] = { 4, 1, 3, 5, 0 };
	static const char *expected_names[] = {
		"channel=RED", "channel=GREEN", "channel=BLUE",
		"channel=COOL_WHITE", "channel=WARM_WHITE",
	};

	moes_bench_selftest_init();
	assert(pwm_init_calls == 1);
	assert(mux_calls == 5);
	assert(mux_gpio[0] == GPIO_PB4 && mux_func[0] == AS_PWM4);
	assert(mux_gpio[1] == GPIO_PC3 && mux_func[1] == AS_PWM1);
	assert(mux_gpio[2] == GPIO_PD2 && mux_func[2] == AS_PWM3);
	assert(mux_gpio[3] == GPIO_PB5 && mux_func[3] == AS_PWM5);
	assert(mux_gpio[4] == GPIO_PC2 && mux_func[4] == AS_PWM0);
	assert(pwm_starts[0] == 1 && pwm_starts[1] == 1 && pwm_starts[3] == 1);
	assert(pwm_starts[4] == 1 && pwm_starts[5] == 1);
	assert(strstr(uart_log, "BENCH ONLY - DO NOT INSTALL ON LIGHT") != NULL);
	assert(strstr(uart_log, "PB1 1Mbps 8N1") != NULL);
	assert(strstr(uart_log, "No Zigbee commissioning, NV, OTA") != NULL);
	assert(strstr(uart_log, "channel=SYNC_ALL_OFF duty=0%") != NULL);
	assert_all_off();

	/* The 500 ms all-off sync interval must be fully observable. */
	advance_us(500000U);
	assert_all_off();

	for(size_t i = 0; i < sizeof(expected_channels) / sizeof(expected_channels[0]); i++){
		advance_us(1U);
		assert_only_channel(expected_channels[i]);
		assert(strstr(uart_log, expected_names[i]) != NULL);
		assert(strstr(uart_log, "duty=50% pwm=4000Hz") != NULL);
		advance_us(1000000U);
	}

	advance_us(1U);
	assert_all_off();
	const char *heartbeat = strstr(uart_log, "BENCH SELFTEST heartbeat cycle=1");
	const char *sync_cycle_1 = strstr(uart_log,
		"BENCH SELFTEST cycle=1 channel=SYNC_ALL_OFF duty=0%");
	assert(heartbeat_off_assertions == 1);
	assert(heartbeat != NULL);
	assert(sync_cycle_1 != NULL);
	assert(heartbeat < sync_cycle_1);
	puts("bench self-test host checks passed");
	return 0;
}
