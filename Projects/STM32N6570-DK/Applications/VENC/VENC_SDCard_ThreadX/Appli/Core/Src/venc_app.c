/**
******************************************************************************
* @file    venc_app.c
* @author  MCD Application Team
* @brief   VENC SDCard application for STM32N6xx: handles video encoding and SD card recording.
******************************************************************************
* @attention
*
* Copyright (c) 2023 STMicroelectronics.
* All rights reserved.
*
* This software is licensed under terms that can be found in the LICENSE file
* in the root directory of this software component.
* If no LICENSE file comes with this software, it is provided AS-IS.
*
******************************************************************************
*/

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "stdio.h"
#include "string.h"
#include "ewl.h"
#include "h264encapi.h"
#include "venc_app.h"
#include "stm32n6xx_ll_venc.h"
#include "stm32n6570_discovery.h"
#include "stm32n6570_discovery_camera.h"
#include "stm32n6570_discovery_lcd.h"
#include "tx_api.h"
#include "utils.h"
#include "venc_h264_config.h"
#include "dcmipp_app.h"
#include "frame_rb.h"
#include "instrumentation.h"
#include "venc_bench.h"


/** @addtogroup Templates
* @{
*/

/** @addtogroup HAL
* @{
*/

/* Private typedef -----------------------------------------------------------*/
typedef struct {
  uint32_t coding_type;
  uint32_t size;
  uint32_t * block_addr;
  uint32_t * aligned_block_addr;
  uint32_t frame_id;
} venc_output_frame_t;
/* Private define ------------------------------------------------------------*/
/* Align and use unsigned suffixes for sizes/counts */
#define VENC_APP_QUEUE_SIZE        30U
#define VENC_OUTPUT_BLOCK_NBR      4U

/* Private macro -------------------------------------------------------------*/
/* Align a pointer up to 'bytes' boundary */
#define ALIGNED(ptr, bytes)       ((((uintptr_t)(ptr) + ((bytes) - 1U)) / (bytes)) * (bytes))

/* Private variables ---------------------------------------------------------*/


extern DCMIPP_HandleTypeDef hcamera_dcmipp;


static H264EncIn encIn= {0};
static H264EncOut encOut= {H264ENC_INTRA_FRAME,0, 0, 0, 0, 0, 0, 0,  H264ENC_NO_REFERENCE_NO_REFRESH,  H264ENC_NO_REFERENCE_NO_REFRESH};
static H264EncInst encoder= {0};
static uint32_t frame_nb = 0;
uint32_t frame_received = 0;
static uint32_t last_frame_received = 0;
static uint32_t nbLineEvent=0;
static uint32_t outputBlockSize; 
static uint32_t *g_curr_block = NULL;
static uint32_t g_max_output_buffer_size = 0U;
/* frame_id of the block handed out by the last VENC_APP_GetData() call */
static uint32_t g_curr_frame_id = 0U;
static volatile VENC_APP_PipelineState_t g_pipeline_state = VENC_APP_PIPELINE_STOPPED;

/* Error accounting for PLAN.md M3. The callbacks below already print, but a
   sustained run needs a tally rather than a scrolling console - the question is
   whether anything accumulates over minutes, not whether it ever happens. */
static volatile uint32_t g_pipe_errors;
static volatile uint32_t g_global_errors;
static volatile uint32_t g_last_error_code;
static volatile uint32_t g_encode_errors;
static volatile uint32_t g_fuse_errors;
static volatile uint32_t g_frames_skipped;
/* Kept apart from g_encode_errors on purpose: a full output ring means the SD
   card is behind, which is a throughput fact. Counting it as an encoder error
   would make a healthy encoder look broken. */
static volatile uint32_t g_ring_full;
static volatile UINT g_pipeline_last_status = TX_NOT_AVAILABLE;
static volatile uint32_t g_venc_events_ready = 0U;

/* Input Frame : in internal ram */
venc_output_frame_t enc_queue_buf[VENC_APP_QUEUE_SIZE];

TX_EVENT_FLAGS_GROUP venc_app_flags;
TX_QUEUE enc_frame_queue;

/* Private function prototypes -----------------------------------------------*/
static int encoder_prepare(void);
static int encode_frame(uint32_t frame_id);
static int encoder_end(void);
static int encoder_start(void);
static UINT pipeline_start_internal(void);
static UINT pipeline_stop_internal(void);

static void invalidate_dcache_region(const void *addr, uint32_t size)
{
  uintptr_t start;
  uintptr_t end;
  uint32_t aligned_size;

  if ((addr == NULL) || (size == 0U))
  {
    return;
  }

  start = ((uintptr_t)addr) & ~((uintptr_t)31U);
  end = ((uintptr_t)addr + (uintptr_t)size + 31U) & ~((uintptr_t)31U);
  aligned_size = (uint32_t)(end - start);
  SCB_InvalidateDCache_by_Addr((void *)start, (int32_t)aligned_size);
}

static void release_output_block(uint32_t *block_addr)
{
  (void)block_addr;
  frb_free(0xFFFFFFFFU);
}

/**
 * @brief  Dump and clear the CSI-2 host status registers (SR0/SR1).
 * @note   HAL_DCMIPP_Error/PipeErrorCallback only cover DCMIPP AXI/pipe-overrun
 *         errors; CSI-2 protocol/D-PHY errors (lane sync, CRC, ECC, watchdog)
 *         only show up here and are never routed through those callbacks.
 */
