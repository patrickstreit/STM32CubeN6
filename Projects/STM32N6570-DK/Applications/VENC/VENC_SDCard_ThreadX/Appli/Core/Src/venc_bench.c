/**
  ******************************************************************************
  * @file    venc_bench.c
  * @brief   Encode-time model of the H.264 encoder - see venc_bench.h.
  ******************************************************************************
  */

/* The include order here is not cosmetic. The encoder's basetype.h typedefs
   both size_t and an enum bool of its own, so it has to sit after the C library
   headers that define size_t - and before <stdbool.h>, which venc_bench.h pulls
   in and which turns bool, true and false into macros. Either header first and
   the other one no longer compiles. */
#include <stdio.h>
#include <string.h>

#include "stm32n6xx_hal.h"
#include "venc_app.h"
#include "venc_h264_config.h"
#include "utils.h"

#include "venc_bench.h"

/* The composite the measurement is taken for: 448 x 1792, 28 x 112 macroblocks.
   Kept here rather than derived from a header so this stays readable next to
   the projection it feeds. */
#define COMPOSITE_MACROBLOCKS   3136U

/* One 720p frame in the widest format this build captures. NV12 needs 1.5
   bytes per pixel; YUYV would need 2 and does not fit next to the bitstream
   buffer in AXISRAM, which is the whole reason the staging buffer exists. */
#define STAGE_FRAME_SIZE        ((1280U * 720U * 3U) / 2U)

#if defined(VENC_M2_AXISRAM_INPUT)
/* AXISRAM, non-cacheable - the same attributes the DCMIPP destination has, so
   the only difference against the capture buffer is which memory answers the
   encoder's reads. */
static uint8_t stage_frame[STAGE_FRAME_SIZE] ALIGN_32 IN_UNCACHED_RAM;
#endif

/* ------------------------------------------------------------------------- */
/* Cycle counter                                                             */
/* ------------------------------------------------------------------------- */

static uint32_t g_cycles_per_us;

static void cycle_counter_init(void)
{
  if (g_cycles_per_us != 0U)
  {
    return;
  }

  if ((CoreDebug->DEMCR & CoreDebug_DEMCR_TRCENA_Msk) == 0U)
  {
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  }
  /* Not reset: TraceX timestamps its ring from the same counter. Only
     differences are taken, which a free-running counter gives just as well. */
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

  g_cycles_per_us = HAL_RCC_GetCpuClockFreq() / 1000000U;
  if (g_cycles_per_us == 0U)
  {
    g_cycles_per_us = 1U;
  }
}

uint32_t venc_bench_now(void)
{
  cycle_counter_init();
  return DWT->CYCCNT;
}

uint32_t venc_bench_cycles_per_us(void)
{
  cycle_counter_init();
  return g_cycles_per_us;
}

static uint32_t cyc_to_us(uint32_t cycles)
{
  return cycles / venc_bench_cycles_per_us();
}

/* ------------------------------------------------------------------------- */
/* Statistics                                                                */
/* ------------------------------------------------------------------------- */

typedef struct
{
  uint32_t n;
  uint32_t min;
  uint32_t max;
  uint64_t sum;
} stat_t;

static void stat_reset(stat_t *s)
{
  s->n = 0U;
  s->min = 0xFFFFFFFFU;
  s->max = 0U;
  s->sum = 0U;
}

static void stat_add(stat_t *s, uint32_t v)
{
  if (v < s->min) { s->min = v; }
  if (v > s->max) { s->max = v; }
  s->sum += (uint64_t)v;
  s->n++;
}

static uint32_t stat_avg(const stat_t *s)
{
  return (s->n == 0U) ? 0U : (uint32_t)(s->sum / (uint64_t)s->n);
}

static stat_t   g_intra;
static stat_t   g_inter;
static stat_t   g_copy;
static uint64_t g_stream_bytes;
static uint32_t g_started_ms;
static bool     g_discard;

static venc_input_src_t g_input_src = VENC_INPUT_FROM_CAPTURE;

void venc_bench_reset(void)
{
  cycle_counter_init();
  stat_reset(&g_intra);
  stat_reset(&g_inter);
  stat_reset(&g_copy);
  g_stream_bytes = 0U;
  g_started_ms   = HAL_GetTick();
}

void venc_bench_sample(bool intra, uint32_t cycles, uint32_t stream_bytes, uint32_t copy_cycles)
{
  stat_add(intra ? &g_intra : &g_inter, cycles);
  if (copy_cycles != 0U)
  {
    stat_add(&g_copy, copy_cycles);
  }
  g_stream_bytes += (uint64_t)stream_bytes;
}

uint32_t venc_bench_frames(void)
{
  return g_intra.n + g_inter.n;
}

bool venc_bench_discard(void)
{
  return g_discard;
}

