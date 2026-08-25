/**
  ******************************************************************************
  * @file    csi_probe_app.c
  * @brief   Minimal application around csi_probe / csi_preview.
  *
  * Built only when CSI_PROBE_MODE is defined. It brings up the DCMIPP/CSI
  * receiver and nothing else - no encoder, no SD card, no FileX - so the CSI-2
  * link can be characterised and previewed on the display in isolation.
  *
  * It measures nothing on its own: every measurement costs a source power cycle,
  * so what runs is what was asked for over COM1. Type "help".
  ******************************************************************************
  */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csi_grab.h"
#include "csi_preview.h"
#include "csi_probe.h"
#include "main.h"
#include "stm32n6570_discovery.h"
#include "stm32n6570_discovery_camera.h"
#include "tx_api.h"

#define CONSOLE_LINE_SIZE 48U

extern UART_HandleTypeDef  hcom_uart[];
extern DCMIPP_HandleTypeDef hcamera_dcmipp;

static csi_probe_phy_t    g_phy;      /* what the receiver is set to right now */
static csi_probe_report_t g_report;   /* and what was measured, at its own setting */

/* ------------------------------------------------------------------------- */
/* BSP hooks                                                                 */
/* ------------------------------------------------------------------------- */

/**
  * @brief  Bring up the DCMIPP core only.
  * @note   Overrides the weak BSP implementation, which would program a CSI and
  *         pipe configuration for the IMX335. Here the CSI settings are the
  *         unknown that the probe is about to determine, so nothing beyond
  *         HAL_DCMIPP_Init() may be assumed.
  */
HAL_StatusTypeDef MX_DCMIPP_Init(DCMIPP_HandleTypeDef *hdcmipp)
{
  return HAL_DCMIPP_Init(hdcmipp);
}

void BSP_CAMERA_FrameEventCallback(uint32_t Instance)
{
  if (Instance == DCMIPP_PIPE1)
  {
    csi_preview_on_pipe1_frame();
  }
}

/* Counted, not printed. These run in the DCMIPP interrupt, and printf there
   blocks it for milliseconds per line at 115200 baud - which is long enough to
   lose frames and so to change the very thing an error report is meant to
   measure. 'status' reads the counters out. */
static volatile uint32_t g_pipe_errors;
static volatile uint32_t g_global_errors;
static volatile uint32_t g_last_error_code;

void BSP_CAMERA_PipeErrorCallback(uint32_t Instance)
{
  (void)Instance;
  g_pipe_errors++;
  g_last_error_code = hcamera_dcmipp.ErrorCode;
}

void BSP_CAMERA_ErrorCallback(uint32_t Instance)
{
  (void)Instance;
  g_global_errors++;
  g_last_error_code = hcamera_dcmipp.ErrorCode;
}

/* ------------------------------------------------------------------------- */
/* Preview selection                                                         */
/* ------------------------------------------------------------------------- */

/**
  * @brief  Make the report describe the setting the receiver holds now.
  * @note   Called before a partial measurement such as 'dt' or 'geom'. If the
  *         report belongs to a different setting its numbers are dropped rather
  *         than mixed with the new ones - that mixture is what made a stale run
  *         at 1250 look like a fresh result at every other bitrate.
  */
static void report_rebase(void)
{
  if (g_report.valid &&
      (g_report.phy.mbps == g_phy.mbps) &&
      (g_report.phy.lanes == g_phy.lanes) &&
      (g_report.phy.swapped == g_phy.swapped))
  {
    return;
  }

  csi_probe_report_invalidate(&g_report);
  g_report.phy   = g_phy;
  g_report.valid = true;   /* link stats stay absent: link_measured is false */
}

/** @brief Fill a preview source from what the probe measured for @p vc. */
static bool source_from_probe(uint32_t vc, csi_preview_source_t *src)
{
  const csi_probe_vc_info_t *info;

  if (!g_report.valid)
  {
    printf("CTRL: nothing measured yet; run 'probe' first\n");
    return false;
  }
  if (vc >= CSI_PROBE_VC_COUNT)
  {
    return false;
  }

  info = &g_report.vc[vc];

  if (!info->present)
  {
    printf("CTRL: VC%lu was not seen by the probe; run 'probe' first\n", (unsigned long)vc);
    return false;
  }
  if ((info->width == 0U) || (info->lines == 0U))
  {
    printf("CTRL: geometry of VC%lu is unknown, cannot configure the pipe\n", (unsigned long)vc);
    return false;
  }

  src->vc     = vc;
  src->dt     = info->image_dt;
  src->width  = info->width;
  src->height = info->lines;
  return true;
}

