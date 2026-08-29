/********************************************************************************************************
 * @file    moes_bench_selftest.h
 *
 * @brief   BENCH ONLY output sequencer for a loose TS0505B / ZT3L module.
 *
 * This API is compiled into the deliberately named bench image only. It is
 * not a Zigbee application and must never be installed in a powered light.
 *******************************************************************************************************/

#pragma once

#if MOES_BENCH_SELFTEST
void moes_bench_selftest_init(void);
void moes_bench_selftest_task(void);
#endif
