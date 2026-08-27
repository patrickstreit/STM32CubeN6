/**
  ******************************************************************************
  * @file    venc_app.h
  * @author  MCD Application Team
  * @brief   Header for venc_app.c module
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2025 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef VENC_APP_H
#define VENC_APP_H

/* Includes ------------------------------------------------------------------*/
#include "stdio.h"
#include "stm32n6xx_hal.h"
#include "tx_api.h"
/* Exported types ------------------------------------------------------------*/
typedef enum
{
  VENC_APP_PIPELINE_STOPPED = 0,
  VENC_APP_PIPELINE_STARTING,
  VENC_APP_PIPELINE_RUNNING,
  VENC_APP_PIPELINE_STOPPING,
  VENC_APP_PIPELINE_ERROR,
} VENC_APP_PipelineState_t;

typedef struct
{
  VENC_APP_PipelineState_t state;
  uint32_t frame_received;
  uint32_t frame_encoded;
  UINT last_status;
} VENC_APP_Status_t;

/* Exported constants --------------------------------------------------------*/
/* Event flags used by the video pipeline */
#define FRAME_RECEIVED_FLAG        (1U << 0)
#define VIDEO_START_FLAG           (1U << 1)
#define VIDEO_START_REQUEST_FLAG   (1U << 2)
#define VIDEO_STOP_REQUEST_FLAG    (1U << 3)
#define VIDEO_STOPPED_FLAG         (1U << 4)

/* Exported variables --------------------------------------------------------*/
/**
 * @brief  Event flags group used for USB/video device signalling.
 * @note   Defined in the corresponding C module.
 */
extern TX_EVENT_FLAGS_GROUP USB_video_device_flags;

/**
 * @brief  Queue carrying encoded frames (TX_QUEUE).
 * @note   Defined in the corresponding C module.
 */
extern TX_QUEUE enc_frame_queue;

/* Exported functions --------------------------------------------------------*/

/**
 * @brief  Main VENC thread entry function.
 * @param  arg Thread argument (unused or user-defined)
 */
void venc_thread_func(ULONG arg);

/**
 * @brief  Release the encoder instance and build it again from the current
 *         configuration.
 * @retval int 0 on success, -1 when the pipeline is not stopped or the encoder
 *         refused the configuration.
 *
 * Geometry, coding tools and rate control are fixed at H264EncInit; trying a
 * different set means a new instance. Only legal while stopped.
 */
int VENC_APP_ReinitEncoder(void);

/**
 * @brief  Start the video encoding pipeline.
 * @retval UINT ThreadX-style status
 */
UINT VENC_APP_EncodingStart(void);

/**
 * @brief  Retrieve encoded data from the encoder.
 * @param  data  Output pointer to the data buffer (UCHAR **)
 * @param  size  Output pointer to the data size (ULONG *)
 * @retval UINT Status code (NX/ThreadX style, 0 for success)
 */
INT VENC_APP_GetData(UCHAR **data, ULONG *size);

/**
 * @brief  Correlation key (frame id) of the block returned by the last
 *         VENC_APP_GetData() call.
 * @retval uint32_t frame id
 */
uint32_t VENC_APP_GetFrameId(void);

/**
 * @brief  Stop the video encoding pipeline and release resources.
 * @retval UINT Status code
 */
UINT VENC_APP_EncodingStop(void);

/**
 * @brief  Get current pipeline state and counters.
 */
void VENC_APP_GetStatus(VENC_APP_Status_t *status);

/**
  * @brief  Run a sustained error tally and print it (PLAN.md M3).
  *
  * Records for @p seconds with the pipeline running, then reports what the
  * CSI-2 receiver, the DCMIPP and the encoder complained about in that window.
  * The question the plan asks is not whether the stray 0x12/0x2f packets and
  * the permanent IDERR ever appear - they do - but whether anything downstream
  * of them accumulates.
  *
  * @param  seconds Length of the observation window.
  */
void VENC_APP_WatchErrors(uint32_t seconds);

/**
 * @brief  Convert a pipeline state to a printable string.
 */
const char *VENC_APP_PipelineStateName(VENC_APP_PipelineState_t state);
#endif /* VENC_APP_H */