/* ------------------------------------------------------------------------- */
/* Console                                                                   */
/* ------------------------------------------------------------------------- */

static void print_help(void)
{
  printf("CTRL: commands\n"
         "  link [ms]          apply the current setting, get the source restarted\n"
         "                     and report when clock, lane sync and frames appear\n"
         "  refine [n]         compare the n bitrate profiles either side of the\n"
         "                     current one and keep the quietest (default 2)\n"
         "  source manual|auto who restarts the source. manual is the default: the\n"
         "                     probe prompts and waits for the clock. auto drives\n"
         "                     EN_CAM/NRST_CAM, which only reaches a module powered\n"
         "                     from the camera connector\n"
         "  power              drive EN_CAM/NRST_CAM once, whatever the mode\n"
         "  scan [ms] [slow]   sweep lanes/mapping/bitrate. 'slow' restarts the\n"
         "                     source per combination; otherwise it is restarted\n"
         "                     once up front\n"
         "  probe [mbps]       characterise; without an argument it tries the\n"
         "                     known-good setting and then sweeps\n"
         "  phy <mbps> [lanes] [swap]   apply one D-PHY setting without probing\n"
         "  vcs [ms]           ask every virtual channel in turn whether anything\n"
         "                     arrives on it, delimited or not (default 200 ms)\n"
         "  dt <vc>            identify the data types on one virtual channel\n"
         "  geom <vc>          measure lines and bytes per line of one channel\n"
         "  grab <vc> <dt> [n] dump the raw payload of one data type through the\n"
         "                     DCMIPP dump pipe and print the first n bytes\n"
         "  single <vc>        preview one channel, centred\n"
         "  bayer <0-3>        which corner of the Bayer cell is red, live:\n"
         "                     0 RGGB, 1 GRBG, 2 GBRG, 3 BGGR\n"
         "  dual <vcL> <vcR>   preview two channels side by side\n"
         "  off                stop the preview\n"
         "  errors [ms]        clear every CSI flag, then count what comes back,\n"
         "                     next to the frame count over the same window\n"
         "  status             CSI status registers, error counters and preview\n"
         "                     counters. The flags are sticky - use 'errors' for\n"
         "                     what is happening now\n"
         "  report             reprint the last probe result\n");
}

/** @brief Read the n-th whitespace separated argument as an unsigned number. */
static uint32_t arg_u32(const char *line, uint32_t index, uint32_t fallback)
{
  const char *p = line;

  /* Skip the command word, then index-1 further words. */
  for (uint32_t i = 0U; i <= index; i++)
  {
    while ((*p != '\0') && (*p != ' ')) { p++; }
    while (*p == ' ') { p++; }
  }

  if ((*p < '0') || (*p > '9'))
  {
    return fallback;
  }
  return (uint32_t)strtoul(p, NULL, 0);
}

static bool arg_has(const char *line, const char *word)
{
  return strstr(line, word) != NULL;
}

