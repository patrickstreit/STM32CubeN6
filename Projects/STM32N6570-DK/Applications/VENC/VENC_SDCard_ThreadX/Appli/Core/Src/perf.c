/* perf.c - lightweight profiling implementation */
#include "perf.h"
#include "main.h"
#include <stdint.h>
#include <stdio.h>
#include <inttypes.h>

static PerfStat_t s_encode = {0};
static PerfStat_t s_h264 = {0};
static PerfStat_t s_qsend = {0};
static PerfStat_t s_qrecv = {0};
static PerfStat_t s_sd = {0};
static PerfStat_t s_sd_ll_dma = {0};
static PerfStat_t s_file_close = {0};
static PerfStat_t s_media_flush = {0};
static uint64_t s_cpu_hz = 1;
static uint32_t s_sd_ll_write_calls = 0;
static uint64_t s_sd_ll_write_total_blocks = 0;
static uint32_t s_sd_ll_write_max_blocks = 0;
static uint32_t s_sd_ll_blk_1 = 0;
static uint32_t s_sd_ll_blk_2_4 = 0;
static uint32_t s_sd_ll_blk_5_16 = 0;
static uint32_t s_sd_ll_blk_17_64 = 0;
static uint32_t s_sd_ll_blk_65_plus = 0;
/* occupancy counters */
static uint32_t s_blockpool_used = 0;
static uint32_t s_blockpool_max = 0;
static uint32_t s_blockpool_capacity = 0;
static uint32_t s_queue_used = 0;
static uint32_t s_queue_max = 0;
static uint32_t s_queue_capacity = 0;

static void perf_add_sample(PerfStat_t *s, uint32_t us)
{
  uint32_t prim = __get_PRIMASK();
  __disable_irq();
  if (s->count == 0)
  {
    s->min_us = us;
    s->max_us = us;
  }
  else
  {
    if (us < s->min_us) s->min_us = us;
    if (us > s->max_us) s->max_us = us;
  }
  s->count++;
  s->total_us += us;
  if (!prim) __enable_irq();
}

void perf_init(void)
{
  static int inited = 0;
  if (inited) return;
  inited = 1;
  /* Enable DWT cycle counter if available */
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
  /* read and cache CPU frequency once */
  s_cpu_hz = HAL_RCC_GetCpuClockFreq();
  if (s_cpu_hz == 0) s_cpu_hz = 1;
}

uint64_t perf_get_time_us(void)
{
  uint32_t cycles = DWT->CYCCNT;
  return (uint64_t)cycles * 1000000ULL / s_cpu_hz;
}

uint32_t perf_get_cycle_count(void)
{
  return (uint32_t)DWT->CYCCNT;
}

uint32_t perf_delta_us(uint32_t start_cycles, uint32_t end_cycles)
{
  uint32_t diff = end_cycles - start_cycles; /* modular subtraction handles wrap */
  return (uint32_t)((uint64_t)diff * 1000000ULL / s_cpu_hz);
}

uint64_t perf_get_u64_cycles(void)
{
  uint32_t prim = __get_PRIMASK();
  __disable_irq();
  static uint32_t last32 = 0;
  static uint64_t acc = 0; /* accumulates multiples of 2^32 */
  uint32_t cur = DWT->CYCCNT;
  if (cur < last32)
  {
    acc += 4294967296ULL; /* overflow of 32-bit counter */
  }
  last32 = cur;
  uint64_t result = acc + (uint64_t)cur;
  if (!prim) __enable_irq();
  return result;
}

uint64_t perf_delta_us64(uint64_t start_cycles, uint64_t end_cycles)
{
  uint64_t diff = end_cycles - start_cycles;
  return (uint64_t)((diff * 1000000ULL) / s_cpu_hz);
}

