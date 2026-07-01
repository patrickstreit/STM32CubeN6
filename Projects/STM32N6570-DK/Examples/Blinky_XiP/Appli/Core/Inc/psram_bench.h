#pragma once

#include "stm32n6xx_hal.h"

/* ---------------------------------------------------------------------------
 * Optional PSRAM throughput benchmark.
 *
 * Enabled only when the CMake option PSRAM_BENCH=ON is set (which defines
 * PSRAM_BENCH_ENABLE and compiles psram_bench.c). When disabled, the call
 * below collapses to a zero-cost no-op, so main.c can call it unconditionally.
 *
 * Use the "Debug + Bench" CMake preset to build with the benchmark active.
 * ---------------------------------------------------------------------------*/

#if defined(PSRAM_BENCH_ENABLE)

/* Runs the full benchmark suite and streams the results over the given UART. */
void PSRAM_Bench_Run(UART_HandleTypeDef *huart);

#else

static inline void PSRAM_Bench_Run(UART_HandleTypeDef *huart)
{
  (void)huart;
}

#endif /* PSRAM_BENCH_ENABLE */
