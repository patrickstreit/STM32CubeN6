/**
******************************************************************************
* @file    sdcard_app.c
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
#include "stm32n6570_discovery.h"
#include "stdio.h"
#include "tx_api.h"
#include "utils.h"
#include "app_filex.h"
#include "h264encapi.h"
#include "perf.h"
#include <string.h>

/* Private typedef -----------------------------------------------------------*/
/* Private define ------------------------------------------------------------*/
/* Private macro -------------------------------------------------------------*/
#define NB_FRAMES_PER_FILE (30U*10U) /* 10sec  @ 30 fps*/
#define SD_WRITE_BUFFER_SIZE (64U * 1024U)
#define SD_WRITE_FLUSH_THRESHOLD (48U * 1024U)
#define SD_SECTOR_SIZE (512U)
/* Private variables ---------------------------------------------------------*/
ALIGN_32BYTES (static UCHAR sd_write_buffer[SD_WRITE_BUFFER_SIZE]);
/* Private function prototypes -----------------------------------------------*/
static UINT sdcard_write_chunk(UCHAR *buffer, ULONG size);
static UINT sdcard_flush_buffer(ULONG *buffered_size, UINT force_full_flush);

INT VENC_APP_GetData(UCHAR **data, ULONG *size);

static UINT sdcard_write_chunk(UCHAR *buffer, ULONG size)
{
  UINT status;
  uint64_t t0;
  uint64_t t1;

  if (size == 0U)
  {
    return FX_SUCCESS;
  }

  t0 = perf_get_u64_cycles();
  status = VENC_FileX_write((CHAR *)buffer, (LONG)size);
  t1 = perf_get_u64_cycles();
  perf_add_sd_write((uint32_t)perf_delta_us64(t0, t1));

  return status;
}

static UINT sdcard_flush_buffer(ULONG *buffered_size, UINT force_full_flush)
{
  UINT status;
  ULONG write_size;
  ULONG remain;
  ULONG buffered_before;

  if (*buffered_size == 0U)
  {
    return FX_SUCCESS;
  }

  buffered_before = *buffered_size;

  if (force_full_flush)
  {
    write_size = *buffered_size;
  }
  else
  {
    write_size = *buffered_size & ~(SD_SECTOR_SIZE - 1U); //
    if (write_size == 0U)
    {
      return FX_SUCCESS;
    }
  }

  status = sdcard_write_chunk(sd_write_buffer, write_size);
  if (status != FX_SUCCESS)
  {
    return status;
  }

  remain = *buffered_size - write_size;
  if (remain > 0U)
  {
    memmove(sd_write_buffer, &sd_write_buffer[write_size], remain);
  }

  *buffered_size = remain;
  perf_add_sd_buffer_flush(buffered_before, write_size, remain, force_full_flush != 0U);

  return FX_SUCCESS;
}


