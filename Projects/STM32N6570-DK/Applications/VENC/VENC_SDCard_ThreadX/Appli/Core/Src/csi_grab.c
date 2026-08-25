/**
  ******************************************************************************
  * @file    csi_grab.c
  * @brief   Raw payload dump of one CSI-2 data type - see csi_grab.h.
  ******************************************************************************
  */

#include "csi_grab.h"

#include <stdio.h>
#include <string.h>

#include "csi_probe.h"
#include "stm32n6570_discovery.h"
#include "stm32n6570_discovery_camera.h"
#include "utils.h"

extern DCMIPP_HandleTypeDef hcamera_dcmipp;

/* One 1920-pixel RAW10 line is 2400 bytes, so this holds around 27 of them -
   enough to see structure without reserving a frame's worth of PSRAM. */
#define GRAB_BUF_SIZE   (64U * 1024U)

/* Neither 0x00 nor 0xFF: both are plausible payload, and a buffer that still
   reads as the fill pattern afterwards is the clearest possible "nothing was
   written here". */
#define GRAB_FILL       0xA5U

#define GRAB_TIMEOUT_MS 500U

static uint8_t grab_buf[GRAB_BUF_SIZE] ALIGN_32 IN_PSRAM;

/** @brief Invalidate the cache over a buffer the DCMIPP wrote behind the CPU. */
static void grab_invalidate(const void *addr, uint32_t size)
{
  uintptr_t start = ((uintptr_t)addr) & ~((uintptr_t)31U);
  uintptr_t end   = ((uintptr_t)addr + (uintptr_t)size + 31U) & ~((uintptr_t)31U);

  SCB_InvalidateDCache_by_Addr((void *)start, (int32_t)(end - start));
}

/** @brief Configure PIPE0 - the dump pipe - to accept only @p dt on @p vc. */
static HAL_StatusTypeDef grab_configure(uint32_t vc, uint32_t dt)
{
  DCMIPP_CSI_PIPE_ConfTypeDef csi_pipe = {0};
  DCMIPP_PipeConfTypeDef      pipe     = {0};

  /* The channel accepts everything and the pipe does the selecting. That is the
     point of grabbing through PIPE0: the virtual channel filter and the pipe's
     DTIDA comparison are separate hardware, so a data type the first one cannot
     distinguish may still be distinguished by the second.

     BPP8 keeps the unpacking byte-faithful, which is what a hexdump needs -
     and what makes two data types comparable, since the format that belongs to
     a given data type would differ between them. */
  if (HAL_DCMIPP_CSI_SetVCConfig(&hcamera_dcmipp, vc, DCMIPP_CSI_DT_BPP8) != HAL_OK)
  {
    return HAL_ERROR;
  }

  csi_pipe.DataTypeMode = DCMIPP_DTMODE_DTIDA;
  csi_pipe.DataTypeIDA  = dt;
  csi_pipe.DataTypeIDB  = dt;
  if (HAL_DCMIPP_CSI_PIPE_SetConfig(&hcamera_dcmipp, DCMIPP_PIPE0, &csi_pipe) != HAL_OK)
  {
    return HAL_ERROR;
  }

  /* Same defence as in the preview: that call writes CMCR.INSEL only while the
     handle is in INIT or READY, and otherwise does nothing and reports success. */
  if (READ_BIT(hcamera_dcmipp.Instance->CMCR, DCMIPP_CMCR_INSEL) != DCMIPP_SERIAL_MODE)
  {
    MODIFY_REG(hcamera_dcmipp.Instance->P0FSCR,
               DCMIPP_P0FSCR_DTMODE | DCMIPP_P0FSCR_DTIDA | DCMIPP_P0FSCR_DTIDB,
               DCMIPP_DTMODE_DTIDA | (dt << DCMIPP_P0FSCR_DTIDA_Pos) |
               (dt << DCMIPP_P0FSCR_DTIDB_Pos));
    CLEAR_BIT(hcamera_dcmipp.Instance->PRCR, DCMIPP_PRCR_ENABLE);
    SET_BIT(hcamera_dcmipp.Instance->CMCR, DCMIPP_CMCR_INSEL);
  }

  /* PIPE0 has no pixel packer and no pitch - it writes what arrives, in order.
     HAL_DCMIPP_PIPE_SetConfig() skips both fields for this pipe. */
  pipe.FrameRate = DCMIPP_FRAME_RATE_ALL;
  return HAL_DCMIPP_PIPE_SetConfig(&hcamera_dcmipp, DCMIPP_PIPE0, &pipe);
}

