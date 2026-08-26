/**
  ******************************************************************************
  * @file    csi_mux.c
  * @brief   Alternating two-channel capture into a composite frame - see csi_mux.h.
  ******************************************************************************
  */

#include "csi_mux.h"

#include <stdio.h>
#include <string.h>

#include "csi_phase.h"
#include "csi_probe.h"
#include "stm32n6570_discovery.h"
#include "stm32n6570_discovery_camera.h"
#include "utils.h"

extern DCMIPP_HandleTypeDef hcamera_dcmipp;

#define MUX_DEFAULT_SECONDS   60U

#define SEG_LUMA_SIZE     (CSI_MUX_SEG_W * CSI_MUX_SEG_H)
#define COMPOSITE_LUMA    (SEG_LUMA_SIZE * CSI_MUX_SEGMENTS)
#define COMPOSITE_SIZE    (COMPOSITE_LUMA + (COMPOSITE_LUMA / 2U))

/* One composite NV12 frame. Not double-buffered: pairing two segments into a
   complete composite is the encoder path's problem (PLAN.md phase 1), and this
   measurement is about whether the frames arrive at all. PSRAM is mapped
   non-cacheable, so what the DCMIPP writes is what the CPU reads back. */
static uint8_t composite[COMPOSITE_SIZE] ALIGN_32 IN_PSRAM;

static const uint32_t vc_stop_bit[4] =
{ CSI_CR_VC0STOP, CSI_CR_VC1STOP, CSI_CR_VC2STOP, CSI_CR_VC3STOP };

static csi_preview_source_t              g_src[CSI_MUX_SEGMENTS];
static DCMIPP_SemiPlanarDstAddressTypeDef g_addr[CSI_MUX_SEGMENTS];
static volatile uint32_t                 g_segment;      /* segment being filled now */
static volatile bool                     g_active;

static volatile uint32_t g_captured[CSI_MUX_SEGMENTS];
static volatile uint32_t g_vc_mismatch;   /* frames the pipe latched on the wrong VC */
static volatile uint32_t g_eof_late;      /* pipe interrupt beat the link frame end  */
static csi_stat_t        g_isr_cycles;    /* cost of the switch itself               */
static csi_stat_t        g_eof_to_switch; /* link frame end -> switch complete       */

/* A frame-complete interrupt that lands more than this after the last frame end
   on the wire did not belong to that frame end - the two interrupts are on
   different lines and either can win the race. Half a frame period at 48 fps. */
#define EOF_ASSOCIATION_LIMIT_US  10000U

/* ------------------------------------------------------------------------- */
/* Geometry                                                                  */
/* ------------------------------------------------------------------------- */

/** Region of the source that is scaled into one segment. */
typedef struct
{
  uint32_t hstart;
  uint32_t vstart;
  uint32_t hsize;
  uint32_t vsize;
} mux_roi_t;

/**
  * @brief  Largest centred region of @p src_w x @p src_h with a segment's shape.
  * @note   Cropping to the segment aspect ratio first is what keeps the picture
  *         undistorted: the downsize block scales the two axes independently,
  *         so squeezing 16:9 into a 1:2 segment without a crop would simply
  *         stretch it.
  */
static void mux_roi(uint32_t src_w, uint32_t src_h, mux_roi_t *roi)
{
  uint32_t want_w = (src_h * CSI_MUX_SEG_W) / CSI_MUX_SEG_H;

  if (want_w <= src_w)
  {
    roi->hsize = want_w & ~1U;
    roi->vsize = src_h & ~1U;
  }
  else
  {
    roi->hsize = src_w & ~1U;
    roi->vsize = ((src_w * CSI_MUX_SEG_H) / CSI_MUX_SEG_W) & ~1U;
  }

  roi->hstart = (src_w - roi->hsize) / 2U;
  roi->vstart = (src_h - roi->vsize) / 2U;
}