static void csi_dump_status(void)
{
  uint32_t sr0 = CSI->SR0;
  uint32_t sr1 = CSI->SR1;

  printf("CSI SR0=0x%08lx SR1=0x%08lx frames=%lu", (unsigned long)sr0, (unsigned long)sr1,
         (unsigned long)frame_received);
  if (sr0 & CSI_SR0_SOF0F)     printf(" SOF0");
  if (sr0 & CSI_SR0_EOF0F)     printf(" EOF0");
  if (sr0 & CSI_SR0_SYNCERRF)  printf(" SYNCERR");
  if (sr0 & CSI_SR0_CRCERRF)   printf(" CRCERR");
  if (sr0 & CSI_SR0_ECCERRF)   printf(" ECCERR(uncorrectable)");
  if (sr0 & CSI_SR0_CECCERRF)  printf(" CECCERR(corrected)");
  if (sr0 & CSI_SR0_IDERRF)    printf(" IDERR(wrong DT)");
  if (sr0 & CSI_SR0_WDERRF)    printf(" WDERR(no data/timeout)");
  if (sr0 & CSI_SR0_SPKTF)     printf(" SPKT");
  if (sr1 & CSI_SR1_ACTCLF)       printf(" ACT_CLK");
  if (sr1 & CSI_SR1_ACTDL0F)      printf(" ACT_L0");
  if (sr1 & CSI_SR1_ACTDL1F)      printf(" ACT_L1");
  if (sr1 & CSI_SR1_SYNCDL0F)     printf(" SYNC_L0");
  if (sr1 & CSI_SR1_SYNCDL1F)     printf(" SYNC_L1");
  if (sr1 & CSI_SR1_ESOTDL0F)     printf(" ESOT_L0(bitrate?)");
  if (sr1 & CSI_SR1_ESOTSYNCDL0F) printf(" ESOTSYNC_L0(bitrate?)");
  if (sr1 & CSI_SR1_ESOTDL1F)     printf(" ESOT_L1(bitrate?)");
  if (sr1 & CSI_SR1_ESOTSYNCDL1F) printf(" ESOTSYNC_L1(bitrate?)");
  if (sr0 == 0U && sr1 == 0U)  printf(" (nothing ever received on the CSI-2 lanes)");
  printf("\n");

  /* Flag-clear registers mirror the status bit positions */
  CSI->FCR0 = sr0;
  CSI->FCR1 = sr1;
}
/**
  * @brief  Checks if a video buffer overflow condition has occurred.
  * @note   This function is typically used to monitor the video streaming process
  *         and detect if the video buffer has exceeded its capacity, which may
  *         result in data loss or corruption.
  * @retval true  if a video overflow condition is detected.
  * @retval false if no overflow condition is present.
  *
  */
bool IsVideoOverflow(void)    
{
    bool videoOverflow = false;

    if (IsHwHanshakeMode())
    {
        videoOverflow = (nbLineEvent != 0U);
    }
    else
    {
        /* Ping-pong buffer overflow detection */
        videoOverflow = (frame_received > last_frame_received + GetNbInputFrame());
        last_frame_received = frame_received;
    }
    return videoOverflow;
}

__weak void lcd_init(void){return;}

/**
 * @brief  VENC application thread function.
 * @param  arg Thread argument (unused).
 *
 * Waits for VIDEO_START_FLAG; processes camera background tasks and, for each
 * FRAME_RECEIVED_FLAG, encodes the frame and queues the output for transmission.
 */