static void handle_command(char *line)
{
  if ((strcmp(line, "help") == 0) || (strcmp(line, "?") == 0))
  {
    print_help();
  }
  else if (strncmp(line, "link", 4) == 0)
  {
    csi_preview_stop();
    if (g_phy.mbps == 0U)
    {
      g_phy = csi_probe_default_phy;
    }
    csi_probe_report_invalidate(&g_report);
    (void)csi_probe_wait_for_link(&g_phy, arg_u32(line, 0U, 0U));
  }
  else if (strncmp(line, "refine", 6) == 0)
  {
    csi_preview_stop();
    if (g_phy.mbps == 0U)
    {
      g_phy = csi_probe_default_phy;
    }
    csi_probe_report_invalidate(&g_report);
    (void)csi_probe_refine(&g_phy, arg_u32(line, 0U, 2U), 400U);
  }
  else if (strncmp(line, "source", 6) == 0)
  {
    if (arg_has(line, "auto"))
    {
      csi_probe_set_source_mode(CSI_SOURCE_AUTO);
    }
    else if (arg_has(line, "manual"))
    {
      csi_probe_set_source_mode(CSI_SOURCE_MANUAL);
    }
    printf("CTRL: source mode is %s\n",
           (csi_probe_get_source_mode() == CSI_SOURCE_AUTO)
             ? "auto (EN_CAM/NRST_CAM)" : "manual (operator power-cycles)");
  }
  else if (strcmp(line, "power") == 0)
  {
    printf("CTRL: driving EN_CAM/NRST_CAM\n");
    csi_probe_source_restart(0U);
    printf("CTRL: done; this only reaches a module powered from the camera connector\n");
  }
  else if (strncmp(line, "scan", 4) == 0)
  {
    csi_preview_stop();
    /* The winner lands in g_phy so a following bare "probe" characterises it. */
    csi_probe_report_invalidate(&g_report);
    (void)csi_probe_scan(arg_u32(line, 0U, 150U), arg_has(line, "slow"), &g_phy);
  }
  else if (strncmp(line, "probe", 5) == 0)
  {
    csi_preview_stop();
    g_phy.mbps    = (uint16_t)arg_u32(line, 0U, 0U);
    g_phy.lanes   = (g_phy.lanes == 0U) ? 2U : g_phy.lanes;
    csi_probe_run(&g_phy, &g_report);
    csi_probe_print_report(&g_report, &g_phy);
  }
  else if (strncmp(line, "phy ", 4) == 0)
  {
    csi_preview_stop();
    csi_probe_report_invalidate(&g_report);
    g_phy.mbps    = (uint16_t)arg_u32(line, 0U, 1500U);
    g_phy.lanes   = (uint8_t)arg_u32(line, 1U, 2U);
    g_phy.swapped = arg_has(line, "swap") ? 1U : 0U;
    if (csi_probe_apply_phy(&g_phy) == HAL_OK)
    {
      printf("CTRL: D-PHY set to %u Mbit/s, %u lane(s), %s mapping\n",
             (unsigned)g_phy.mbps, (unsigned)g_phy.lanes,
             (g_phy.swapped != 0U) ? "inverted" : "physical");
      /* Applying a setting resets the D-PHY, so whatever the source was doing
         has just been lost. It has to come up again after the receiver. */
      printf("CTRL: the receiver was reset by this; run 'link' to restart the source\n");
    }
    else
    {
      printf("CTRL: applying the D-PHY setting failed\n");
    }
  }
  else if (strncmp(line, "bayer", 5) == 0)
  {
    (void)csi_preview_set_bayer(arg_u32(line, 0U, 0U));
  }
  else if (strncmp(line, "grab", 4) == 0)
  {
    /* PIPE0 and PIPE1 would be reading the same channel; stop the preview so the
       dump is the only consumer. */
    csi_preview_stop();
    csi_grab(arg_u32(line, 0U, 0U), arg_u32(line, 1U, 0x2BU), arg_u32(line, 2U, 64U));
  }
  else if (strncmp(line, "vcs", 3) == 0)
  {
    /* Stops and restarts every channel, so nothing may be capturing. */
    csi_preview_stop();
    csi_probe_vc_survey(arg_u32(line, 0U, 200U));
  }
  else if (strncmp(line, "dt", 2) == 0)
  {
    uint32_t vc = arg_u32(line, 0U, 0U);
    if (vc < CSI_PROBE_VC_COUNT)
    {
      report_rebase();
      csi_probe_datatypes(vc, 200U, &g_report.vc[vc]);
      g_report.vc[vc].present = (g_report.vc[vc].dt_mask_lo != 0U) ||
                                (g_report.vc[vc].dt_mask_hi != 0U);
      csi_probe_print_report(&g_report, &g_phy);
    }
  }
  else if (strncmp(line, "geom", 4) == 0)
  {
    uint32_t vc = arg_u32(line, 0U, 0U);
    if (vc < CSI_PROBE_VC_COUNT)
    {
      report_rebase();
      csi_probe_geometry(vc, &g_report.vc[vc]);
      csi_probe_print_report(&g_report, &g_phy);
    }
  }
  else if (strncmp(line, "single ", 7) == 0)
  {
    csi_preview_source_t src;
    if (source_from_probe(arg_u32(line, 0U, 0U), &src))
    {
      (void)csi_preview_single(&src);
    }
  }
  else if (strncmp(line, "dual ", 5) == 0)
  {
    csi_preview_source_t left;
    csi_preview_source_t right;
    if (source_from_probe(arg_u32(line, 0U, 0U), &left) &&
        source_from_probe(arg_u32(line, 1U, 1U), &right))
    {
      (void)csi_preview_dual(&left, &right);
    }
  }
  else if (strcmp(line, "off") == 0)
  {
    csi_preview_stop();
    printf("CTRL: preview stopped\n");
  }
  else if (strcmp(line, "status") == 0)
  {
    csi_probe_dump_status();
    printf("DCMIPP: %lu pipe error(s), %lu global error(s), last code 0x%08lx\n",
           (unsigned long)g_pipe_errors, (unsigned long)g_global_errors,
           (unsigned long)g_last_error_code);
    csi_preview_print_stats();
  }
  else if (strncmp(line, "errors", 6) == 0)
  {
    csi_probe_watch_errors(arg_u32(line, 0U, 500U));
  }
  else if (strcmp(line, "report") == 0)
  {
    csi_probe_print_report(&g_report, &g_phy);
  }
  else if (line[0] != '\0')
  {
    printf("CTRL: unknown command '%s'\n", line);
  }
}

