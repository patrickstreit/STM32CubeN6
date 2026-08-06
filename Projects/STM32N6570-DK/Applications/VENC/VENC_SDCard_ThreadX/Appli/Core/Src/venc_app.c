/**
******************************************************************************
* @file    venc_app.c
* @author  MCD Application Team
* @brief   VENC USB application for STM32N6xx: handles video encoding and USB video streaming.
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
#include "ewl.h"
#include "h264encapi.h"
#include "venc_app.h"
#include "imx335.h"
#include "stm32n6xx_ll_venc.h"
#include "stm32n6570_discovery.h"
#include "perf.h"
#include "stm32n6570_discovery_camera.h"
#include "stm32n6570_discovery_lcd.h"
#include "tx_api.h"
#include "utils.h"
#include "venc_h264_config.h"
#include "dcmipp_app.h"

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
} venc_output_frame_t;
/* Private define ------------------------------------------------------------*/
/* Align and use unsigned suffixes for sizes/counts */
#define VENC_APP_QUEUE_SIZE        15U
#define VENC_OUTPUT_BLOCK_NBR      8U

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

/* Input Frame : in internal ram */
venc_output_frame_t enc_queue_buf[VENC_OUTPUT_BLOCK_NBR];

TX_EVENT_FLAGS_GROUP venc_app_flags;
TX_QUEUE enc_frame_queue;
TX_BLOCK_POOL venc_block_pool;