void venc_thread_func(ULONG arg)
{
  ULONG flags;
  uint8_t *  outputBuffer;
  uint32_t outputBufferSize;    
  uint32_t nbFrameSkip=0; 

  if(tx_event_flags_create(&venc_app_flags, "venc_app_events") != TX_SUCCESS)
  {
    return ;
  }
  g_venc_events_ready = 1U;
    
    if(tx_queue_create(&enc_frame_queue, "ENC frame queue", sizeof(venc_output_frame_t)/4, enc_queue_buf, sizeof(enc_queue_buf)) != TX_SUCCESS)
  {
    Error_Handler();
  }

  
  /* Get address and size reserved for h264 output bitstream */
  outputBuffer = GetOutputBuffer(&outputBufferSize);
  outputBlockSize = outputBufferSize / VENC_OUTPUT_BLOCK_NBR;
     
  if (frb_init(outputBuffer, outputBufferSize) == false)
  {
    Error_Handler();
  }

  /* Initialize the DCMIPP/CSI receiver only. No sensor is driven from the STM32:
     the MIPI CSI-2 stream (RAW10, VC0, 4 lanes, ~1485 Mbit/s/lane) is generated
     externally by a Lattice CrossLink and is assumed to already be running. */
  if(BSP_CAMERA_Init(0, 0, 0) != BSP_ERROR_NONE)
  {
    Error_Handler();
  }

  /* initialize VENC */
  LL_VENC_Init();

  /* initialization done. Turn on the LEDs */
  BSP_LED_On(LED1);
  BSP_LED_On(LED2);
  
  if (encoder_prepare())
  {
   printf("Encoder init failed\n");
   Error_Handler();
  }
  
  /* Start the LCD, if present. */
  lcd_init();

  printf("CTRL: pipeline stopped; use 'start' on COM1\n");
  tx_event_flags_set(&venc_app_flags, VIDEO_STOPPED_FLAG, TX_OR);
  
 
  while(1)
  {
    tx_event_flags_get(&venc_app_flags, VIDEO_START_REQUEST_FLAG, TX_AND_CLEAR, &flags, TX_WAIT_FOREVER);

    if (g_pipeline_state == VENC_APP_PIPELINE_RUNNING)
    {
      continue;
    }

    g_pipeline_state = VENC_APP_PIPELINE_STARTING;
    (void)tx_event_flags_get(&venc_app_flags, VIDEO_STOPPED_FLAG, TX_AND_CLEAR, &flags, TX_NO_WAIT);
    g_pipeline_last_status = pipeline_start_internal();
    if (g_pipeline_last_status != TX_SUCCESS)
    {
      g_pipeline_state = VENC_APP_PIPELINE_ERROR;
      tx_event_flags_set(&venc_app_flags, VIDEO_STOPPED_FLAG, TX_OR);
      printf("CTRL: pipeline start failed (%lu)\n", (unsigned long)g_pipeline_last_status);
      continue;
    }

    g_pipeline_state = VENC_APP_PIPELINE_RUNNING;
    tx_event_flags_set(&venc_app_flags, VIDEO_START_FLAG, TX_OR);
    printf("CTRL: pipeline running\n");

    while (g_pipeline_state == VENC_APP_PIPELINE_RUNNING)
    {
      UINT wait_ret = tx_event_flags_get(&venc_app_flags, FRAME_RECEIVED_FLAG | VIDEO_STOP_REQUEST_FLAG, TX_OR_CLEAR,
                                          &flags, 1U * TX_TIMER_TICKS_PER_SECOND);
      if (wait_ret != TX_SUCCESS)
      {
        /* No frame in 1s: dump CSI-2 status to see whether anything is arriving at all. */
        csi_dump_status();
        continue;
      }

      if ((flags & VIDEO_STOP_REQUEST_FLAG) != 0U)
      {
        g_pipeline_state = VENC_APP_PIPELINE_STOPPING;
        g_pipeline_last_status = pipeline_stop_internal();
        g_pipeline_state = (g_pipeline_last_status == TX_SUCCESS) ? VENC_APP_PIPELINE_STOPPED : VENC_APP_PIPELINE_ERROR;
        tx_event_flags_set(&venc_app_flags, VIDEO_STOPPED_FLAG, TX_OR);
        printf("CTRL: pipeline %s\n", (g_pipeline_last_status == TX_SUCCESS) ? "stopped" : "stop failed");
        break;
      }

      if ((flags & FRAME_RECEIVED_FLAG) != 0U)
      {
        /* Reuse the existing capture counter as the pipeline-wide correlation key. */
        uint32_t frame_id = frame_received;

        if (IsVideoOverflow())
        {
          nbFrameSkip++;
          g_frames_skipped++;
          INSTR_EVENT(INSTR_ID_FRAME_DROPPED, frame_id, nbFrameSkip, 1U /* CAPTURE_OVERFLOW */, 0U);
          continue; 
        }
        if(encode_frame(frame_id))
        {
          g_encode_errors++;
          printf("error encoding frame\n");
        }
        else
        {
          BSP_LED_Toggle(LED_GREEN);
        }
      }
    }
  }
}

/**
 * @brief  Prepare encoder: initialize encoder instance and configure
 *         preprocessing, coding control and rate control parameters.
 * @retval int 0 on success, -1 on failure
 */
static int encoder_prepare(void)
{
  H264EncRet ret;

    frame_nb = 0U;

  /* Set encode configuration */
  ret = H264EncInit(&hVencH264Instance.cfgH264Main, &encoder);

  if (ret != H264ENC_OK)
  {
    return -1;
  }

  /* Set preprocessing*/
  ret = H264EncSetPreProcessing(encoder, &hVencH264Instance.cfgH264Preproc);
  if(ret != H264ENC_OK)
  {
    return -1;
  }

  ret = H264EncSetCodingCtrl(encoder, &hVencH264Instance.cfgH264Coding);
  if(ret != H264ENC_OK)
  {
    return -1;
  }

    /* Set rate control */
  ret = H264EncSetRateCtrl(encoder, &hVencH264Instance.cfgH264Rate);
  if(ret != H264ENC_OK)
  {
    return -1;
  }

    return 0;
}

/**
 * @brief Resets the encoder to its initial state.
 *
 * This function performs all necessary operations to bring the encoder hardware
 * and associated software state back to a known, default condition. It should be
 * called whenever a full reinitialization of the encoder is required, such as after
 * an error or before starting a new encoding session.
 *
 * @note This function is static and intended for internal use within this module only.
 */
static void  encoder_reset(void)
{
  if (HAL_DCMIPP_PIPE_Suspend(&hcamera_dcmipp, DCMIPP_PIPE1) != HAL_OK)
  {
    printf("HAL_DCMIPP_PIPE_Suspend failed\n");
  }

  /* Wait for end of frame */
  tx_thread_sleep(15U * TX_TIMER_TICKS_PER_SECOND / 1000U);


  /* VENC HW Reset */
  __HAL_RCC_VENC_FORCE_RESET();
  tx_thread_sleep(1U * TX_TIMER_TICKS_PER_SECOND / 1000U);
  __HAL_RCC_VENC_RELEASE_RESET();
  tx_thread_sleep(1U * TX_TIMER_TICKS_PER_SECOND / 1000U);

  /*Resume DCMIPP after VENC reset*/
  if (HAL_DCMIPP_PIPE_Resume(&hcamera_dcmipp, DCMIPP_PIPE1) != HAL_OK)
  {
    printf("HAL_DCMIPP_PIPE_Resume failed");
  }
}


/**
 * @brief Starts the video encoder.
 *
 * This function initializes and starts the video encoder process.
 * It prepares all necessary resources and configurations required
 * for encoding video streams.
 *
 * @return int Returns 0 on success, or a negative error code on failure.
 */