static void mux_segment_addresses(void)
{
  uint32_t y_base  = (uint32_t)composite;
  uint32_t uv_base = y_base + COMPOSITE_LUMA;

  for (uint32_t k = 0U; k < CSI_MUX_SEGMENTS; k++)
  {
    g_addr[k].YAddress  = y_base + (k * SEG_LUMA_SIZE);
    g_addr[k].UVAddress = uv_base + (k * (SEG_LUMA_SIZE / 2U));
  }
}

/* ------------------------------------------------------------------------- */
/* Pipe configuration                                                        */
/* ------------------------------------------------------------------------- */

/**
  * @brief  Configure PIPE1 to turn one RAW Bayer channel into an NV12 segment.
  * @note   Everything here is shared by both channels. Only the virtual channel
  *         and the two destination addresses change per frame, which is what
  *         makes the switch a pair of register writes rather than a
  *         reconfiguration.
  */
static HAL_StatusTypeDef mux_configure_pipe1(const csi_preview_source_t *src, const mux_roi_t *roi)
{
  DCMIPP_CSI_PIPE_ConfTypeDef csi_pipe = {0};
  DCMIPP_PipeConfTypeDef      pipe     = {0};
  DCMIPP_CropConfTypeDef      crop     = {0};
  DCMIPP_DownsizeTypeDef      ds       = {0};

  csi_pipe.DataTypeMode = DCMIPP_DTMODE_DTIDA;
  csi_pipe.DataTypeIDA  = src->dt;
  csi_pipe.DataTypeIDB  = src->dt;
  if (HAL_DCMIPP_CSI_PIPE_SetConfig(&hcamera_dcmipp, DCMIPP_PIPE1, &csi_pipe) != HAL_OK)
  {
    return HAL_ERROR;
  }

  /* That call only writes CMCR.INSEL while the handle is in INIT or READY; in
     any other state it silently does nothing and still returns HAL_OK. Routing
     the pixel pipes to the serial interface is not optional here, so it is
     written directly when the DCMIPP was left in parallel mode. Same reasoning
     and same code as csi_preview.c. */
  if (READ_BIT(hcamera_dcmipp.Instance->CMCR, DCMIPP_CMCR_INSEL) != DCMIPP_SERIAL_MODE)
  {
    MODIFY_REG(hcamera_dcmipp.Instance->P1FSCR,
               DCMIPP_P1FSCR_DTMODE | DCMIPP_P1FSCR_DTIDA | DCMIPP_P1FSCR_DTIDB,
               DCMIPP_DTMODE_DTIDA | (src->dt << DCMIPP_P1FSCR_DTIDA_Pos) |
               (src->dt << DCMIPP_P1FSCR_DTIDB_Pos));
    CLEAR_BIT(hcamera_dcmipp.Instance->PRCR, DCMIPP_PRCR_ENABLE);
    SET_BIT(hcamera_dcmipp.Instance->CMCR, DCMIPP_CMCR_INSEL);
  }

  /* Both channels carry the same format, so the same unpacking applies. */
  for (uint32_t k = 0U; k < CSI_MUX_SEGMENTS; k++)
  {
    (void)HAL_DCMIPP_CSI_SetVCConfig(&hcamera_dcmipp, g_src[k].vc,
                                     csi_probe_dt_bpp_code(src->dt));
  }

  /* The source dictates the frame rate; take every frame. */
  pipe.FrameRate         = DCMIPP_FRAME_RATE_ALL;
  pipe.PixelPackerFormat = DCMIPP_PIXEL_PACKER_FORMAT_YUV420_2;   /* NV12 */
  pipe.PixelPipePitch    = CSI_MUX_SEG_W;
  if (HAL_DCMIPP_PIPE_SetConfig(&hcamera_dcmipp, DCMIPP_PIPE1, &pipe) != HAL_OK)
  {
    return HAL_ERROR;
  }

  if (HAL_DCMIPP_PIPE_EnableRedBlueSwap(&hcamera_dcmipp, DCMIPP_PIPE1) != HAL_OK)
  {
    return HAL_ERROR;
  }

  /* RAW Bayer -> RGB. RGGB is measured against this source, not inherited: the
     four patterns were compared on a live picture with the probe's 'bayer'
     command. */
  DCMIPP_RawBayer2RGBConfTypeDef bayer =
  {
    .RawBayerType  = DCMIPP_RAWBAYER_RGGB,
    .PeakStrength  = DCMIPP_RAWBAYER_ALGO_STRENGTH_4,
    .VLineStrength = DCMIPP_RAWBAYER_ALGO_STRENGTH_8,
    .HLineStrength = DCMIPP_RAWBAYER_ALGO_STRENGTH_8,
    .EdgeStrength  = DCMIPP_RAWBAYER_ALGO_STRENGTH_16,
  };
  if (HAL_DCMIPP_PIPE_SetISPRawBayer2RGBConfig(&hcamera_dcmipp, DCMIPP_PIPE1, &bayer) != HAL_OK)
  {
    return HAL_ERROR;
  }
  if (HAL_DCMIPP_PIPE_EnableISPRawBayer2RGB(&hcamera_dcmipp, DCMIPP_PIPE1) != HAL_OK)
  {
    return HAL_ERROR;
  }

  /* RGB -> YUV, the same coefficients the encoder path uses. */
#define N10(val) (((val) ^ 0x7FF) + 1)
  DCMIPP_ColorConversionConfTypeDef yuv =
  {
    .ClampOutputSamples = ENABLE,
    .OutputSamplesType  = DCMIPP_CLAMP_YUV,
    .RR = N10(26), .RG = N10(87), .RB = 112,      .RA = 128,
    .GR = 47,      .GG = 157,     .GB = 16,       .GA = 16,
    .BR = 112,     .BG = N10(102), .BB = N10(10), .BA = 128,
  };
#undef N10
  if (HAL_DCMIPP_PIPE_SetYUVConversionConfig(&hcamera_dcmipp, DCMIPP_PIPE1, &yuv) != HAL_OK)
  {
    return HAL_ERROR;
  }
  if (HAL_DCMIPP_PIPE_EnableYUVConversion(&hcamera_dcmipp, DCMIPP_PIPE1) != HAL_OK)
  {
    return HAL_ERROR;
  }

  /* Crop sits ahead of the downsize block in the pipe (registers 0x904/0x908
     against 0x910), so the ratios below are taken against the cropped size. */
  crop.HStart   = roi->hstart;
  crop.VStart   = roi->vstart;
  crop.HSize    = roi->hsize;
  crop.VSize    = roi->vsize;
  crop.PipeArea = DCMIPP_POSITIVE_AREA;
  if (HAL_DCMIPP_PIPE_SetCropConfig(&hcamera_dcmipp, DCMIPP_PIPE1, &crop) != HAL_OK)
  {
    return HAL_ERROR;
  }
  if (HAL_DCMIPP_PIPE_EnableCrop(&hcamera_dcmipp, DCMIPP_PIPE1) != HAL_OK)
  {
    return HAL_ERROR;
  }

  /* RM0486 table 354: ratio is 8192 * src / dst, div factor (1024*8192-1)/ratio. */
  ds.HRatio     = (roi->hsize * 8192U) / CSI_MUX_SEG_W;
  ds.VRatio     = (roi->vsize * 8192U) / CSI_MUX_SEG_H;
  ds.HSize      = CSI_MUX_SEG_W;
  ds.VSize      = CSI_MUX_SEG_H;
  ds.HDivFactor = ((1024U * 8192U) - 1U) / ds.HRatio;
  ds.VDivFactor = ((1024U * 8192U) - 1U) / ds.VRatio;
  if (HAL_DCMIPP_PIPE_SetDownsizeConfig(&hcamera_dcmipp, DCMIPP_PIPE1, &ds) != HAL_OK)
  {
    return HAL_ERROR;
  }
  if (HAL_DCMIPP_PIPE_EnableDownsize(&hcamera_dcmipp, DCMIPP_PIPE1) != HAL_OK)
  {
    return HAL_ERROR;
  }

  /* Frame mode: the line event would only add interrupts, and the multi-line
     trigger is pushed out for the same reason the encoder path does it. */
  MODIFY_REG(DCMIPP->P1PPCR, DCMIPP_P1PPCR_LINEMULT_Msk, DCMIPP_MULTILINE_128_LINES);
  (void)HAL_DCMIPP_PIPE_DisableLineEvent(&hcamera_dcmipp, DCMIPP_PIPE1);

  return HAL_OK;
}