/**
* @brief  SDCard application thread function.
* @param  arg Thread argument (unused).
*
*/
void sdcard_thread_func(ULONG arg)
{
  CHAR filename[10];
  UINT filenumber = 0;
  UCHAR * data = NULL;
  ULONG size = 0;
  ULONG nb_frames = 0;
  ULONG record = 1;
  INT res;
  UINT open_new_file = 1;
  uint64_t total_bytes = 0;
  uint64_t file_start_cycles = 0;
  ULONG buffered_size = 0;
  
  H264EncPictureCodingType frame_type = H264ENC_NOTCODED_FRAME;
  
  if (VENC_FileX_Init() != FX_SUCCESS)
  {
    printf("FileX init failed\n");
    return;
  }
  perf_init();
  
  /* Wait for the first I frame*/
  do {
    res = VENC_APP_GetData(&data, &size);
  }
  while (res != H264ENC_INTRA_FRAME);
  /* Ensure the very first frame written is the I-frame we just received */
  frame_type = (H264EncPictureCodingType)res;
  
  while(record)
  {
    if (open_new_file)
    {
      /* If we are about to open a new file and this is not the first, report stats for the previous file */
      if (filenumber)
      {
        if (sdcard_flush_buffer(&buffered_size, 1U) != FX_SUCCESS)
        {
          printf("FileX failed to flush buffered data for %s\n", filename);
          return;
        }
        if  (VENC_FileX_close() !=  FX_SUCCESS)
        {
          printf("FileX failed to close %s\n", filename);
        }
        else
        {
          uint32_t elapsed_ms = (uint32_t)(perf_delta_us64(file_start_cycles, perf_get_u64_cycles()) / 1000ULL);
          perf_report_and_reset(nb_frames, total_bytes, elapsed_ms);
          total_bytes = 0;
        }
      }
      
      sprintf(filename, "%04d.h264", filenumber);
      
      printf("Open file %s\n", filename);
      if (VENC_FileX_Open(filename)  != FX_SUCCESS)
      {
        printf("FileX failed to open %s\n", filename);
        return;
      }
      filenumber++;
      nb_frames = 1;
      /* mark file start (monotone 64-bit cycle count) */
      file_start_cycles = perf_get_u64_cycles();
    }
    
    /* Write Frame to SDCard */
    if (data && size)
    {
      if (size > SD_WRITE_BUFFER_SIZE)
      {
        if (sdcard_flush_buffer(&buffered_size, 0U) != FX_SUCCESS)
        {
          printf("FileX failed to flush buffered data before large frame\n");
          return;
        }

        {
          ULONG direct_size = size & ~(SD_SECTOR_SIZE - 1U);
          ULONG tail_size = size - direct_size;

          if (direct_size > 0U)
          {
            perf_add_sd_direct_write(direct_size);
            if (sdcard_write_chunk(data, direct_size) != FX_SUCCESS)
            {
              printf("FileX failed to write large frame\n");
              return;
            }
          }

          if (tail_size > 0U)
          {
            if (tail_size > SD_WRITE_BUFFER_SIZE)
            {
              printf("FileX tail chunk too large\n");
              return;
            }

            memcpy(sd_write_buffer, &data[direct_size], tail_size);
            buffered_size = tail_size;
          }
        }
      }
      else
      {
        if ((buffered_size + size) > SD_WRITE_BUFFER_SIZE)
        {
          if (sdcard_flush_buffer(&buffered_size, 0U) != FX_SUCCESS)
          {
            printf("FileX failed to flush buffered data before append\n");
            return;
          }

          if ((buffered_size + size) > SD_WRITE_BUFFER_SIZE)
          {
            if (sdcard_flush_buffer(&buffered_size, 1U) != FX_SUCCESS)
            {
              printf("FileX failed to force flush buffered data before append\n");
              return;
            }
          }
        }

        memcpy(&sd_write_buffer[buffered_size], data, size);
        buffered_size += size;

        if (buffered_size >= SD_WRITE_FLUSH_THRESHOLD)
        {
          if (sdcard_flush_buffer(&buffered_size, 0U) != FX_SUCCESS)
          {
            printf("FileX failed to flush buffered data\n");
            return;
          }
        }
      }

      total_bytes += size;
      perf_note_written_frame();
      data = NULL; size = 0;
      BSP_LED_Toggle(LED_RED);
    }
    
    res = VENC_APP_GetData(&data, &size);
    if (res < 0)
    {
      printf("Failed to get encoded datas\n");
      tx_thread_sleep(15U*TX_TIMER_TICKS_PER_SECOND/1000U);
    }
    else
    { 
      frame_type = (H264EncPictureCodingType)res;
      nb_frames++;
    } 
    
    open_new_file = (nb_frames >= NB_FRAMES_PER_FILE && frame_type == H264ENC_INTRA_FRAME);
  }
  
  (void)sdcard_flush_buffer(&buffered_size, 1U);
  VENC_FileX_close();
}

