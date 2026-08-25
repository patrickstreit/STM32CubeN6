/**
  ******************************************************************************
  * @file    csi_preview.h
  * @brief   Live preview of one or two CSI-2 virtual channels on the DK display.
  *
  * Hardware constraint that shapes this module
  * -------------------------------------------
  * The DCMIPP has exactly one demosaicing block and one colour-conversion block,
  * and both sit in PIPE1 (registers P1DMCR / P1CCxx / P1YUVxx). PIPE2 has no
  * such registers: it only has crop, decimation, downsize, gamma and the pixel
  * packer. PIPE2 can therefore either
  *
  *   - share PIPE1's flow (P1FSCR.PIPEDIFF = 0), in which case it sees the same
  *     virtual channel as PIPE1 and just scales it differently, or
  *   - run on its own virtual channel (PIPEDIFF = 1), in which case it receives
  *     the raw Bayer mosaic with no demosaicing available.
  *
  * So two RAW Bayer virtual channels cannot both be turned into colour at the
  * same time. To show both in colour, PIPE1 is time-multiplexed: it switches
  * virtual channel and destination address at every frame-complete interrupt,
  * writing straight into the left or right half of the LTDC framebuffer. Each
  * channel is then displayed at half the source frame rate, which is the
  * trade-off this silicon forces.
  ******************************************************************************
  */

#ifndef CSI_PREVIEW_H
#define CSI_PREVIEW_H

#include <stdbool.h>
#include <stdint.h>

#include "stm32n6xx_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Width of one preview tile; two tiles fit side by side on the 800x480 panel. */
#define CSI_PREVIEW_TILE_W   400U
/** Height of one preview tile, 16:9 against CSI_PREVIEW_TILE_W. */
#define CSI_PREVIEW_TILE_H   225U

typedef enum
{
  CSI_PREVIEW_OFF = 0,
  CSI_PREVIEW_SINGLE,    /*!< one virtual channel, centred, one tile wide      */
  CSI_PREVIEW_DUAL_ALT   /*!< two channels side by side, PIPE1 alternating     */
} csi_preview_mode_t;

/** Source geometry and format of one virtual channel to be previewed. */
typedef struct
{
  uint32_t vc;      /*!< virtual channel id                       */
  uint32_t dt;      /*!< CSI-2 data type, e.g. DCMIPP_DT_RAW10    */
  uint32_t width;   /*!< source width in pixels                   */
  uint32_t height;  /*!< source height in lines                   */
} csi_preview_source_t;

/** @brief Initialise the LTDC and clear the framebuffer. Call once. */
int csi_preview_lcd_init(void);

/** @brief Show a single virtual channel in the centre of the screen. */
int csi_preview_single(const csi_preview_source_t *src);

/**
  * @brief  Show two virtual channels side by side, left tile first.
  * @note   Both sources must have identical geometry and data type: one PIPE1
  *         configuration is shared and only the channel and the destination
  *         address are swapped per frame.
  */
int csi_preview_dual(const csi_preview_source_t *left, const csi_preview_source_t *right);

/** @brief Stop the capture and blank the screen. */
void csi_preview_stop(void);

/**
  * @brief  Choose which corner of the Bayer cell is red: 0 RGGB, 1 GRBG, 2 GBRG, 3 BGGR.
  * @retval 0 on success
  *
  * Nothing in a CSI-2 stream says this, so it cannot be probed - it has to be
  * decided by looking at the picture. The write takes effect on the next frame
  * without stopping the pipe, so the four can be compared on a live image.
  */
int csi_preview_set_bayer(uint32_t pattern);

/** @brief Per-frame hook; call from BSP_CAMERA_FrameEventCallback() for PIPE1. */
void csi_preview_on_pipe1_frame(void);

/** @brief Print frame counters per tile and the flow-selection registers. */
void csi_preview_print_stats(void);

/** @brief Current mode. */
csi_preview_mode_t csi_preview_get_mode(void);

#ifdef __cplusplus
}
#endif

#endif /* CSI_PREVIEW_H */
