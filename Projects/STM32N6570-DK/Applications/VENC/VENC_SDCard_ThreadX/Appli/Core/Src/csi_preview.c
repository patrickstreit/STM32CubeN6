/**
  ******************************************************************************
  * @file    csi_preview.c
  * @brief   Live preview of one or two CSI-2 virtual channels - see csi_preview.h.
  ******************************************************************************
  */

#include "csi_preview.h"

#include <stdio.h>
#include <string.h>

#include "csi_probe.h"
#include "stm32n6570_discovery.h"
#include "stm32n6570_discovery_camera.h"
#include "stm32n6570_discovery_lcd.h"
#include "utils.h"

extern DCMIPP_HandleTypeDef hcamera_dcmipp;

#define LCD_BPP          2U   /* RGB565 */
#define LCD_PITCH        (LCD_DEFAULT_WIDTH * LCD_BPP)
#define LCD_FRAME_SIZE   (LCD_DEFAULT_WIDTH * LCD_DEFAULT_HEIGHT * LCD_BPP)

/* Vertical placement of both tiles, and horizontal origin of the left tile when
   two are shown. Every offset is a multiple of 16 bytes because the pitch is
   1600 and the tile width is 400 pixels: HAL_DCMIPP_CSI_PIPE_Start() rejects a
   destination address whose low four bits are set. */
#define TILE_Y           ((LCD_DEFAULT_HEIGHT - CSI_PREVIEW_TILE_H) / 2U)
#define DUAL_LEFT_X      0U
#define DUAL_RIGHT_X     CSI_PREVIEW_TILE_W
#define SINGLE_X         ((LCD_DEFAULT_WIDTH - CSI_PREVIEW_TILE_W) / 2U)

static uint8_t preview_frame[LCD_FRAME_SIZE] ALIGN_32 IN_PSRAM;

static csi_preview_mode_t   preview_mode = CSI_PREVIEW_OFF;
static csi_preview_source_t preview_src[2];
static uint32_t             preview_tile_addr[2];
static uint32_t             preview_tile_count[2];
static volatile uint32_t    preview_next_tile;   /* tile the pipe is currently filling */
static bool                 lcd_ready;

/* Which corner of the 2x2 Bayer cell is red. RGGB - checked against this source
   by stepping through all four with 'bayer' and looking at the picture, which is
   the only way to decide it: nothing in a CSI-2 stream states the pattern. */
static uint32_t             preview_bayer = DCMIPP_RAWBAYER_RGGB;

/* ------------------------------------------------------------------------- */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------- */

static uint32_t tile_address(uint32_t x, uint32_t y)
{
  return (uint32_t)preview_frame + (y * LCD_PITCH) + (x * LCD_BPP);
}

/** @brief Program PIPE1's downsize block to map @p src_w x @p src_h onto one tile. */
static HAL_StatusTypeDef preview_set_downsize(uint32_t src_w, uint32_t src_h)
{
  DCMIPP_DownsizeTypeDef ds = {0};

  if ((src_w < CSI_PREVIEW_TILE_W) || (src_h < CSI_PREVIEW_TILE_H))
  {
    printf("PREVIEW: source %lux%lu is smaller than a %ux%u tile; upscaling is not supported\n",
           (unsigned long)src_w, (unsigned long)src_h,
           (unsigned)CSI_PREVIEW_TILE_W, (unsigned)CSI_PREVIEW_TILE_H);
    return HAL_ERROR;
  }

  /* RM0486 table 354: ratio is 8192 * src / dst, div factor is (1024*8192-1)/ratio. */
  ds.HRatio     = (src_w * 8192U) / CSI_PREVIEW_TILE_W;
  ds.VRatio     = (src_h * 8192U) / CSI_PREVIEW_TILE_H;
  ds.HSize      = CSI_PREVIEW_TILE_W;
  ds.VSize      = CSI_PREVIEW_TILE_H;
  ds.HDivFactor = ((1024U * 8192U) - 1U) / ds.HRatio;
  ds.VDivFactor = ((1024U * 8192U) - 1U) / ds.VRatio;

  if (HAL_DCMIPP_PIPE_SetDownsizeConfig(&hcamera_dcmipp, DCMIPP_PIPE1, &ds) != HAL_OK)
  {
    return HAL_ERROR;
  }
  return HAL_DCMIPP_PIPE_EnableDownsize(&hcamera_dcmipp, DCMIPP_PIPE1);
}

/* What the DCMIPP looked like around the pipe configuration. Kept so that a
   failed start can report what happened instead of what might have happened:
   the state a HAL call decided on is overwritten by the call itself. */
