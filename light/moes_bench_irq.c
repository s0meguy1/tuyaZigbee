/********************************************************************************************************
 * @file    moes_bench_irq.c
 *
 * @brief   Deliberately inert IRQ vector for the BENCH ONLY image.
 *
 * The self-test uses no radio, DMA UART, GPIO IRQ, or hardware timer IRQ.
 * Keeping the startup vector local avoids pulling Telink's radio-oriented
 * irq_handler.c and its Zigbee dependencies into the bench artifact.
 *******************************************************************************************************/

#include "../common/comm_cfg.h"
#include "tl_common.h"

#if MOES_BENCH_SELFTEST
_attribute_ram_code_ void irq_handler(void)
{
	/* No interrupt source is armed by moes_bench_main.c. */
}
#endif