static int encoder_start(void)
{
    H264EncRet ret = H264ENC_OK;
  venc_output_frame_t frame_buffer = {0};
    uint32_t buff_size = g_max_output_buffer_size ? g_max_output_buffer_size : outputBlockSize;
    uint32_t outBufSize;

  if (dcmipp_config(GetInputFrame(NULL)))
  {
    return -1;
  }

  frame_buffer.block_addr = (uint32_t *)frb_alloc(&buff_size);
  if (frame_buffer.block_addr == NULL)
  {
    return -1;
  }
  frame_buffer.aligned_block_addr = (uint32_t *) ALIGNED((uint32_t) frame_buffer.block_addr, 8);
  outBufSize = (buff_size > 8U) ? (buff_size - 8U) : 0U;
  encIn.pOutBuf = frame_buffer.aligned_block_addr;
  encIn.busOutBuf = (uint32_t) encIn.pOutBuf;
  encIn.outBufSize = outBufSize;

  /* create stream */
  ret = H264EncStrmStart(encoder, &encIn, &encOut);
  if (ret != H264ENC_OK)
  {
    release_output_block(frame_buffer.block_addr);
    BSP_CAMERA_Stop(0);
    return -1;
  }
  frame_buffer.size = encOut.streamSize;
  
  release_output_block(frame_buffer.block_addr);

  encIn.codingType = H264ENC_INTRA_FRAME;
  return ret;
}

/* Debug and instrumentation functions*/
__weak void mark_frame(void * frame) {}
__weak void timeMonitor(void) {};

uint32_t nb_encoded_frame = 0;

/**
 * @brief Encode a single captured frame and queue the resulting H.264 bitstream.
 *
 * - Selects coding type (intra every 30 frames, otherwise predicted).
 * - Maps DCMIPP capture buffer addresses to encoder input (Y/UV or Y/U/V).
 * - Allocates an output block from the ThreadX pool and runs H264EncStrmEncode.
 * - On success, enqueues the encoded chunk for SD card recording.
 * - Handles desync via encoder_reset and error paths by releasing buffers.
 *
 * @return 0 on success, -1 on failure.
 */
