/* Minimal host surface for compiling the real moes_bench_selftest.c. */
#pragma once

#include <stdarg.h>
#include <stdint.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;

#define TRUE 1

/* TLSR8258 GPIO/PWM tokens used by device_config/light_ts0505b.h. */
#define GPIO_PB4  0x14U
#define GPIO_PC3  0x23U
#define GPIO_PD2  0x32U
#define GPIO_PB5  0x15U
#define GPIO_PC2  0x22U

#define AS_PWM0   0x100U
#define AS_PWM1   0x101U
#define AS_PWM3   0x103U
#define AS_PWM4   0x104U
#define AS_PWM5   0x105U

#define PWM_CLOCK_SOURCE 48000000U

u32 host_clock_time(void);
int host_clock_time_exceed(u32 ref, u32 us);
#define clock_time()                 host_clock_time()
#define clock_time_exceed(ref, us)   host_clock_time_exceed((ref), (us))

void gpio_set_func(u32 gpio, u16 func);
void drv_pwm_init(void);
void drv_pwm_cfg(u8 pwmChannel, u16 cmpTicks, u16 periodTicks);
void host_pwm_start(u8 pwmChannel);
#define drv_pwm_start(channel) host_pwm_start(channel)

int host_printf(const char *format, ...);
#define printf host_printf