void perf_add_encode(uint32_t us) { perf_add_sample(&s_encode, us); }
void perf_add_h264(uint32_t us)   { perf_add_sample(&s_h264, us); }
void perf_add_queue_send(uint32_t us) { perf_add_sample(&s_qsend, us); }
void perf_add_queue_recv(uint32_t us) { perf_add_sample(&s_qrecv, us); }
void perf_add_sd_write(uint32_t us)   { perf_add_sample(&s_sd, us); }
void perf_add_sd_ll_dma(uint32_t us)  { perf_add_sample(&s_sd_ll_dma, us); }
void perf_add_file_close(uint32_t us) { perf_add_sample(&s_file_close, us); }
void perf_add_media_flush(uint32_t us) { perf_add_sample(&s_media_flush, us); }

void perf_add_sd_ll_write_blocks(uint32_t blocks)
{
  uint32_t prim = __get_PRIMASK();
  __disable_irq();
  s_sd_ll_write_calls++;
  s_sd_ll_write_total_blocks += blocks;
  if (blocks > s_sd_ll_write_max_blocks) s_sd_ll_write_max_blocks = blocks;
  if (blocks <= 1U)
  {
    s_sd_ll_blk_1++;
  }
  else if (blocks <= 4U)
  {
    s_sd_ll_blk_2_4++;
  }
  else if (blocks <= 16U)
  {
    s_sd_ll_blk_5_16++;
  }
  else if (blocks <= 64U)
  {
    s_sd_ll_blk_17_64++;
  }
  else
  {
    s_sd_ll_blk_65_plus++;
  }
  if (!prim) __enable_irq();
}

void perf_set_queue_capacity(uint32_t capacity)
{
  uint32_t prim = __get_PRIMASK();
  __disable_irq();
  s_queue_capacity = capacity;
  if (!prim) __enable_irq();
}

void perf_set_blockpool_capacity(uint32_t capacity)
{
  uint32_t prim = __get_PRIMASK();
  __disable_irq();
  s_blockpool_capacity = capacity;
  if (!prim) __enable_irq();
}

void perf_inc_blockpool(void)
{
  uint32_t prim = __get_PRIMASK();
  __disable_irq();
  s_blockpool_used++;
  if (s_blockpool_used > s_blockpool_max) s_blockpool_max = s_blockpool_used;
  if (!prim) __enable_irq();
}

void perf_dec_blockpool(void)
{
  uint32_t prim = __get_PRIMASK();
  __disable_irq();
  if (s_blockpool_used) s_blockpool_used--;
  if (!prim) __enable_irq();
}

void perf_inc_queue(void)
{
  uint32_t prim = __get_PRIMASK();
  __disable_irq();
  s_queue_used++;
  if (s_queue_used > s_queue_max) s_queue_max = s_queue_used;
  if (!prim) __enable_irq();
}

void perf_dec_queue(void)
{
  uint32_t prim = __get_PRIMASK();
  __disable_irq();
  if (s_queue_used) s_queue_used--;
  if (!prim) __enable_irq();
}

static void perf_print_stat(const char *name, PerfStat_t *s)
{
  if (s->count == 0)
  {
    printf("%-20s %10" PRIu32 ", %10" PRIu32 ", %10" PRIu32 ", %10" PRIu32 ", %10" PRIu32 "\n",
           name, (uint32_t)0, (uint32_t)0, (uint32_t)0, (uint32_t)0, (uint32_t)0);
    return;
  }
  uint32_t total_ms = (uint32_t)(s->total_us / 1000ULL);
  uint32_t avg_us = (uint32_t)(s->total_us / s->count);
  printf("%-20s %10" PRIu32 ", %10" PRIu32 ", %10" PRIu32 ", %10" PRIu32 ", %10" PRIu32 "\n", name,
         s->count,
         total_ms,
         avg_us,
         s->min_us,
         s->max_us);
}