static int encode_frame(uint32_t frame_id)
{
  venc_output_frame_t frame_buffer = {0};
  int ret = H264ENC_FRAME_READY;
    uint32_t outBufSize;
  uint32_t buff_size = g_max_output_buffer_size ? g_max_output_buffer_size : outputBlockSize;
  uint32_t encode_start;
  uint32_t encode_cycles = 0U;
  uint32_t copy_cycles   = 0U;
  bool     was_intra;

  frame_buffer.frame_id = frame_id;

  if (!(frame_nb % hVencH264Instance.cfgH264Rate.gopLen))
  {
    /* if frame is the first : set as intra coded */
    encIn.codingType = H264ENC_INTRA_FRAME;
  }
  else
  {
    /* if there was a frame previously, set as predicted */
    encIn.timeIncrement = 1;
    encIn.codingType = H264ENC_PREDICTED_FRAME;
  }
  encIn.ipf = H264ENC_REFERENCE_AND_REFRESH;
  encIn.ltrf = H264ENC_REFERENCE;
  /* Read before the encode: the coding type is advanced on the way out, and
     the time model keeps intra and inter apart. */
  was_intra = (encIn.codingType == H264ENC_INTRA_FRAME);
  

   DCMIPP_FullPlanarDstAddressTypeDef  planar_address;
   DCMIPP_SemiPlanarDstAddressTypeDef  semi_planar_address;


/* set input buffers to structures */  
  dcmipp_get_address((void*)GetNextFrame(nb_encoded_frame), &planar_address, &semi_planar_address);

  if (dcmipp_is_semiplanar())
  {
  encIn.busLuma = semi_planar_address.YAddress;
  encIn.busChromaU = semi_planar_address.UVAddress;
  encIn.busChromaV = semi_planar_address.UVAddress;        
  }
  else
  {
  encIn.busLuma = planar_address.YAddress;
  encIn.busChromaU = planar_address.UAddress;
  encIn.busChromaV = planar_address.VAddress;   
  }
  
  /* Water mark for debug*/
  mark_frame((void*)encIn.busLuma);

  frame_buffer.block_addr = (uint32_t *)frb_alloc(&buff_size);
  if (frame_buffer.block_addr == NULL)
  {
    g_ring_full++;
    printf("VENC : failed to allocate output buffer\n");
    return -1;
  }
  outBufSize = (buff_size > 8U) ? (buff_size - 8U) : 0U;
  frame_buffer.aligned_block_addr = (uint32_t *) ALIGNED((uint32_t) frame_buffer.block_addr, 8);

  encIn.pOutBuf    = frame_buffer.aligned_block_addr;
  encIn.busOutBuf  = (uint32_t) encIn.pOutBuf;
  encIn.outBufSize = outBufSize;
  

  /* The encoder can be pointed at a copy of the frame in AXISRAM instead of the
     capture buffer in PSRAM. The copy is deliberately outside the timed region:
     what is under test is the memory the encoder reads from, not the cost of
     putting the frame there. (PLAN.md M2, variant b.) */
  if (venc_bench_input_src() == VENC_INPUT_FROM_AXISRAM)
  {
    uint32_t stage_size = 0U;
    uint8_t *stage      = venc_bench_stage_buffer(&stage_size);
    uint32_t luma_bytes = (uint32_t)hVencH264Instance.cfgH264Main.width *
                          GetDCMIPPNbLinesCaptured();
    uint32_t frame_bytes = luma_bytes + (luma_bytes / 2U);   /* NV12 */

    if ((stage != NULL) && (frame_bytes <= stage_size))
    {
      uint32_t t0 = venc_bench_now();
      /* Luma and chroma are contiguous in the capture buffer, so one copy does
         both planes. */
      (void)memcpy(stage, (const void *)encIn.busLuma, frame_bytes);
      copy_cycles = venc_bench_now() - t0;

      encIn.busLuma    = (uint32_t)stage;
      encIn.busChromaU = (uint32_t)stage + luma_bytes;
      encIn.busChromaV = encIn.busChromaU;
    }
  }

  /* Encode Frame*/
  INSTR_EVENT(INSTR_ID_VENC_SUBMITTED, frame_id, (uint32_t)encIn.codingType,
              frb_get_frames_stored(), buff_size);
  encode_start = venc_bench_now();
  ret = H264EncStrmEncode(encoder, &encIn, &encOut, NULL, NULL, NULL);
  encode_cycles = venc_bench_now() - encode_start;

  /* Measure encode time*/
  timeMonitor();

  switch (ret)
  {
  case H264ENC_FRAME_READY:
    /*save stream */
    if(encOut.streamSize == 0)
    {
      encIn.codingType = H264ENC_INTRA_FRAME;
      release_output_block(frame_buffer.block_addr);
      INSTR_EVENT(INSTR_ID_VENC_ERROR, frame_id, (uint32_t)ret, 0U, 0U);
      return -1;
    }
    venc_bench_sample(was_intra, encode_cycles, encOut.streamSize, copy_cycles);

    if (venc_bench_discard())
    {
      /* A measurement must not be able to stall on the recording path: with the
         output dropped here, a slow card can no longer back the ring buffer up
         and turn "how long does the encoder take" into "how long did it wait
         for a block". */
      release_output_block(frame_buffer.block_addr);
      encIn.codingType = H264ENC_PREDICTED_FRAME;
      nb_encoded_frame++;
      frame_nb++;
      return 0;
    }

    invalidate_dcache_region((const void *)encIn.pOutBuf, encOut.streamSize);
    frame_buffer.coding_type = (uint32_t)encIn.codingType;
    frame_buffer.size = encOut.streamSize;
    if (frame_buffer.size > g_max_output_buffer_size)
    {
      g_max_output_buffer_size = frame_buffer.size;
    }
    if (frb_push(frame_buffer.block_addr, frame_buffer.size, 0U) == false)
    {
      g_ring_full++;
      release_output_block(frame_buffer.block_addr);
      INSTR_EVENT(INSTR_ID_VENC_ERROR, frame_id, (uint32_t)-1, frame_buffer.size, 0U);
      return -1;
    }
    /* Also marks the hand-off to the SD writer; tx_queue_send() follows immediately. */
    INSTR_EVENT(INSTR_ID_VENC_DONE, frame_id, frame_buffer.size,
                frb_get_frames_stored(), frame_buffer.coding_type);
    if(tx_queue_send(&enc_frame_queue, (void *) &frame_buffer, TX_WAIT_FOREVER) != TX_SUCCESS)
    {
      return -1;
    }
    encIn.codingType = H264ENC_PREDICTED_FRAME;
     nb_encoded_frame++;
    break;
  case H264ENC_FUSE_ERROR:
    g_fuse_errors++;
    printf("DCMIPP and VENC desync (frame#%ld), restart the video\n", frame_nb);
    release_output_block(frame_buffer.block_addr);
    INSTR_EVENT(INSTR_ID_VENC_ERROR, frame_id, (uint32_t)ret, 0U, 0U);
    encoder_reset();
    break;
  default:
    printf("error encoding frame %d\n", ret);
    release_output_block(frame_buffer.block_addr);
    INSTR_EVENT(INSTR_ID_VENC_ERROR, frame_id, (uint32_t)ret, 0U, 0U);
    encIn.codingType = H264ENC_INTRA_FRAME;
    return -1;
    break;
  }
  frame_nb++;
  return 0;
}


/**
 * @brief  End encoding session: stop camera, finalize encoder stream and
 *         release/flush resources.
 * @retval int 0 on success, -1 on failure
 */
static int encoder_end(void){
  
  printf("\x1b[31mStopping camera and encoder !!!\x1b[0m\n");
  
  if (BSP_CAMERA_Stop(0) != BSP_ERROR_NONE)
  {
    /* HAL_DCMIPP_CSI_PIPE_Stop() waits for the virtual channel to go inactive
       and gives up when it does not. It then leaves PipeState at BUSY, and from
       there every HAL_DCMIPP_PIPE_SetConfig() on pipe 1 returns HAL_ERROR - so
       one rough stop would make every later start fail. Put the channel and the
       state back by hand. */
    SET_BIT(CSI->CR, CSI_CR_VC0STOP);
    hcamera_dcmipp.PipeState[DCMIPP_PIPE1] = HAL_DCMIPP_PIPE_STATE_READY;
    printf("camera stop timed out, pipe state restored by hand\n");
  }
  int ret = H264EncStrmEnd(encoder, &encIn, &encOut);
  if (ret != H264ENC_OK)
  {
    return -1;
  }
  tx_queue_flush(&enc_frame_queue);
  frb_reset();
  g_curr_block = NULL;
  return 0;
}