void venc_bench_set_discard(bool discard)
{
  g_discard = discard;
}

uint8_t *venc_bench_stage_buffer(uint32_t *size)
{
#if defined(VENC_M2_AXISRAM_INPUT)
  if (size != NULL)
  {
    *size = STAGE_FRAME_SIZE;
  }
  return stage_frame;
#else
  if (size != NULL)
  {
    *size = 0U;
  }
  return NULL;
#endif
}

/* ------------------------------------------------------------------------- */
/* Report                                                                    */
/* ------------------------------------------------------------------------- */

static uint32_t macroblocks(void)
{
  uint32_t w = (uint32_t)hVencH264Instance.cfgH264Main.width;
  uint32_t h = (uint32_t)hVencH264Instance.cfgH264Main.height;

  return ((w + 15U) / 16U) * ((h + 15U) / 16U);
}

static const char *format_name(void)
{
  return (hDcmippH264Instance.format == DCMIPP_PIXEL_PACKER_FORMAT_YUV420_2) ? "NV12" : "YUYV";
}

static const char *input_src_name(void)
{
  return (g_input_src == VENC_INPUT_FROM_AXISRAM) ? "axisram" : "capture-psram";
}

/** @brief Where the bitstream ring the SD writer reads from was linked. */
static const char *bitstream_location_name(void)
{
#if defined(VENC_BITSTREAM_IN_PSRAM)
  return "psram";
#else
  return "axisram";
#endif
}

/** @brief Print min/avg/max of a cycle statistic in whole microseconds. */
static void stat_print(const char *prefix, const stat_t *s)
{
  printf("%s.n=%lu\n", prefix, (unsigned long)s->n);
  if (s->n == 0U)
  {
    return;
  }
  printf("%s.us_min=%lu\n", prefix, (unsigned long)cyc_to_us(s->min));
  printf("%s.us_avg=%lu\n", prefix, (unsigned long)cyc_to_us(stat_avg(s)));
  printf("%s.us_max=%lu\n", prefix, (unsigned long)cyc_to_us(s->max));
}

/* Counted in the FileX SD glue: which memory the SDMMC was pointed at, and
   whether it accepted and completed the transfer. */
extern volatile uint32_t sd_write_calls;
extern volatile uint32_t sd_write_from_axisram;
extern volatile uint32_t sd_write_from_psram;
extern volatile uint32_t sd_write_rejected;
extern volatile uint32_t sd_write_completions;

static stat_t   g_sd_write;
static uint64_t g_sd_bytes;
static uint32_t g_sd_started_ms;

void venc_bench_sd_reset(void)
{
  cycle_counter_init();
  stat_reset(&g_sd_write);
  g_sd_bytes      = 0U;
  g_sd_started_ms = HAL_GetTick();

  sd_write_calls        = 0U;
  sd_write_from_axisram = 0U;
  sd_write_from_psram   = 0U;
  sd_write_rejected     = 0U;
  sd_write_completions  = 0U;
}

void venc_bench_sd_sample(uint32_t cycles, uint32_t bytes)
{
  stat_add(&g_sd_write, cycles);
  g_sd_bytes += (uint64_t)bytes;
}

uint32_t venc_bench_sd_writes(void)
{
  return g_sd_write.n;
}

void venc_bench_sd_report(void)
{
  uint32_t elapsed = HAL_GetTick() - g_sd_started_ms;
  /* stat_avg() is in cycles - everything else here is microseconds. */
  uint32_t avg_us  = (g_sd_write.n == 0U) ? 0U : cyc_to_us(stat_avg(&g_sd_write));

  printf("=== SD RESULT ===\n");
  printf("cpu_hz=%lu\n", (unsigned long)HAL_RCC_GetCpuClockFreq());
  printf("bitstream_src=%s\n", bitstream_location_name());
  printf("blk.calls=%lu\n", (unsigned long)sd_write_calls);
  printf("blk.from_axisram=%lu\n", (unsigned long)sd_write_from_axisram);
  printf("blk.from_psram=%lu\n", (unsigned long)sd_write_from_psram);
  printf("blk.rejected=%lu\n", (unsigned long)sd_write_rejected);
  printf("blk.completions=%lu\n", (unsigned long)sd_write_completions);
  printf("bitrate=%lu\n", (unsigned long)hVencH264Instance.cfgH264Rate.bitPerSecond);
  printf("window_ms=%lu\n", (unsigned long)elapsed);
  stat_print("write", &g_sd_write);
  printf("writes=%lu\n", (unsigned long)g_sd_write.n);
  printf("kbytes=%lu\n", (unsigned long)(g_sd_bytes / 1024U));
  if (g_sd_write.n != 0U)
  {
    printf("bytes_per_write=%lu\n",
           (unsigned long)(g_sd_bytes / (uint64_t)g_sd_write.n));
  }
  /* Throughput of the write calls themselves, not of the recording: it ignores
     the time the writer spends waiting for frames, which is what makes it
     comparable between two buffer placements. */
  if (avg_us != 0U)
  {
    uint32_t bytes_per_write = (uint32_t)(g_sd_bytes / (uint64_t)g_sd_write.n);
    printf("write_kbyte_per_s=%lu\n",
           (unsigned long)(((uint64_t)bytes_per_write * 1000000U) /
                           ((uint64_t)avg_us * 1024U)));
  }
  printf("=== END ===\n");
}