void perf_report_and_reset(uint32_t frames, uint64_t bytes, uint32_t elapsed_ms)
{
  perf_init();
  uint32_t fps_x100 = 0;
  if (elapsed_ms) fps_x100 = (uint32_t)(((uint64_t)frames * 100000ULL) / (uint64_t)elapsed_ms);

  printf("=== PERF SUMMARY ===\n");
  printf("%10s, %10s, %10s, %13s\n", "frames", "bytes", "elapsed_ms", "fps");
  printf("%10lu, %10lu, %10lu, %10lu.%02lu\n", (uint32_t)frames, (uint32_t)bytes, (uint32_t)elapsed_ms, fps_x100 / 100, fps_x100 % 100);
  printf("%-20s %10s, %10s, %10s, %10s, %10s\n", "stage", "count", "total_ms", "avg_us", "min_us", "max_us");
  perf_print_stat("Encoder", &s_encode);
  perf_print_stat("H264Encode", &s_h264);
  perf_print_stat("QueueSend", &s_qsend);
  perf_print_stat("QueueRecv", &s_qrecv);
  perf_print_stat("SD_Write", &s_sd);
  perf_print_stat("SD_LLDMA", &s_sd_ll_dma);
  perf_print_stat("FileClose", &s_file_close);
  perf_print_stat("MediaFlush", &s_media_flush);

  printf("%-20s %10s, %10s, %10s\n", "sd_ll_write", "calls", "total_blk", "max_blk");
  printf("%-20s %10lu, %10lu, %10lu\n", "SD_WriteReq", s_sd_ll_write_calls, (uint32_t)s_sd_ll_write_total_blocks, s_sd_ll_write_max_blocks);
  printf("%-20s %10s, %10s, %10s, %10s, %10s\n", "sd_ll_blk_bins", "1", "2_4", "5_16", "17_64", "65p");
  printf("%-20s %10lu, %10lu, %10lu, %10lu, %10lu\n", "SD_WriteBins", s_sd_ll_blk_1, s_sd_ll_blk_2_4, s_sd_ll_blk_5_16, s_sd_ll_blk_17_64, s_sd_ll_blk_65_plus);

  /* Print occupancy summary */
  printf("%-20s %10s, %10s, %10s\n", "resource", "capacity", "max_used", "cur_used");
  printf("%-20s %10lu, %10lu, %10lu\n", "BlockPool", (uint32_t)s_blockpool_capacity, (uint32_t)s_blockpool_max, (uint32_t)s_blockpool_used);
  printf("%-20s %10lu, %10lu, %10lu\n", "Queue", (uint32_t)s_queue_capacity, (uint32_t)s_queue_max, (uint32_t)s_queue_used);

  /* reset */
  s_encode.count = s_encode.total_us = s_encode.min_us = s_encode.max_us = 0;
  s_h264.count = s_h264.total_us = s_h264.min_us = s_h264.max_us = 0;
  s_qsend.count = s_qsend.total_us = s_qsend.min_us = s_qsend.max_us = 0;
  s_qrecv.count = s_qrecv.total_us = s_qrecv.min_us = s_qrecv.max_us = 0;
  s_sd.count = s_sd.total_us = s_sd.min_us = s_sd.max_us = 0;
  s_sd_ll_dma.count = s_sd_ll_dma.total_us = s_sd_ll_dma.min_us = s_sd_ll_dma.max_us = 0;
  s_file_close.count = s_file_close.total_us = s_file_close.min_us = s_file_close.max_us = 0;
  s_media_flush.count = s_media_flush.total_us = s_media_flush.min_us = s_media_flush.max_us = 0;
  s_sd_ll_write_calls = 0;
  s_sd_ll_write_total_blocks = 0;
  s_sd_ll_write_max_blocks = 0;
  s_sd_ll_blk_1 = 0;
  s_sd_ll_blk_2_4 = 0;
  s_sd_ll_blk_5_16 = 0;
  s_sd_ll_blk_17_64 = 0;
  s_sd_ll_blk_65_plus = 0;
  /* reset occupancy maxima and current usage */
  s_blockpool_used = 0;
  s_blockpool_max = 0;
  s_queue_used = 0;
  s_queue_max = 0;
}