static UINT pipeline_start_internal(void)
{
  ULONG flags;

  (void)tx_event_flags_get(&venc_app_flags,
                           FRAME_RECEIVED_FLAG | VIDEO_STOP_REQUEST_FLAG | VIDEO_START_FLAG,
                           TX_OR_CLEAR, &flags, TX_NO_WAIT);
  frb_reset();
  tx_queue_flush(&enc_frame_queue);
  g_curr_block = NULL;
  g_max_output_buffer_size = 0U;
  frame_nb = 0U;
  frame_received = 0U;
  last_frame_received = 0U;
  nbLineEvent = 0U;
  nb_encoded_frame = 0U;

  if (encoder_start())
  {
    return TX_NOT_DONE;
  }
  return TX_SUCCESS;
}

static UINT pipeline_stop_internal(void)
{
  UINT ret = TX_SUCCESS;
  ULONG flags;

  if (encoder_end())
  {
    ret = TX_NOT_DONE;
  }
  (void)tx_event_flags_get(&venc_app_flags, FRAME_RECEIVED_FLAG | VIDEO_START_FLAG, TX_OR_CLEAR, &flags, TX_NO_WAIT);
  frb_reset();
  tx_queue_flush(&enc_frame_queue);
  g_curr_block = NULL;
  return ret;
}


/**
 * @brief  Callback when a full camera frame is captured.
 * @param  instance Camera instance index.
 *
 * Resets line event counter, increments received frame count, signals the
 * encoder thread that a frame is ready, and programs DCMIPP for the next
 * frame buffer address to sustain continuous capture.
 */
void BSP_CAMERA_FrameEventCallback(uint32_t instance)
{
  if (instance == DCMIPP_PIPE1)
  {
  /* signal new frame */
  nbLineEvent = 0;
  frame_received++;
  INSTR_EVENT(INSTR_ID_FRAME_CAPTURED, frame_received,
              frame_received - last_frame_received, nb_encoded_frame, 0U);
  if (TX_SUCCESS != tx_event_flags_set(&venc_app_flags, FRAME_RECEIVED_FLAG, TX_OR))
  {
    Error_Handler();
  }

  /* Signal DCMIPP for next frame address */
  dcmipp_set_memory_address(GetNextFrame(frame_received));
  }
  else if (instance == DCMIPP_PIPE2)
  {
    BSP_LCD_Reload(0, BSP_LCD_RELOAD_VERTICAL_BLANKING);
  }
}


/**
 * @brief  Callback function called when a camera line event occurs.
 * @param  instance Camera instance that triggered the event.
 * @retval None
 *
 * This function is typically called by the BSP (Board Support Package) layer
 * when a line event is detected by the camera hardware. The user can implement
 * this callback to handle line-based processing or synchronization.
 */
void BSP_CAMERA_LineEventCallback(uint32_t instance)
{
  /* signal new frame*/
 nbLineEvent++;
}

/**
 * @brief  Print the human-readable HAL_DCMIPP_(CSI_)ERROR_* bits of an ErrorCode.
 */
static void print_dcmipp_error_code(uint32_t err)
{
  if (err & HAL_DCMIPP_ERROR_AXI_TRANSFER)  printf(" AXI_TRANSFER");
  if (err & HAL_DCMIPP_ERROR_PARALLEL_SYNC) printf(" PARALLEL_SYNC");
  if (err & HAL_DCMIPP_ERROR_PIPE0_LIMIT)   printf(" PIPE0_LIMIT");
  if (err & HAL_DCMIPP_ERROR_PIPE0_OVR)     printf(" PIPE0_OVR");
  if (err & HAL_DCMIPP_ERROR_PIPE1_OVR)     printf(" PIPE1_OVR");
  if (err & HAL_DCMIPP_ERROR_PIPE2_OVR)     printf(" PIPE2_OVR");
  if (err & HAL_DCMIPP_CSI_ERROR_SYNC)         printf(" CSI_SYNC");
  if (err & HAL_DCMIPP_CSI_ERROR_WDG)          printf(" CSI_WDG(no data)");
  if (err & HAL_DCMIPP_CSI_ERROR_SPKT)         printf(" CSI_SPKT");
  if (err & HAL_DCMIPP_CSI_ERROR_DATA_ID)      printf(" CSI_DATA_ID(wrong DT)");
  if (err & HAL_DCMIPP_CSI_ERROR_CECC)         printf(" CSI_CECC(corrected)");
  if (err & HAL_DCMIPP_CSI_ERROR_ECC)          printf(" CSI_ECC(uncorrectable)");
  if (err & HAL_DCMIPP_CSI_ERROR_CRC)          printf(" CSI_CRC");
  if (err & HAL_DCMIPP_CSI_ERROR_DPHY_CTRL)    printf(" DPHY_CTRL(illegal ctrl code)");
  if (err & HAL_DCMIPP_CSI_ERROR_DPHY_LP_SYNC) printf(" DPHY_LP_SYNC");
  if (err & HAL_DCMIPP_CSI_ERROR_DPHY_ESCAPE)  printf(" DPHY_ESCAPE");
  if (err & HAL_DCMIPP_CSI_ERROR_SOT_SYNC)     printf(" DPHY_SOT_SYNC(bitrate/skew?)");
  if (err & HAL_DCMIPP_CSI_ERROR_SOT)          printf(" DPHY_SOT(bitrate/skew?)");
}

/**
 * @brief  Called on DCMIPP pipe error (overrun, limit, sync loss, ...).
 * @param  Instance DCMIPP pipe index.
 */