/** @brief Print @p count bytes of @p buf as 16 per line with an ASCII column. */
static void grab_hexdump(const uint8_t *buf, uint32_t count)
{
  for (uint32_t off = 0U; off < count; off += 16U)
  {
    uint32_t n = ((count - off) < 16U) ? (count - off) : 16U;

    printf("  %04lx ", (unsigned long)off);
    for (uint32_t i = 0U; i < 16U; i++)
    {
      if (i < n) { printf(" %02x", (unsigned)buf[off + i]); }
      else       { printf("   "); }
    }
    printf("  ");
    for (uint32_t i = 0U; i < n; i++)
    {
      uint8_t c = buf[off + i];
      printf("%c", ((c >= 0x20U) && (c < 0x7FU)) ? (char)c : '.');
    }
    printf("\n");
  }
}

void csi_grab(uint32_t vc, uint32_t dt, uint32_t show_bytes)
{
  uint32_t counter   = 0U;
  uint32_t tickstart;
  bool     frame_done = false;

  if (vc >= CSI_PROBE_VC_COUNT)
  {
    printf("GRAB: VC%lu does not exist\n", (unsigned long)vc);
    return;
  }
  if (show_bytes > GRAB_BUF_SIZE) { show_bytes = GRAB_BUF_SIZE; }
  if (show_bytes == 0U)           { show_bytes = 64U; }

  memset(grab_buf, GRAB_FILL, sizeof(grab_buf));
  SCB_CleanDCache_by_Addr((void *)grab_buf, (int32_t)sizeof(grab_buf));

  if (grab_configure(vc, dt) != HAL_OK)
  {
    printf("GRAB: could not configure PIPE0 for 0x%02lx\n", (unsigned long)dt);
    return;
  }

  /* The dump limit is what makes this safe against an unknown source: the pipe
     stops at that many 32-bit words per frame no matter how much is sent. Set
     the register directly rather than through HAL_DCMIPP_PIPE_EnableLimitEvent(),
     which also unmasks the limit interrupt - and there is no handler for it. */
  WRITE_REG(hcamera_dcmipp.Instance->P0DCLMTR,
            ((GRAB_BUF_SIZE / 4U) << DCMIPP_P0DCLMTR_LIMIT_Pos) | DCMIPP_P0DCLMTR_ENABLE);

  WRITE_REG(hcamera_dcmipp.Instance->CMFCR, DCMIPP_CMFCR_CP0FRAMEF);

  if (HAL_DCMIPP_CSI_PIPE_Start(&hcamera_dcmipp, DCMIPP_PIPE0, vc,
                                (uint32_t)grab_buf, DCMIPP_MODE_SNAPSHOT) != HAL_OK)
  {
    printf("GRAB: PIPE0 refused to start on VC%lu (CMCR=0x%08lx, pipe state %d)\n",
           (unsigned long)vc, (unsigned long)hcamera_dcmipp.Instance->CMCR,
           (int)hcamera_dcmipp.PipeState[DCMIPP_PIPE0]);
    return;
  }

  tickstart = HAL_GetTick();
  while ((HAL_GetTick() - tickstart) < GRAB_TIMEOUT_MS)
  {
    if ((hcamera_dcmipp.Instance->CMSR2 & DCMIPP_CMSR2_P0FRAMEF) != 0U)
    {
      frame_done = true;
      break;
    }
  }

  counter = READ_REG(hcamera_dcmipp.Instance->P0DCCNTR) & DCMIPP_P0DCCNTR_CNT;

  (void)HAL_DCMIPP_CSI_PIPE_Stop(&hcamera_dcmipp, DCMIPP_PIPE0, vc);
  CLEAR_BIT(hcamera_dcmipp.Instance->P0DCLMTR, DCMIPP_P0DCLMTR_ENABLE);

  grab_invalidate(grab_buf, sizeof(grab_buf));

  printf("GRAB: VC%lu 0x%02lx (%s): %lu byte(s) dumped, frame %s\n",
         (unsigned long)vc, (unsigned long)dt, csi_probe_dt_name(dt),
         (unsigned long)counter, frame_done ? "complete" : "did not complete");

  if (counter == 0U)
  {
    printf("      nothing arrived under this data type. The pipe's own filter\n"
           "      rejected every packet, so no packet on the wire carries it\n");
    return;
  }

  if (show_bytes > counter) { show_bytes = counter; }
  grab_hexdump(grab_buf, show_bytes);
}
