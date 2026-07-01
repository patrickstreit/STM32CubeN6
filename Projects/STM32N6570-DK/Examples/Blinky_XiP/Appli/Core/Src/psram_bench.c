#include "psram_bench.h"

#if defined(PSRAM_BENCH_ENABLE)

#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * PSRAM throughput benchmark (APS256XX @ XSPI1, memory-mapped @ 0x90000000).
 *
 * Measures effective bandwidth for CPU and HPDMA1 memory-to-memory transfers
 * across several access widths and directions. Cycle counts are taken from the
 * DWT cycle counter and converted to time / MB/s using SystemCoreClock.
 *
 * Notes on cache handling (Cortex-M55 with D-Cache):
 *   - The PSRAM window is Normal cacheable memory by default, so raw CPU loops
 *     mostly measure cache behaviour. To expose the true PSRAM bandwidth we
 *     invalidate before read timing (forces line fills from PSRAM) and use DMA
 *     which bypasses the CPU cache entirely (with explicit maintenance for
 *     coherency around each transfer).
 * ---------------------------------------------------------------------------*/

#define BENCH_PSRAM_SIZE   (4U * 1024U * 1024U)   /* 4 MB per PSRAM buffer      */
#define BENCH_INT_SIZE     (64U * 1024U)          /* 64 KB internal SRAM block  */
#define BENCH_DMA_CHUNK    (0xFC00U)              /* 64512 B, <=65535 & 64-aln  */
#define BENCH_MB_DIV       (1000000ULL)           /* report decimal MB/s        */

/* Two large scratch buffers in PSRAM (.psram = XSPI1 memory-mapped window). */
__attribute__((section(".psram"), used)) static uint8_t bench_a[BENCH_PSRAM_SIZE];
__attribute__((section(".psram"), used)) static uint8_t bench_b[BENCH_PSRAM_SIZE];

/* Internal AXISRAM block for PSRAM<->internal copy tests. */
static uint8_t bench_int[BENCH_INT_SIZE];

static UART_HandleTypeDef *s_uart;
static uint32_t            s_cpu_hz;

/* --------------------------------------------------------------------------- */
/* UART / reporting helpers                                                     */
/* --------------------------------------------------------------------------- */

static void bench_puts(const char *s)
{
  HAL_UART_Transmit(s_uart, (uint8_t *)s, (uint16_t)strlen(s), 1000U);
}

static void bench_report(const char *name, uint32_t bytes, uint32_t cycles)
{
  char line[80];

  if (cycles == 0U)
  {
    snprintf(line, sizeof(line), "%-20s   n/a\r\n", name);
    bench_puts(line);
    return;
  }

  uint32_t us   = (uint32_t)(((uint64_t)cycles * 1000000ULL) / s_cpu_hz);
  uint32_t mbps = (uint32_t)(((uint64_t)bytes * s_cpu_hz) / ((uint64_t)cycles * BENCH_MB_DIV));

  snprintf(line, sizeof(line), "%-20s %8lu us  %6lu MB/s\r\n",
           name, (unsigned long)us, (unsigned long)mbps);
  bench_puts(line);
}

/* --------------------------------------------------------------------------- */
/* DWT cycle counter                                                            */
/* --------------------------------------------------------------------------- */

static void dwt_init(void)
{
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT       = 0U;
  DWT->CTRL        |= DWT_CTRL_CYCCNTENA_Msk;
}

static inline void dwt_reset(void)
{
  __DSB();
  DWT->CYCCNT = 0U;
  __DSB();
}

static inline uint32_t dwt_read(void)
{
  __DSB();
  return DWT->CYCCNT;
}

/* --------------------------------------------------------------------------- */
/* CPU access patterns                                                          */
/* --------------------------------------------------------------------------- */

/* Word write loop (write-back cache => mostly cache speed, then flushed). */
static uint32_t cpu_write_words(void *dst, uint32_t bytes)
{
  volatile uint32_t *p = (volatile uint32_t *)dst;
  uint32_t n = bytes / 4U;

  SCB_CleanInvalidateDCache();
  dwt_reset();
  for (uint32_t i = 0U; i < n; i++)
  {
    p[i] = i;
  }
  __DSB();
  uint32_t c = dwt_read();

  /* Push the written data out to PSRAM for the subsequent read/DMA tests. */
  SCB_CleanDCache_by_Addr((uint32_t *)dst, (int32_t)bytes);
  return c;
}

/* Word read loop with prior invalidate => real PSRAM line-fill bandwidth. */
static uint32_t cpu_read_words(const void *src, uint32_t bytes, uint32_t *sum_out)
{
  const volatile uint32_t *p = (const volatile uint32_t *)src;
  uint32_t n = bytes / 4U;
  uint32_t sum = 0U;

  SCB_InvalidateDCache_by_Addr((uint32_t *)src, (int32_t)bytes);
  dwt_reset();
  for (uint32_t i = 0U; i < n; i++)
  {
    sum += p[i];
  }
  __DSB();
  uint32_t c = dwt_read();

  *sum_out = sum;
  return c;
}

static uint32_t cpu_memcpy(void *dst, const void *src, uint32_t bytes)
{
  SCB_CleanInvalidateDCache();
  dwt_reset();
  memcpy(dst, src, bytes);
  __DSB();
  uint32_t c = dwt_read();
  SCB_CleanDCache_by_Addr((uint32_t *)dst, (int32_t)bytes);
  return c;
}

/* --------------------------------------------------------------------------- */
/* HPDMA1 memory-to-memory transfer (chunked: BNDT is only 16-bit)             */
/* --------------------------------------------------------------------------- */