void BSP_CAMERA_PipeErrorCallback(uint32_t Instance)
{
  g_pipe_errors++;
  g_last_error_code = hcamera_dcmipp.ErrorCode;
  printf("DCMIPP PIPE%lu error, ErrorCode=0x%08lx", (unsigned long)Instance, (unsigned long)hcamera_dcmipp.ErrorCode);
  print_dcmipp_error_code(hcamera_dcmipp.ErrorCode);
  printf("\n");
}

/**
 * @brief  Called on global DCMIPP/CSI error (AXI transfer, parallel sync, ...).
 * @param  Instance Camera instance (always 0, single DCMIPP instance).
 */
void BSP_CAMERA_ErrorCallback(uint32_t Instance)
{
  g_global_errors++;
  g_last_error_code = hcamera_dcmipp.ErrorCode;
  printf("DCMIPP global error, ErrorCode=0x%08lx", (unsigned long)hcamera_dcmipp.ErrorCode);
  print_dcmipp_error_code(hcamera_dcmipp.ErrorCode);
  printf("\n");
}

/**
 * @brief Starts the video encoding process.
 *
 * This function initializes and starts the video encoding operation.
 */
UINT VENC_APP_EncodingStart(void)
{
  if (g_venc_events_ready == 0U)
  {
    return TX_NOT_AVAILABLE;
  }
  if ((g_pipeline_state == VENC_APP_PIPELINE_RUNNING) || (g_pipeline_state == VENC_APP_PIPELINE_STARTING))
  {
    return TX_SUCCESS;
  }
  if (tx_event_flags_set(&venc_app_flags, VIDEO_START_REQUEST_FLAG, TX_OR) != TX_SUCCESS)
  {
    return TX_NOT_DONE;
  }
  return TX_SUCCESS;
}

/**
 * @brief Retrieves encoded video data for transmission or processing.
 *
 * This function provides access to the next available chunk of encoded video data.
 * The function sets the pointer to the data buffer and its size.
 *
 * @param[out] data Pointer to a variable that will receive the address of the data buffer.
 * @param[out] size Pointer to a variable that will receive the size (in bytes) of the data buffer.
 *
 * @return UINT Status code of the operation (e.g., 0 for success, error code otherwise).
 */
INT VENC_APP_GetData(UCHAR **data, ULONG *size)
{
  UCHAR *frb_data;
  uint32_t frb_size;
  uint32_t timeStamp = 0U;
  ULONG wait_option = (g_pipeline_state == VENC_APP_PIPELINE_RUNNING) ?
                      (100U * TX_TIMER_TICKS_PER_SECOND / 1000U) : TX_NO_WAIT;
  if(g_curr_block)
  {
    frb_return_frame();
    g_curr_block = NULL;
  }
  venc_output_frame_t frame_block;
  if(tx_queue_receive(&enc_frame_queue, (void *) &frame_block, wait_option) != TX_SUCCESS)
  {
    *data = NULL;
    *size = 0;
    return(-1);
  }
  frb_size = 0U;
  frb_data = (UCHAR *)frb_pull(&frb_size, &timeStamp);
  if ((frb_data == NULL) || (frb_data != (UCHAR *)frame_block.block_addr) || (frb_size != frame_block.size))
  {
    *data = NULL;
    *size = 0;
    return -1;
  }

  *data = (UCHAR *) frame_block.aligned_block_addr;
  *size = frame_block.size;
  g_curr_block = frame_block.block_addr;
  g_curr_frame_id = frame_block.frame_id;
  return(frame_block.coding_type);
}

/**
 * @brief Correlation key of the block returned by the last VENC_APP_GetData().
 */
uint32_t VENC_APP_GetFrameId(void)
{
  return g_curr_frame_id;
}

/**
 * @brief Stops the video encoding process.
 *
 * @return  0 
 */
UINT VENC_APP_EncodingStop(void)
{
  if (g_venc_events_ready == 0U)
  {
    return TX_NOT_AVAILABLE;
  }
  if ((g_pipeline_state == VENC_APP_PIPELINE_STOPPED) || (g_pipeline_state == VENC_APP_PIPELINE_ERROR))
  {
    return TX_SUCCESS;
  }
  if (tx_event_flags_set(&venc_app_flags, VIDEO_STOP_REQUEST_FLAG, TX_OR) != TX_SUCCESS)
  {
    return TX_NOT_DONE;
  }
  return TX_SUCCESS;
}

/* CSI_SR0 error bits that mean "the link is not decoding cleanly", and the
   CSI_SR1 bits that mean "the D-PHY is not locked at this bitrate". Same masks
   csi_probe.c uses - the probe build is where they were worked out. */
#define CSI_SR0_LINK_ERRORS  (CSI_SR0_CRCERRF | CSI_SR0_ECCERRF | CSI_SR0_SYNCERRF | \
                              CSI_SR0_SPKTERRF | CSI_SR0_WDERRF)

#define CSI_SR1_PHY_ERRORS   (CSI_SR1_ESOTDL0F | CSI_SR1_ESOTSYNCDL0F | CSI_SR1_EESCDL0F | \
                              CSI_SR1_ESYNCESCDL0F | CSI_SR1_ECTRLDL0F | \
                              CSI_SR1_ESOTDL1F | CSI_SR1_ESOTSYNCDL1F | CSI_SR1_EESCDL1F | \
                              CSI_SR1_ESYNCESCDL1F | CSI_SR1_ECTRLDL1F)