void venc_bench_report(const char *label)
{
  uint32_t mbs      = macroblocks();
  uint32_t frames   = venc_bench_frames();
  uint32_t elapsed  = HAL_GetTick() - g_started_ms;
  uint64_t all_sum  = g_intra.sum + g_inter.sum;
  uint32_t all_avg;
  uint32_t us_per_mb_x100;
  uint32_t inter_per_mb_x100;
  uint32_t composite_us;

  if (frames == 0U)
  {
    printf("VENC: nothing measured yet - 'start' the pipeline and let it run\n");
    return;
  }

  all_avg = cyc_to_us((uint32_t)(all_sum / (uint64_t)frames));

  /* Per macroblock, in hundredths of a microsecond: the encoder is in the
     single-digit microseconds per macroblock, so whole microseconds would
     round two variants onto the same number. */
  us_per_mb_x100    = (mbs == 0U) ? 0U : ((all_avg * 100U) / mbs);
  inter_per_mb_x100 = ((mbs == 0U) || (g_inter.n == 0U))
                        ? 0U : ((cyc_to_us(stat_avg(&g_inter)) * 100U) / mbs);

  /* What the whole exercise is for: the same encoder on the composite frame. */
  composite_us = (us_per_mb_x100 * COMPOSITE_MACROBLOCKS) / 100U;

  printf("=== VENC RESULT ===\n");
  printf("label=%s\n", (label != NULL) ? label : "-");
  printf("cpu_hz=%lu\n", (unsigned long)HAL_RCC_GetCpuClockFreq());
  printf("width=%lu\n", (unsigned long)hVencH264Instance.cfgH264Main.width);
  printf("height=%lu\n", (unsigned long)hVencH264Instance.cfgH264Main.height);
  printf("macroblocks=%lu\n", (unsigned long)mbs);
  printf("pixel_format=%s\n", format_name());
  printf("input_src=%s\n", input_src_name());
  printf("cabac=%lu\n", (unsigned long)hVencH264Instance.cfgH264Coding.enableCabac);
  printf("transform8x8=%lu\n", (unsigned long)hVencH264Instance.cfgH264Coding.transform8x8Mode);
  printf("bitrate=%lu\n", (unsigned long)hVencH264Instance.cfgH264Rate.bitPerSecond);
  printf("gop=%lu\n", (unsigned long)hVencH264Instance.cfgH264Rate.gopLen);
  printf("framerate_cfg=%lu\n", (unsigned long)GetVideoFramerate());
  printf("window_ms=%lu\n", (unsigned long)elapsed);
  printf("frames=%lu\n", (unsigned long)frames);
  stat_print("intra", &g_intra);
  stat_print("inter", &g_inter);
  if (g_copy.n != 0U)
  {
    stat_print("stage_copy", &g_copy);
  }
  printf("all.us_avg=%lu\n", (unsigned long)all_avg);
  printf("all.us_per_mb_x100=%lu\n", (unsigned long)us_per_mb_x100);
  printf("inter.us_per_mb_x100=%lu\n", (unsigned long)inter_per_mb_x100);
  printf("max_fps_x100=%lu\n", (unsigned long)((all_avg == 0U) ? 0U : (100000000U / all_avg)));
  printf("stream_kbytes=%lu\n", (unsigned long)(g_stream_bytes / 1024U));
  if (elapsed > 0U)
  {
    printf("stream_kbit_per_s=%lu\n",
           (unsigned long)((g_stream_bytes * 8U) / (uint64_t)elapsed));
  }
  printf("composite_mbs=%u\n", (unsigned)COMPOSITE_MACROBLOCKS);
  printf("composite_us=%lu\n", (unsigned long)composite_us);
  printf("composite_max_fps_x100=%lu\n",
         (unsigned long)((composite_us == 0U) ? 0U : (100000000U / composite_us)));
  printf("=== END ===\n");
}

/* ------------------------------------------------------------------------- */
/* Variants                                                                  */
/* ------------------------------------------------------------------------- */

/** @brief Refuse a change that would need the encoder reconfigured mid-stream. */
static bool pipeline_is_stopped(void)
{
  VENC_APP_Status_t status;

  VENC_APP_GetStatus(&status);
  if (status.state == VENC_APP_PIPELINE_STOPPED)
  {
    return true;
  }

  printf("VENC: 'stop' the pipeline first - the encoder is reinitialised by this\n");
  return false;
}