static uint32_t dma_copy(void *dst, const void *src, uint32_t bytes, uint32_t *ok)
{
  DMA_HandleTypeDef hdma;

  __HAL_RCC_HPDMA1_CLK_ENABLE();

  memset(&hdma, 0, sizeof(hdma));
  hdma.Instance                 = HPDMA1_Channel0;
  hdma.Init.Request             = DMA_REQUEST_SW;
  hdma.Init.BlkHWRequest        = DMA_BREQ_SINGLE_BURST;
  hdma.Init.Direction           = DMA_MEMORY_TO_MEMORY;
  hdma.Init.SrcInc              = DMA_SINC_INCREMENTED;
  hdma.Init.DestInc             = DMA_DINC_INCREMENTED;
  hdma.Init.SrcDataWidth        = DMA_SRC_DATAWIDTH_WORD;
  hdma.Init.DestDataWidth       = DMA_DEST_DATAWIDTH_WORD;
  hdma.Init.Priority            = DMA_HIGH_PRIORITY;
  hdma.Init.SrcBurstLength      = 16U;
  hdma.Init.DestBurstLength     = 16U;
  hdma.Init.TransferAllocatedPort = DMA_SRC_ALLOCATED_PORT0 | DMA_DEST_ALLOCATED_PORT1;
  hdma.Init.TransferEventMode   = DMA_TCEM_BLOCK_TRANSFER;
  hdma.Init.Mode                = DMA_NORMAL;

  if (HAL_DMA_Init(&hdma) != HAL_OK)
  {
    *ok = 0U;
    return 0U;
  }

  uint32_t remaining = bytes;
  uint32_t s_addr    = (uint32_t)src;
  uint32_t d_addr    = (uint32_t)dst;

  dwt_reset();
  while (remaining > 0U)
  {
    uint32_t chunk = (remaining > BENCH_DMA_CHUNK) ? BENCH_DMA_CHUNK : remaining;

    if (HAL_DMA_Start(&hdma, s_addr, d_addr, chunk) != HAL_OK ||
        HAL_DMA_PollForTransfer(&hdma, HAL_DMA_FULL_TRANSFER, 1000U) != HAL_OK)
    {
      *ok = 0U;
      HAL_DMA_DeInit(&hdma);
      return 0U;
    }

    s_addr    += chunk;
    d_addr    += chunk;
    remaining -= chunk;
  }
  uint32_t c = dwt_read();

  HAL_DMA_DeInit(&hdma);
  *ok = 1U;
  return c;
}

/* --------------------------------------------------------------------------- */
/* Public entry point                                                           */
/* --------------------------------------------------------------------------- */

void PSRAM_Bench_Run(UART_HandleTypeDef *huart)
{
  s_uart   = huart;
  s_cpu_hz = SystemCoreClock;

  dwt_init();

  char hdr[96];
  snprintf(hdr, sizeof(hdr),
           "\r\n=== PSRAM benchmark (CPU %lu MHz, %lu MB blocks) ===\r\n",
           (unsigned long)(s_cpu_hz / 1000000U),
           (unsigned long)(BENCH_PSRAM_SIZE / (1024U * 1024U)));
  bench_puts(hdr);

  uint32_t cyc;
  uint32_t sum;
  uint32_t ok;

  /* ---- CPU, full 4 MB PSRAM buffer ---- */
  cyc = cpu_write_words(bench_a, BENCH_PSRAM_SIZE);
  bench_report("CPU write  (word)", BENCH_PSRAM_SIZE, cyc);

  cyc = cpu_read_words(bench_a, BENCH_PSRAM_SIZE, &sum);
  bench_report("CPU read   (word)", BENCH_PSRAM_SIZE, cyc);

  cyc = cpu_memcpy(bench_b, bench_a, BENCH_PSRAM_SIZE);
  bench_report("CPU memcpy P->P", BENCH_PSRAM_SIZE, cyc);

  /* ---- HPDMA1, full 4 MB PSRAM buffer ---- */
  SCB_CleanDCache_by_Addr((uint32_t *)bench_a, (int32_t)BENCH_PSRAM_SIZE);
  cyc = dma_copy(bench_b, bench_a, BENCH_PSRAM_SIZE, &ok);
  SCB_InvalidateDCache_by_Addr((uint32_t *)bench_b, (int32_t)BENCH_PSRAM_SIZE);
  bench_report("DMA copy   P->P", BENCH_PSRAM_SIZE, ok ? cyc : 0U);

  /* ---- PSRAM <-> internal SRAM (64 KB block, CPU vs DMA) ---- */
  cyc = cpu_memcpy(bench_int, bench_a, BENCH_INT_SIZE);
  bench_report("CPU memcpy P->int", BENCH_INT_SIZE, cyc);

  cyc = cpu_memcpy(bench_a, bench_int, BENCH_INT_SIZE);
  bench_report("CPU memcpy int->P", BENCH_INT_SIZE, cyc);

  SCB_CleanDCache_by_Addr((uint32_t *)bench_a, (int32_t)BENCH_INT_SIZE);
  cyc = dma_copy(bench_int, bench_a, BENCH_INT_SIZE, &ok);
  SCB_InvalidateDCache_by_Addr((uint32_t *)bench_int, (int32_t)BENCH_INT_SIZE);
  bench_report("DMA copy   P->int", BENCH_INT_SIZE, ok ? cyc : 0U);

  SCB_CleanDCache_by_Addr((uint32_t *)bench_int, (int32_t)BENCH_INT_SIZE);
  cyc = dma_copy(bench_a, bench_int, BENCH_INT_SIZE, &ok);
  bench_report("DMA copy   int->P", BENCH_INT_SIZE, ok ? cyc : 0U);

  bench_puts("=== PSRAM benchmark done ===\r\n");
}

#endif /* PSRAM_BENCH_ENABLE */