void VENC_APP_WatchErrors(uint32_t seconds)
{
  uint32_t id_err = 0U, ecc = 0U, ecc_corr = 0U, crc = 0U;
  uint32_t sync = 0U, spkt = 0U, wdg = 0U, phy = 0U, samples = 0U;
  uint32_t frames_start, frames_end, encoded_start, encoded_end;
  uint32_t tickstart;

  if (seconds == 0U) { seconds = 60U; }

  g_pipe_errors     = 0U;
  g_global_errors   = 0U;
  g_last_error_code = 0U;
  g_encode_errors   = 0U;
  g_fuse_errors     = 0U;
  g_frames_skipped  = 0U;
  g_ring_full       = 0U;

  /* Clear the sticky link-error flags once, then sample. The frame flags
     (SOF/EOF) are deliberately left alone: the running pipe's interrupt handler
     owns those, and clearing them here would steal frames from it. */
  CSI->FCR0 = CSI_SR0_LINK_ERRORS | CSI_SR0_IDERRF | CSI_SR0_CECCERRF;
  CSI->FCR1 = CSI_SR1_PHY_ERRORS;

  frames_start  = frame_received;
  encoded_start = nb_encoded_frame;
  tickstart     = HAL_GetTick();

  while ((HAL_GetTick() - tickstart) < (seconds * 1000U))
  {
    uint32_t sr0 = CSI->SR0;
    uint32_t sr1 = CSI->SR1;

    if ((sr0 & CSI_SR0_IDERRF)     != 0U) { id_err++; }
    if ((sr0 & CSI_SR0_ECCERRF)    != 0U) { ecc++; }
    if ((sr0 & CSI_SR0_CECCERRF)   != 0U) { ecc_corr++; }
    if ((sr0 & CSI_SR0_CRCERRF)    != 0U) { crc++; }
    if ((sr0 & CSI_SR0_SYNCERRF)   != 0U) { sync++; }
    if ((sr0 & CSI_SR0_SPKTERRF)   != 0U) { spkt++; }
    if ((sr0 & CSI_SR0_WDERRF)     != 0U) { wdg++; }
    if ((sr1 & CSI_SR1_PHY_ERRORS) != 0U) { phy++; }
    samples++;

    CSI->FCR0 = sr0 & (CSI_SR0_LINK_ERRORS | CSI_SR0_IDERRF | CSI_SR0_CECCERRF);
    CSI->FCR1 = sr1 & CSI_SR1_PHY_ERRORS;

    /* Yield: this runs on the console thread and must not starve the encoder
       or the SD writer, which are what the measurement is about. */
    tx_thread_sleep(1U);
  }

  frames_end  = frame_received;
  encoded_end = nb_encoded_frame;

  printf("=== M3 RESULT ===\n");
  printf("window_s=%lu\n", (unsigned long)seconds);
  printf("samples=%lu\n", (unsigned long)samples);
  printf("csi.id_err=%lu\n", (unsigned long)id_err);
  printf("csi.ecc_uncorrectable=%lu\n", (unsigned long)ecc);
  printf("csi.ecc_corrected=%lu\n", (unsigned long)ecc_corr);
  printf("csi.crc=%lu\n", (unsigned long)crc);
  printf("csi.sync=%lu\n", (unsigned long)sync);
  printf("csi.spkt=%lu\n", (unsigned long)spkt);
  printf("csi.watchdog=%lu\n", (unsigned long)wdg);
  printf("csi.phy=%lu\n", (unsigned long)phy);
  printf("dcmipp.pipe_errors=%lu\n", (unsigned long)g_pipe_errors);
  printf("dcmipp.global_errors=%lu\n", (unsigned long)g_global_errors);
  printf("dcmipp.last_error_code=0x%08lx\n", (unsigned long)g_last_error_code);
  printf("frames_received=%lu\n", (unsigned long)(frames_end - frames_start));
  printf("frames_encoded=%lu\n", (unsigned long)(encoded_end - encoded_start));
  printf("frames_skipped=%lu\n", (unsigned long)g_frames_skipped);
  printf("ring_full=%lu\n", (unsigned long)g_ring_full);
  printf("encode_errors=%lu\n", (unsigned long)g_encode_errors);
  printf("fuse_errors=%lu\n", (unsigned long)g_fuse_errors);
  printf("sd_writes=%lu\n", (unsigned long)venc_bench_sd_writes());
  printf("=== END ===\n");
}

int VENC_APP_ReinitEncoder(void)
{
  if (g_pipeline_state != VENC_APP_PIPELINE_STOPPED)
  {
    return -1;
  }

  /* The encoder holds the picture geometry, the coding tools and the rate
     control from H264EncInit onwards, and several of them cannot be changed on
     a live instance. Releasing and building it again is the only way to try a
     different set - which is what the M2 measurements do between runs. */
  if (encoder != NULL)
  {
    (void)H264EncRelease(encoder);
    encoder = NULL;
  }

  return encoder_prepare();
}

void VENC_APP_GetStatus(VENC_APP_Status_t *status)
{
  if (status == NULL)
  {
    return;
  }
  status->state = g_pipeline_state;
  status->frame_received = frame_received;
  status->frame_encoded = nb_encoded_frame;
  status->last_status = g_pipeline_last_status;
}

const char *VENC_APP_PipelineStateName(VENC_APP_PipelineState_t state)
{
  switch (state)
  {
    case VENC_APP_PIPELINE_STOPPED: return "stopped";
    case VENC_APP_PIPELINE_STARTING: return "starting";
    case VENC_APP_PIPELINE_RUNNING: return "running";
    case VENC_APP_PIPELINE_STOPPING: return "stopping";
    case VENC_APP_PIPELINE_ERROR: return "error";
    default: return "unknown";
  }
}