/* Private function prototypes -----------------------------------------------*/
static int encoder_prepare(void);
static int encode_frame(void);
static int encoder_end(void);
static int encoder_start(void);

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

    if (IsHwHandshakeMode())
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
  if(tx_queue_create(&enc_frame_queue, "ENC frame queue", sizeof(venc_output_frame_t)/4, &enc_queue_buf, sizeof(enc_queue_buf)) != TX_SUCCESS)
  {
    Error_Handler();
  }

  /* expose queue capacity to perf */
  perf_set_queue_capacity(VENC_APP_QUEUE_SIZE);

  /* init perf */
  perf_init();

  
  /* Get address and size reserved for h264 output bitstream */
  outputBuffer = GetOutputBuffer(&outputBufferSize);
  outputBlockSize = outputBufferSize / VENC_OUTPUT_BLOCK_NBR;
     
  if(tx_block_pool_create(&venc_block_pool, "venc output block pool", outputBlockSize,
                            outputBuffer, outputBufferSize) != TX_SUCCESS)
  {
    Error_Handler();
  }

  /* expose block-pool capacity to perf (number of blocks) */
  perf_set_blockpool_capacity(outputBufferSize / outputBlockSize);

   /* Initialize camera */
  if(BSP_CAMERA_Init(0,CAMERA_R2592x1944, CAMERA_PF_RAW_RGGB10) != BSP_ERROR_NONE)
  {
    Error_Handler();
  }

  IMX335_SetFramerate(Camera_CompObj, hVencH264Instance.cfgH264Main.frameRateNum /hVencH264Instance.cfgH264Main.frameRateDenom);
  
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
  
  /* STart lcd if any*/
  lcd_init();
  
  VENC_APP_EncodingStart();

  while(1)
  {
    tx_event_flags_get(&venc_app_flags, VIDEO_START_FLAG, TX_AND, &flags, TX_WAIT_FOREVER);
    if(BSP_CAMERA_BackgroundProcess() != BSP_ERROR_NONE)
    {
      printf("Error in BSP image processing\n");
    }

    tx_event_flags_get(&venc_app_flags, FRAME_RECEIVED_FLAG, TX_AND_CLEAR, &flags, TX_WAIT_FOREVER);
    if (IsVideoOverflow())
    {
      nbFrameSkip++;
      continue; 
    }
    if(encode_frame())
    {
      printf("error encoding frame\n");
    }
    else
    {
      BSP_LED_Toggle(LED_GREEN);
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
  H264EncRet ret;
  venc_output_frame_t frame_buffer = {0};

  if (dcmipp_config(GetInputFrame(NULL)))
  {
    return -1;
  }

  if(tx_block_allocate(&venc_block_pool, (void **) &frame_buffer.block_addr, TX_NO_WAIT) != TX_SUCCESS)
  {
    return -1;
  }
  perf_inc_blockpool();
  frame_buffer.aligned_block_addr = (uint32_t *) ALIGNED((uint32_t) frame_buffer.block_addr, 8);
  encIn.pOutBuf = frame_buffer.aligned_block_addr;
  encIn.busOutBuf = (uint32_t) encIn.pOutBuf;
  encIn.outBufSize = outputBlockSize;

  /* create stream */
  ret = H264EncStrmStart(encoder, &encIn, &encOut);
  if (ret != H264ENC_OK)
  {
    return -1;
  }
  frame_buffer.size = encOut.streamSize;
  
  tx_block_release(frame_buffer.block_addr);
  perf_dec_blockpool();

  encIn.codingType = H264ENC_INTRA_FRAME;
  return 0;
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
 * - On success, enqueues the encoded chunk for USB transmission.
 * - Handles desync via encoder_reset and error paths by releasing buffers.
 *
 * @return 0 on success, -1 on failure.
 */
static int encode_frame(void)
{
  venc_output_frame_t frame_buffer = {0};
  int ret = H264ENC_FRAME_READY;
  uint64_t t_start = perf_get_u64_cycles();
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

  /* allocate and set output buffer */
  if(tx_block_allocate(&venc_block_pool, (void **) &frame_buffer.block_addr, TX_WAIT_FOREVER) != TX_SUCCESS)
  {
    printf("VENC : failed to allocate output buffer\n");
    return -1;
  }
  perf_inc_blockpool();

  frame_buffer.aligned_block_addr = (uint32_t *) ALIGNED((uint32_t) frame_buffer.block_addr, 8);

  encIn.pOutBuf    = frame_buffer.aligned_block_addr;
  encIn.busOutBuf  = (uint32_t) encIn.pOutBuf;
  encIn.outBufSize = outputBlockSize - 8;
  

  /* Encode Frame*/
  uint64_t t_h264_0 = perf_get_u64_cycles();
  ret = H264EncStrmEncode(encoder, &encIn, &encOut, NULL, NULL, NULL);
  uint64_t t_h264_1 = perf_get_u64_cycles();
  perf_add_h264((uint32_t)perf_delta_us64(t_h264_0, t_h264_1));

  /* Measure encode time hook (user-defined) */
  timeMonitor();

  switch (ret)
  {
  case H264ENC_FRAME_READY:
    /*save stream */
    if(encOut.streamSize == 0)
    {
      encIn.codingType = H264ENC_INTRA_FRAME;
      tx_block_release(frame_buffer.block_addr);
      return -1;
    }
    frame_buffer.coding_type = (uint32_t)encIn.codingType;
    frame_buffer.size = encOut.streamSize;
    if(tx_queue_send(&enc_frame_queue, (void *) &frame_buffer, TX_NO_WAIT) != TX_SUCCESS)
    {
      tx_block_release(frame_buffer.block_addr);
      perf_dec_blockpool();
    }
    else
    {
      uint64_t t_qsend_end = perf_get_u64_cycles();
      /* approximate tx_queue_send as lightweight: record delta in us */
      perf_add_queue_send((uint32_t)perf_delta_us64(t_h264_1, t_qsend_end));
      perf_inc_queue();
    }
    encIn.codingType = H264ENC_PREDICTED_FRAME;
     nb_encoded_frame++;
    break;
  case H264ENC_FUSE_ERROR:
    printf("DCMIPP and VENC desync (frame#%ld), restart the video\n", frame_nb);
    tx_block_release(frame_buffer.block_addr);
    encoder_reset();
    break;
  default:
    printf("error encoding frame %d\n", ret);
    tx_block_release(frame_buffer.block_addr);
    encIn.codingType = H264ENC_INTRA_FRAME;
    return -1;
    break;
  }
  frame_nb++;
  uint64_t t_end = perf_get_u64_cycles();
  perf_add_encode((uint32_t)perf_delta_us64(t_start, t_end));
  return 0;
}


/**
 * @brief  End encoding session: stop camera, finalize encoder stream and
 *         release/flush resources.
 * @retval int 0 on success, -1 on failure
 */
static int encoder_end(void){
  
  printf("\x1b[31mStopping camera and encoder !!!\x1b[0m\n");
  
  BSP_CAMERA_Stop(0);
  int ret = H264EncStrmEnd(encoder, &encIn, &encOut);
  if (ret != H264ENC_OK)
  {
    return -1;
  }
  tx_queue_flush(&enc_frame_queue);
  tx_block_pool_delete(&venc_block_pool);
  return 0;
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
  tx_event_flags_set(&venc_app_flags, FRAME_RECEIVED_FLAG, TX_OR);

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
 * @brief Starts the video encoding process.
 *
 * This function initializes and starts the video encoding operation.
 */
void VENC_APP_EncodingStart(void)
{
  /* initialize encoder software for camera feed encoding */
  encoder_start();
  tx_event_flags_set(&venc_app_flags, VIDEO_START_FLAG, TX_OR);
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
  static uint32_t * curr_block = NULL;
  if(curr_block)
  {
    tx_block_release(curr_block);
    curr_block = NULL;
    perf_dec_blockpool();
  }
  venc_output_frame_t frame_block;
  uint64_t t_qrecv_0 = perf_get_u64_cycles();
  if(tx_queue_receive(&enc_frame_queue, (void *) &frame_block, TX_WAIT_FOREVER) != TX_SUCCESS)
  {
    *data = NULL;
    *size = 0;
    return(-1);
  }
  uint64_t t_qrecv_1 = perf_get_u64_cycles();
  perf_add_queue_recv((uint32_t)perf_delta_us64(t_qrecv_0, t_qrecv_1));
  perf_dec_queue();
  *data = (UCHAR *) frame_block.aligned_block_addr;
  *size = frame_block.size;
  curr_block = frame_block.block_addr;
  return(frame_block.coding_type);
}

/**
 * @brief Stops the video encoding process.
 *
 * @return  0 
 */
UINT VENC_APP_EncodingStop(void)
{
  ULONG flags;
  tx_event_flags_get(&venc_app_flags, VIDEO_START_FLAG, TX_AND_CLEAR, &flags, TX_WAIT_FOREVER);
  (void) encoder_end();
  return(0);
}