/* ------------------------------------------------------------------------- */
/* Thread                                                                    */
/* ------------------------------------------------------------------------- */

void csi_probe_thread_func(ULONG arg)
{
  uint8_t  ch;
  char     line[CONSOLE_LINE_SIZE];
  uint32_t pos = 0U;

  (void)arg;

  /* No sensor is driven from the STM32: BSP_CAMERA_Init() only powers the
     camera connector, configures the DCMIPP clocks and calls the MX_DCMIPP_Init
     above. The CSI-2 stream is produced externally and is expected to be
     running already. */
  if (BSP_CAMERA_Init(0, 0, 0) != BSP_ERROR_NONE)
  {
    printf("CSI: BSP_CAMERA_Init failed\n");
    Error_Handler();
  }

  BSP_LED_On(LED1);

  printf("\nCSI-2 probe application\n");
  printf("Nothing is measured until you ask for it: every measurement needs the\n");
  printf("source power-cycled *after* the receiver is configured - that is the\n");
  printf("order a D-PHY transmitter needs to be picked up - and doing that at\n");
  printf("boot only burns a power cycle on a setting you may not want.\n");
  printf("'probe <mbps>' characterises one setting, 'probe' alone tries the\n");
  printf("known-good %u Mbit/s and then sweeps. You will be prompted for the\n",
         (unsigned)csi_probe_default_phy.mbps);
  printf("power cycle; 'source auto' instead drives EN_CAM/NRST_CAM, which only\n");
  printf("reaches a module powered from the camera connector.\n");

  /* The receiver is left unprogrammed: 0 means 'no setting applied yet', so the
     first command decides what the D-PHY gets, and no report can claim numbers
     that were never measured. */
  g_phy.mbps  = 0U;
  g_phy.lanes = 2U;
  csi_probe_report_invalidate(&g_report);

  BSP_LED_On(LED2);
  print_help();

  while (1)
  {
    if (HAL_UART_Receive(&hcom_uart[COM1], &ch, 1U, 50U) == HAL_OK)
    {
      if ((ch == '\r') || (ch == '\n'))
      {
        line[pos] = '\0';
        handle_command(line);
        pos = 0U;
      }
      else if ((ch == '\b') || (ch == 0x7FU))
      {
        if (pos > 0U)
        {
          pos--;
        }
      }
      else if (pos < (CONSOLE_LINE_SIZE - 1U))
      {
        line[pos++] = (char)ch;
      }
      else
      {
        pos = 0U;
        printf("CTRL: line too long\n");
      }
    }
    else
    {
      tx_thread_sleep(1U);
    }
  }
}