static int apply(void)
{
  if (VENC_APP_ReinitEncoder() != 0)
  {
    printf("VENC: the encoder refused the new configuration\n");
    return -1;
  }
  return 0;
}

int venc_bench_set_nv12(bool nv12)
{
  if (!pipeline_is_stopped())
  {
    return -1;
  }

  if (nv12)
  {
    hDcmippH264Instance.format          = DCMIPP_PIXEL_PACKER_FORMAT_YUV420_2;
    hDcmippH264Instance.pitch           = (uint32_t)hVencH264Instance.cfgH264Main.width;
    hDcmippH264Instance.bytes_per_pixel = 1.5F;
    hVencH264Instance.cfgH264Preproc.inputType = H264ENC_YUV420_SEMIPLANAR;
  }
  else
  {
    hDcmippH264Instance.format          = DCMIPP_PIXEL_PACKER_FORMAT_YUV422_1;
    hDcmippH264Instance.pitch           = 2U * (uint32_t)hVencH264Instance.cfgH264Main.width;
    hDcmippH264Instance.bytes_per_pixel = 2.0F;
    hVencH264Instance.cfgH264Preproc.inputType = H264ENC_YUV422_INTERLEAVED_YUYV;
  }

  if ((g_input_src == VENC_INPUT_FROM_AXISRAM) && !nv12)
  {
    /* YUYV is 1.33x the bytes of NV12 and the staging buffer was sized for the
       format that fits next to the bitstream ring. Say so rather than write
       past it. */
    printf("VENC: the AXISRAM staging buffer only holds NV12; input source back to capture\n");
    g_input_src = VENC_INPUT_FROM_CAPTURE;
  }

  return apply();
}

int venc_bench_set_input_src(venc_input_src_t src)
{
  uint32_t stage_size = 0U;

  if (!pipeline_is_stopped())
  {
    return -1;
  }

  if (src == VENC_INPUT_FROM_AXISRAM)
  {
    if (venc_bench_stage_buffer(&stage_size) == NULL)
    {
      printf("VENC: this build has no AXISRAM staging buffer - configure with "
             "-DVENC_M2_AXISRAM_INPUT=ON\n");
      return -1;
    }
    if (hDcmippH264Instance.format != DCMIPP_PIXEL_PACKER_FORMAT_YUV420_2)
    {
      printf("VENC: the staging buffer only holds NV12 - run 'format nv12' first\n");
      return -1;
    }
  }

  g_input_src = src;
  return 0;
}

int venc_bench_set_cabac(uint32_t mode)
{
  if (mode > 2U)
  {
    printf("VENC: cabac takes 0 (CAVLC), 1 (CABAC) or 2 (CAVLC intra / CABAC inter)\n");
    return -1;
  }
  if (!pipeline_is_stopped())
  {
    return -1;
  }

  hVencH264Instance.cfgH264Coding.enableCabac = mode;
  return apply();
}

int venc_bench_set_transform8x8(uint32_t mode)
{
  if (mode > 2U)
  {
    printf("VENC: t8x8 takes 0 (off), 1 (adaptive) or 2 (always)\n");
    return -1;
  }
  if (!pipeline_is_stopped())
  {
    return -1;
  }

  hVencH264Instance.cfgH264Coding.transform8x8Mode = mode;
  return apply();
}

int venc_bench_set_bitrate(uint32_t bits_per_second)
{
  if ((bits_per_second < 10000U) || (bits_per_second > 60000000U))
  {
    printf("VENC: the encoder takes 10000..60000000 bit/s\n");
    return -1;
  }
  if (!pipeline_is_stopped())
  {
    return -1;
  }

  hVencH264Instance.cfgH264Rate.bitPerSecond = bits_per_second;
  return apply();
}

venc_input_src_t venc_bench_input_src(void)
{
  return g_input_src;
}

void venc_bench_print_config(void)
{
  printf("VENC: %lux%lu %s from %s, cabac=%lu t8x8=%lu %lu bit/s gop=%lu, "
         "output %s\n",
         (unsigned long)hVencH264Instance.cfgH264Main.width,
         (unsigned long)hVencH264Instance.cfgH264Main.height,
         format_name(), input_src_name(),
         (unsigned long)hVencH264Instance.cfgH264Coding.enableCabac,
         (unsigned long)hVencH264Instance.cfgH264Coding.transform8x8Mode,
         (unsigned long)hVencH264Instance.cfgH264Rate.bitPerSecond,
         (unsigned long)hVencH264Instance.cfgH264Rate.gopLen,
         g_discard ? "discarded" : "recorded");
}