/* ------------------------------------------------------------------------- */
/* Per-frame switch                                                          */
/* ------------------------------------------------------------------------- */

bool csi_mux_active(void)
{
  return g_active;
}

void csi_mux_on_pipe1_frame(void)
{
  uint32_t entry = DWT->CYCCNT;
  uint32_t done;
  uint32_t next;
  uint32_t latched;
  uint32_t eof_cyc;
  uint32_t since_eof;

  if (!g_active)
  {
    return;
  }

  done = g_segment;

  /* P1CFSCR is the active copy of the flow selection, so it still names the
     channel the finished frame came from - which is how a switch that did not
     take effect becomes visible instead of being assumed away. */
  latched = (DCMIPP->P1CFSCR & DCMIPP_P1FSCR_VC) >> DCMIPP_P1FSCR_VC_Pos;
  if (latched != g_src[done].vc)
  {
    g_vc_mismatch++;
  }

  g_captured[done]++;

  eof_cyc   = csi_phase_last_eof_cyc(g_src[done].vc);
  since_eof = entry - eof_cyc;
  if ((eof_cyc != 0U) &&
      (since_eof < (EOF_ASSOCIATION_LIMIT_US * csi_phase_cycles_per_us())))
  {
    csi_stat_add(&g_eof_to_switch, since_eof);
  }
  else
  {
    g_eof_late++;
  }

  next = (done + 1U) % CSI_MUX_SEGMENTS;

  /* P1FSCR is the shadow copy; the DCMIPP latches it into P1CFSCR at the next
     frame start, which is exactly the boundary this interrupt sits on. The
     destination addresses are latched the same way. */
  MODIFY_REG(DCMIPP->P1FSCR, DCMIPP_P1FSCR_VC,
             g_src[next].vc << DCMIPP_P1FSCR_VC_Pos);
  (void)HAL_DCMIPP_PIPE_SetSemiPlanarMemoryAddress(&hcamera_dcmipp, DCMIPP_PIPE1,
                                                   &g_addr[next]);

  g_segment = next;
  csi_stat_add(&g_isr_cycles, DWT->CYCCNT - entry);
}

