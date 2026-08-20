/**
******************************************************************************
* @file    lcd_app.c
* @author  MCD Application Team
* @brief   VENC SDCard application for STM32N6xx: handles video encoding and SD card recording.
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

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "venc_h264_config.h"
#include "stm32n6570_discovery.h"
#include "stm32n6570_discovery_camera.h"
#include "stm32n6570_discovery_lcd.h"
#include "dcmipp_app.h"
#include "stdio.h"
#include "tx_api.h"
#include "utils.h"



/* Private typedef -----------------------------------------------------------*/
/* Private define ------------------------------------------------------------*/
/* Private macro -------------------------------------------------------------*/
#define LCD_BPP   2U /*RGB565 = 2 biytes / pixels*/
#define LCD_FRAME_SIZE (LCD_DEFAULT_WIDTH*LCD_DEFAULT_HEIGHT*LCD_BPP)
/* Private variables ---------------------------------------------------------*/

uint8_t lcd_frame[LCD_FRAME_SIZE] ALIGN_32 IN_PSRAM;

/* Private function prototypes -----------------------------------------------*/
extern DCMIPP_HandleTypeDef hcamera_dcmipp;
/* CSI-2 input from external Lattice CrossLink: RAW10, VC0, 1920x1080 */
extern cam_h264_cfg_t hCamH264Instance;

HAL_StatusTypeDef  dcmipp_downsize(DCMIPP_HandleTypeDef *hdcmipp,int32_t pipe, int32_t camWidth,int32_t camHeight,int32_t captureWidth,int32_t captureHeight);


/**
* @brief  SDCard application thread function.
* @param  arg Thread argument (unused).
*
*/
void lcd_init(void)
{
  DCMIPP_PipeConfTypeDef pPipeConf = {0};
  
  if (HAL_DCMIPP_PIPE_CSI_EnableShare(&hcamera_dcmipp, DCMIPP_PIPE2) != HAL_OK)
  {
    printf("Failed to configure Pipe2\n");
    Error_Handler();
  }
      
  pPipeConf.FrameRate         = DCMIPP_FRAME_RATE_ALL;  /* Sensor framerate is set with IMX335_SetFramerate(..FRAMERATE);*/
  pPipeConf.PixelPackerFormat = DCMIPP_PIXEL_PACKER_FORMAT_RGB565_1;
  pPipeConf.PixelPipePitch    = LCD_DEFAULT_WIDTH * LCD_BPP; /* Number of bytes */
  
  /* Configure Pipe */
  if (HAL_DCMIPP_PIPE_SetConfig(&hcamera_dcmipp, DCMIPP_PIPE2, &pPipeConf) != HAL_OK)
  {
    printf("Failed to configure Pipe2\n");
    Error_Handler();
  }
  
  if (dcmipp_downsize(&hcamera_dcmipp,
                      DCMIPP_PIPE2,
                      hCamH264Instance.width, hCamH264Instance.height,
                      LCD_DEFAULT_WIDTH,LCD_DEFAULT_HEIGHT))
  {
    printf("Failed to configure Pipe2 size\n");
    Error_Handler();
  }
  
  
  if (HAL_DCMIPP_PIPE_SetMemoryAddress(&hcamera_dcmipp, DCMIPP_PIPE2, DCMIPP_MEMORY_ADDRESS_0, (uint32_t)lcd_frame))
  {
    printf("Failed to configure Pipe2 address\n");
    Error_Handler();
  }

  if (HAL_DCMIPP_CSI_PIPE_Start(&hcamera_dcmipp, DCMIPP_PIPE2, DCMIPP_VIRTUAL_CHANNEL1, (uint32_t)lcd_frame, CAMERA_MODE_CONTINUOUS))
  {
    printf("Failed to start Pipe2 DCMIPP\n");
    Error_Handler();
  }
  
  if( BSP_LCD_InitEx(0, LCD_ORIENTATION_LANDSCAPE, LCD_PIXEL_FORMAT_RGB565, LCD_DEFAULT_WIDTH, LCD_DEFAULT_HEIGHT))
  {
    printf("Failed to configure LCD\n");
    Error_Handler();
  }
  
  BSP_LCD_SetLayerAddress(0, 0,(uint32_t)lcd_frame);
}