static uint8_t  cfg_state_before;
static uint8_t  cfg_state_after;
static uint32_t cfg_cmcr;
static uint32_t cfg_p1fscr;

/**
  * @brief  Configure PIPE1 to turn @p dt into RGB565 tiles.
  * @note   Only the demosaicing branch is enabled; the YUV conversion of the
  *         encoder path is deliberately left off so the pixel packer receives
  *         RGB and can emit RGB565 straight into the framebuffer.
  */
static HAL_StatusTypeDef preview_configure_pipe1(const csi_preview_source_t *src)
{
  DCMIPP_CSI_PIPE_ConfTypeDef csi_pipe = {0};
  DCMIPP_PipeConfTypeDef      pipe     = {0};
  uint32_t                    bpp      = csi_probe_dt_bpp(src->dt);

  csi_pipe.DataTypeMode = DCMIPP_DTMODE_DTIDA;
  csi_pipe.DataTypeIDA  = src->dt;
  csi_pipe.DataTypeIDB  = src->dt;
  cfg_state_before      = (uint8_t)hcamera_dcmipp.State;
  if (HAL_DCMIPP_CSI_PIPE_SetConfig(&hcamera_dcmipp, DCMIPP_PIPE1, &csi_pipe) != HAL_OK)
  {
    return HAL_ERROR;
  }
  cfg_state_after = (uint8_t)hcamera_dcmipp.State;
  cfg_cmcr        = hcamera_dcmipp.Instance->CMCR;
  cfg_p1fscr      = hcamera_dcmipp.Instance->P1FSCR;

  /* That call writes CMCR.INSEL only while the handle is in INIT or READY. In
     any other state it does nothing at all, returns HAL_OK and then claims
     READY - so the flow selection above may not have been written either.
     Routing the pixel pipes to the serial interface is not optional here, so
     do it directly when the call left the DCMIPP in parallel mode. */
  if (READ_BIT(hcamera_dcmipp.Instance->CMCR, DCMIPP_CMCR_INSEL) != DCMIPP_SERIAL_MODE)
  {
    MODIFY_REG(hcamera_dcmipp.Instance->P1FSCR,
               DCMIPP_P1FSCR_DTMODE | DCMIPP_P1FSCR_DTIDA | DCMIPP_P1FSCR_DTIDB,
               DCMIPP_DTMODE_DTIDA | (src->dt << DCMIPP_P1FSCR_DTIDA_Pos) |
               (src->dt << DCMIPP_P1FSCR_DTIDB_Pos));
    CLEAR_BIT(hcamera_dcmipp.Instance->PRCR, DCMIPP_PRCR_ENABLE);
    SET_BIT(hcamera_dcmipp.Instance->CMCR, DCMIPP_CMCR_INSEL);
  }

  /* The external source dictates the frame rate; take every frame. */
  pipe.FrameRate         = DCMIPP_FRAME_RATE_ALL;
  pipe.PixelPackerFormat = DCMIPP_PIXEL_PACKER_FORMAT_RGB565_1;
  /* Pitch is the full screen, not the tile: that is what lets the pipe write a
     sub-rectangle of the framebuffer without any copy. */
  pipe.PixelPipePitch    = LCD_PITCH;
  if (HAL_DCMIPP_PIPE_SetConfig(&hcamera_dcmipp, DCMIPP_PIPE1, &pipe) != HAL_OK)
  {
    return HAL_ERROR;
  }

  if ((src->dt == DCMIPP_DT_RAW8) || (src->dt == DCMIPP_DT_RAW10) ||
      (src->dt == DCMIPP_DT_RAW12) || (src->dt == DCMIPP_DT_RAW14))
  {
    /* Bayer pattern is a property of the sensor and cannot be probed from the
       CSI side - nothing in the CSI-2 stream says which corner is red. It has to
       be decided by looking at the picture, which is what 'bayer' is for. */
    DCMIPP_RawBayer2RGBConfTypeDef bayer =
    {
      .RawBayerType  = preview_bayer,
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

    /* Without any exposure control a linear raw frame is very dark on screen;
       the gamma block costs nothing and makes the preview usable. */
    (void)HAL_DCMIPP_PIPE_EnableGammaConversion(&hcamera_dcmipp, DCMIPP_PIPE1);
  }
  else
  {
    (void)HAL_DCMIPP_PIPE_DisableISPRawBayer2RGB(&hcamera_dcmipp, DCMIPP_PIPE1);
    (void)HAL_DCMIPP_PIPE_DisableGammaConversion(&hcamera_dcmipp, DCMIPP_PIPE1);

    if (bpp == 0U)
    {
      printf("PREVIEW: data type 0x%02lx (%s) is not a pixel format this preview handles\n",
             (unsigned long)src->dt, csi_probe_dt_name(src->dt));
      return HAL_ERROR;
    }
    if ((src->dt == DCMIPP_DT_YUV422_8) || (src->dt == DCMIPP_DT_YUV422_10) ||
        (src->dt == DCMIPP_DT_YUV420_8) || (src->dt == DCMIPP_DT_YUV420_10))
    {
      /* The pixel packer converts RGB to YUV, not the other way round: there is
         no YUV-to-RGB block ahead of it. */
      printf("PREVIEW: YUV sources cannot be shown as RGB565 by the DCMIPP; "
             "capture them as YUV422 instead\n");
      return HAL_ERROR;
    }
  }

  /* The pipe never needs the YUV matrix here, and it would corrupt the RGB565
     packing if the encoder path left it enabled. */
  (void)HAL_DCMIPP_PIPE_DisableYUVConversion(&hcamera_dcmipp, DCMIPP_PIPE1);

  if (preview_set_downsize(src->width, src->height) != HAL_OK)
  {
    return HAL_ERROR;
  }

  /* No line events: only the frame-complete interrupt drives the tile swap. */
  (void)HAL_DCMIPP_PIPE_DisableLineEvent(&hcamera_dcmipp, DCMIPP_PIPE1);

  return HAL_OK;
}

static const uint32_t vc_start_bit[4] =
{ CSI_CR_VC0START, CSI_CR_VC1START, CSI_CR_VC2START, CSI_CR_VC3START };

static const uint32_t vc_stop_bit[4] =
{ CSI_CR_VC0STOP, CSI_CR_VC1STOP, CSI_CR_VC2STOP, CSI_CR_VC3STOP };

/**
  * @brief  Start a CSI virtual channel that PIPE1 is not being started on.
  * @note   The channel goes active only after the receiver has seen a frame
  *         start on it, so the timeout covers several frame periods.
  */
static bool preview_start_extra_vc(uint32_t vc)
{
  uint32_t tickstart;

  SET_BIT(CSI->CR, vc_start_bit[vc]);

  tickstart = HAL_GetTick();
  while ((CSI->SR0 & (CSI_SR0_VC0STATEF << vc)) == 0U)
  {
    if ((HAL_GetTick() - tickstart) > 150U)
    {
      return false;
    }
  }
  return true;
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

int csi_preview_lcd_init(void)
{
  if (lcd_ready)
  {
    return 0;
  }

  memset(preview_frame, 0, sizeof(preview_frame));

  if (BSP_LCD_InitEx(0, LCD_ORIENTATION_LANDSCAPE, LCD_PIXEL_FORMAT_RGB565,
                     LCD_DEFAULT_WIDTH, LCD_DEFAULT_HEIGHT) != BSP_ERROR_NONE)
  {
    printf("PREVIEW: BSP_LCD_InitEx failed\n");
    return -1;
  }

  BSP_LCD_SetLayerAddress(0, 0, (uint32_t)preview_frame);
  lcd_ready = true;
  return 0;
}

void csi_preview_stop(void)
{
  if (preview_mode != CSI_PREVIEW_OFF)
  {
    /* One pipe stop, then both channels: HAL_DCMIPP_CSI_PIPE_Stop() only stops
       the channel it is given, and calling it twice on the same pipe would fail
       the second time because the pipe is no longer capturing. */
    (void)HAL_DCMIPP_CSI_PIPE_Stop(&hcamera_dcmipp, DCMIPP_PIPE1, preview_src[0].vc);
    if (preview_mode == CSI_PREVIEW_DUAL_ALT)
    {
      SET_BIT(CSI->CR, vc_stop_bit[preview_src[1].vc]);
    }
  }

  preview_mode          = CSI_PREVIEW_OFF;
  preview_tile_count[0] = 0U;
  preview_tile_count[1] = 0U;
  preview_next_tile     = 0U;

  if (lcd_ready)
  {
    memset(preview_frame, 0, sizeof(preview_frame));
  }
}

/**
  * @brief  Say why HAL_DCMIPP_CSI_PIPE_Start() refused.
  *
  * It has three entry guards and returns the same HAL_ERROR for all of them, so
  * the message alone leaves nothing to act on. Each is cheap to read back.
  */
static void preview_report_start_failure(uint32_t vc, uint32_t addr)
{
  uint32_t insel = READ_BIT(hcamera_dcmipp.Instance->CMCR, DCMIPP_CMCR_INSEL);

  printf("PREVIEW: could not start PIPE1 on VC%lu\n", (unsigned long)vc);
  printf("         DCMIPP state %u before the pipe configuration, %u after;\n"
         "         CMCR 0x%08lx, P1FSCR 0x%08lx directly after it\n",
         (unsigned)cfg_state_before, (unsigned)cfg_state_after,
         (unsigned long)cfg_cmcr, (unsigned long)cfg_p1fscr);

  if ((addr & 0xFU) != 0U)
  {
    printf("         destination 0x%08lx is not 16-byte aligned\n", (unsigned long)addr);
  }
  if (hcamera_dcmipp.PipeState[DCMIPP_PIPE1] != HAL_DCMIPP_PIPE_STATE_READY)
  {
    printf("         PIPE1 state is %d, not READY\n",
           (int)hcamera_dcmipp.PipeState[DCMIPP_PIPE1]);
  }
  if (insel != DCMIPP_SERIAL_MODE)
  {
    /* The configuration above sets CMCR.INSEL directly when the HAL skipped it,
       so reaching this point means the write itself did not stick - which is a
       clock or reset problem in the DCMIPP, not a sequencing one. */
    printf("         CMCR.INSEL reads back as parallel although it was written;\n"
           "         the DCMIPP is not clocked or is held in reset\n");
  }
}

int csi_preview_single(const csi_preview_source_t *src)
{
  if (src == NULL)
  {
    return -1;
  }

  csi_preview_stop();

  if (csi_preview_lcd_init() != 0)
  {
    return -1;
  }

  preview_src[0]       = *src;
  preview_tile_addr[0] = tile_address(SINGLE_X, TILE_Y);

  if (preview_configure_pipe1(&preview_src[0]) != HAL_OK)
  {
    return -1;
  }

  if (HAL_DCMIPP_CSI_PIPE_Start(&hcamera_dcmipp, DCMIPP_PIPE1, preview_src[0].vc,
                                preview_tile_addr[0], CAMERA_MODE_CONTINUOUS) != HAL_OK)
  {
    preview_report_start_failure(preview_src[0].vc, preview_tile_addr[0]);
    return -1;
  }

  preview_next_tile = 0U;
  preview_mode      = CSI_PREVIEW_SINGLE;
  printf("PREVIEW: VC%lu %lux%lu -> %ux%u at x=%u\n",
         (unsigned long)src->vc, (unsigned long)src->width, (unsigned long)src->height,
         (unsigned)CSI_PREVIEW_TILE_W, (unsigned)CSI_PREVIEW_TILE_H, (unsigned)SINGLE_X);
  return 0;
}

int csi_preview_dual(const csi_preview_source_t *left, const csi_preview_source_t *right)
{
  if ((left == NULL) || (right == NULL))
  {
    return -1;
  }

  if ((left->width != right->width) || (left->height != right->height) || (left->dt != right->dt))
  {
    /* PIPE1 is shared between both tiles and only its virtual channel and
       destination address are swapped per frame; geometry and data type are
       part of the shared configuration. */
    printf("PREVIEW: dual mode needs identical geometry and data type on both channels "
           "(VC%lu %lux%lu dt0x%02lx vs VC%lu %lux%lu dt0x%02lx)\n",
           (unsigned long)left->vc, (unsigned long)left->width, (unsigned long)left->height,
           (unsigned long)left->dt,
           (unsigned long)right->vc, (unsigned long)right->width, (unsigned long)right->height,
           (unsigned long)right->dt);
    return -1;
  }

  csi_preview_stop();

  if (csi_preview_lcd_init() != 0)
  {
    return -1;
  }

  preview_src[0]       = *left;
  preview_src[1]       = *right;
  preview_tile_addr[0] = tile_address(DUAL_LEFT_X,  TILE_Y);
  preview_tile_addr[1] = tile_address(DUAL_RIGHT_X, TILE_Y);

  if (preview_configure_pipe1(&preview_src[0]) != HAL_OK)
  {
    return -1;
  }

  /* Both channels must be running in the CSI before the pipe starts hopping
     between them; only the first one is started by the pipe-start call. */
  if (!preview_start_extra_vc(preview_src[1].vc))
  {
    printf("PREVIEW: VC%lu never reported the active state; continuing anyway\n",
           (unsigned long)preview_src[1].vc);
  }

  if (HAL_DCMIPP_CSI_PIPE_Start(&hcamera_dcmipp, DCMIPP_PIPE1, preview_src[0].vc,
                                preview_tile_addr[0], CAMERA_MODE_CONTINUOUS) != HAL_OK)
  {
    preview_report_start_failure(preview_src[0].vc, preview_tile_addr[0]);
    return -1;
  }

  preview_next_tile = 0U;
  preview_mode      = CSI_PREVIEW_DUAL_ALT;
  printf("PREVIEW: VC%lu left, VC%lu right, %lux%lu -> 2 x %ux%u, "
         "each channel at half the source frame rate\n",
         (unsigned long)left->vc, (unsigned long)right->vc,
         (unsigned long)left->width, (unsigned long)left->height,
         (unsigned)CSI_PREVIEW_TILE_W, (unsigned)CSI_PREVIEW_TILE_H);
  return 0;
}

void csi_preview_on_pipe1_frame(void)
{
  uint32_t done_tile = preview_next_tile;
  uint32_t next_tile;

  if (preview_mode == CSI_PREVIEW_OFF)
  {
    return;
  }

  preview_tile_count[done_tile]++;

  if (preview_mode == CSI_PREVIEW_SINGLE)
  {
    return;
  }

  next_tile = done_tile ^ 1U;

  /* P1FSCR is the shadow copy of the flow selection; the DCMIPP latches it into
     P1CFSCR at the next frame start, which is exactly the boundary we are on.
     Same for the destination address. */
  MODIFY_REG(DCMIPP->P1FSCR, DCMIPP_P1FSCR_VC,
             preview_src[next_tile].vc << DCMIPP_P1FSCR_VC_Pos);
  (void)HAL_DCMIPP_PIPE_SetMemoryAddress(&hcamera_dcmipp, DCMIPP_PIPE1,
                                         DCMIPP_MEMORY_ADDRESS_0,
                                         preview_tile_addr[next_tile]);

  preview_next_tile = next_tile;
}

void csi_preview_print_stats(void)
{
  printf("PREVIEW: mode=%s tiles=%lu/%lu P1FSCR=0x%08lx P1CFSCR=0x%08lx\n",
         (preview_mode == CSI_PREVIEW_OFF)    ? "off" :
         (preview_mode == CSI_PREVIEW_SINGLE) ? "single" : "dual-alt",
         (unsigned long)preview_tile_count[0],
         (unsigned long)preview_tile_count[1],
         (unsigned long)DCMIPP->P1FSCR,
         (unsigned long)DCMIPP->P1CFSCR);

  if (preview_mode == CSI_PREVIEW_DUAL_ALT)
  {
    /* If one counter stays at zero the per-frame virtual channel switch is not
       taking effect and the alternating scheme has to be reconsidered. */
    printf("         current VC in flight: %lu (expect it to alternate between %lu and %lu)\n",
           (unsigned long)((DCMIPP->P1CFSCR & DCMIPP_P1FSCR_VC) >> DCMIPP_P1FSCR_VC_Pos),
           (unsigned long)preview_src[0].vc, (unsigned long)preview_src[1].vc);
  }
}

csi_preview_mode_t csi_preview_get_mode(void)
{
  return preview_mode;
}

int csi_preview_set_bayer(uint32_t pattern)
{
  static const uint32_t patterns[4] =
  {
    DCMIPP_RAWBAYER_RGGB, DCMIPP_RAWBAYER_GRBG,
    DCMIPP_RAWBAYER_GBRG, DCMIPP_RAWBAYER_BGGR
  };
  static const char *names[4] = { "RGGB", "GRBG", "GBRG", "BGGR" };

  DCMIPP_RawBayer2RGBConfTypeDef bayer =
  {
    .PeakStrength  = DCMIPP_RAWBAYER_ALGO_STRENGTH_4,
    .VLineStrength = DCMIPP_RAWBAYER_ALGO_STRENGTH_8,
    .HLineStrength = DCMIPP_RAWBAYER_ALGO_STRENGTH_8,
    .EdgeStrength  = DCMIPP_RAWBAYER_ALGO_STRENGTH_16,
  };

  if (pattern > 3U)
  {
    printf("PREVIEW: bayer takes 0..3 - 0 RGGB, 1 GRBG, 2 GBRG, 3 BGGR\n");
    return -1;
  }

  preview_bayer      = patterns[pattern];
  bayer.RawBayerType = preview_bayer;

  /* The demosaic configuration is a single register write with no state guard,
     so this takes effect on the next frame without stopping the pipe - which is
     the point: the four patterns can be compared on a live picture. */
  if (HAL_DCMIPP_PIPE_SetISPRawBayer2RGBConfig(&hcamera_dcmipp, DCMIPP_PIPE1, &bayer) != HAL_OK)
  {
    printf("PREVIEW: could not set the Bayer pattern\n");
    return -1;
  }

  printf("PREVIEW: Bayer pattern %lu (%s)\n", (unsigned long)pattern, names[pattern]);
  return 0;
}