/* ------------------------------------------------------------------------- */
/* M1                                                                        */
/* ------------------------------------------------------------------------- */

/** @brief Mean luma over a sample grid of one segment, as a sign of life. */
static uint32_t segment_luma_mean(uint32_t segment)
{
  const uint8_t *y = &composite[segment * SEG_LUMA_SIZE];
  uint32_t       sum = 0U;
  uint32_t       n   = 0U;

  for (uint32_t i = 0U; i < SEG_LUMA_SIZE; i += 331U)   /* prime stride: no row bias */
  {
    sum += y[i];
    n++;
  }
  return (n == 0U) ? 0U : (sum / n);
}

int csi_mux_run(const csi_preview_source_t *a, const csi_preview_source_t *b, uint32_t seconds)
{
  csi_phase_vc_t baseline[CSI_PHASE_VC_COUNT];
  csi_phase_vc_t final[CSI_PHASE_VC_COUNT];
  mux_roi_t      roi;
  uint32_t       vc_mask;
  uint32_t       started;
  uint32_t       window_ms;
  uint32_t       captured_total = 0U;

  if ((a == NULL) || (b == NULL))
  {
    return -1;
  }

  if ((a->width != b->width) || (a->height != b->height) || (a->dt != b->dt))
  {
    printf("MUX: both channels need the same geometry and data type "
           "(VC%lu %lux%lu dt0x%02lx vs VC%lu %lux%lu dt0x%02lx)\n",
           (unsigned long)a->vc, (unsigned long)a->width, (unsigned long)a->height,
           (unsigned long)a->dt,
           (unsigned long)b->vc, (unsigned long)b->width, (unsigned long)b->height,
           (unsigned long)b->dt);
    return -1;
  }
  if (a->vc == b->vc)
  {
    printf("MUX: the two channels must differ\n");
    return -1;
  }
  if ((a->width < CSI_MUX_SEG_W) || (a->height < CSI_MUX_SEG_H))
  {
    printf("MUX: source %lux%lu is smaller than a %ux%u segment; the downsize "
           "block cannot upscale\n",
           (unsigned long)a->width, (unsigned long)a->height,
           (unsigned)CSI_MUX_SEG_W, (unsigned)CSI_MUX_SEG_H);
    return -1;
  }

  if (seconds == 0U)
  {
    seconds = MUX_DEFAULT_SECONDS;
  }

  g_src[0] = *a;
  g_src[1] = *b;
  vc_mask  = (1UL << a->vc) | (1UL << b->vc);

  mux_roi(a->width, a->height, &roi);
  mux_segment_addresses();
  memset(composite, 0, sizeof(composite));

  g_segment     = 0U;
  g_vc_mismatch = 0U;
  g_eof_late    = 0U;
  g_captured[0] = 0U;
  g_captured[1] = 0U;
  csi_stat_reset(&g_isr_cycles);
  csi_stat_reset(&g_eof_to_switch);

  if (mux_configure_pipe1(a, &roi) != HAL_OK)
  {
    printf("MUX: could not configure PIPE1\n");
    return -1;
  }

  /* The link-level observer has to be counting before the pipe starts, so that
     what the source sent and what the pipe kept cover the same window. It also
     leaves both channels running, which the pipe needs in order to hop between
     them - the pipe start only ever starts one. */
  csi_phase_observer_start(vc_mask);

  if (HAL_DCMIPP_CSI_PIPE_SemiPlanarStart(&hcamera_dcmipp, DCMIPP_PIPE1, g_src[0].vc,
                                          &g_addr[0], CAMERA_MODE_CONTINUOUS) != HAL_OK)
  {
    printf("MUX: could not start PIPE1 on VC%lu (state %d, CMCR 0x%08lx)\n",
           (unsigned long)g_src[0].vc, (int)hcamera_dcmipp.PipeState[DCMIPP_PIPE1],
           (unsigned long)hcamera_dcmipp.Instance->CMCR);
    csi_phase_observer_stop();
    return -1;
  }

  g_active = true;
  csi_phase_observer_snapshot(baseline);
  started = HAL_GetTick();

  printf("MUX: capturing VC%lu and VC%lu alternately for %lu s\n",
         (unsigned long)a->vc, (unsigned long)b->vc, (unsigned long)seconds);

  for (uint32_t s = 0U; s < seconds; s++)
  {
    HAL_Delay(1000U);
    printf(".");
  }
  printf("\n");

  window_ms = HAL_GetTick() - started;
  csi_phase_observer_snapshot(final);
  g_active = false;

  (void)HAL_DCMIPP_CSI_PIPE_Stop(&hcamera_dcmipp, DCMIPP_PIPE1, g_src[0].vc);
  SET_BIT(CSI->CR, vc_stop_bit[g_src[1].vc]);
  csi_phase_observer_stop();

  printf("=== MUX RESULT ===\n");
  printf("cpu_hz=%lu\n", (unsigned long)HAL_RCC_GetCpuClockFreq());
  printf("window_ms=%lu\n", (unsigned long)window_ms);
  printf("source=%lux%lu\n", (unsigned long)a->width, (unsigned long)a->height);
  printf("crop=%lux%lu+%lu+%lu\n", (unsigned long)roi.hsize, (unsigned long)roi.vsize,
         (unsigned long)roi.hstart, (unsigned long)roi.vstart);
  printf("segment=%ux%u\n", (unsigned)CSI_MUX_SEG_W, (unsigned)CSI_MUX_SEG_H);
  printf("composite=%ux%u\n", (unsigned)CSI_MUX_SEG_W,
         (unsigned)(CSI_MUX_SEG_H * CSI_MUX_SEGMENTS));
  printf("format=NV12 pitch=%u\n", (unsigned)CSI_MUX_SEG_W);
  printf("composite_base=0x%08lx\n", (unsigned long)composite);
  printf("composite_bytes=%lu\n", (unsigned long)COMPOSITE_SIZE);

  for (uint32_t k = 0U; k < CSI_MUX_SEGMENTS; k++)
  {
    uint32_t vc   = g_src[k].vc;
    uint32_t sent = final[vc].eof - baseline[vc].eof;
    uint32_t kept = g_captured[k];

    captured_total += kept;

    printf("seg%lu.vc=%lu\n", (unsigned long)k, (unsigned long)vc);
    printf("seg%lu.y_addr=0x%08lx\n", (unsigned long)k, (unsigned long)g_addr[k].YAddress);
    printf("seg%lu.uv_addr=0x%08lx\n", (unsigned long)k, (unsigned long)g_addr[k].UVAddress);
    printf("src.vc%lu.frames=%lu\n", (unsigned long)vc, (unsigned long)sent);
    printf("cap.vc%lu.frames=%lu\n", (unsigned long)vc, (unsigned long)kept);
    if (window_ms > 0U)
    {
      printf("src.vc%lu.fps_x100=%lu\n", (unsigned long)vc,
             (unsigned long)(((uint64_t)sent * 100000U) / (uint64_t)window_ms));
      printf("cap.vc%lu.fps_x100=%lu\n", (unsigned long)vc,
             (unsigned long)(((uint64_t)kept * 100000U) / (uint64_t)window_ms));
    }
    printf("drops.vc%lu=%ld\n", (unsigned long)vc, (long)((int32_t)sent - (int32_t)kept));
    if (sent > 0U)
    {
      printf("keep_pct_x100.vc%lu=%lu\n", (unsigned long)vc,
             (unsigned long)(((uint64_t)kept * 10000U) / (uint64_t)sent));
    }
    printf("seg%lu.luma_mean=%lu\n", (unsigned long)k,
           (unsigned long)segment_luma_mean(k));
  }

  printf("captured_total=%lu\n", (unsigned long)captured_total);
  printf("vc_mismatch=%lu\n", (unsigned long)g_vc_mismatch);
  printf("eof_unassociated=%lu\n", (unsigned long)g_eof_late);
  csi_stat_print("", "isr_switch", &g_isr_cycles);
  csi_stat_print("", "eof_to_switch", &g_eof_to_switch);
  printf("p1fscr=0x%08lx\n", (unsigned long)DCMIPP->P1FSCR);
  printf("p1cfscr=0x%08lx\n", (unsigned long)DCMIPP->P1CFSCR);
  printf("dcmipp_error=0x%08lx\n", (unsigned long)hcamera_dcmipp.ErrorCode);
  printf("csi_sr0=0x%08lx\n", (unsigned long)CSI->SR0);
  printf("=== END ===\n");

  if ((g_captured[0] == 0U) || (g_captured[1] == 0U))
  {
    printf("MUX: one segment never received a frame - the per-frame channel\n"
           "     switch is not taking effect\n");
  }

  return 0;
}
