#include "debug_control.h"

#include "stm32n6570_discovery.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "venc_app.h"
#include "dcmipp_app.h"
#include "venc_bench.h"
#include "tx_api.h"

#define DEBUG_CONTROL_LINE_SIZE 48U

extern UART_HandleTypeDef hcom_uart[];

static void print_status(void)
{
  VENC_APP_Status_t status;
  VENC_APP_GetStatus(&status);
  printf("CTRL: state=%s frames_rx=%lu frames_enc=%lu last=%lu\n",
         VENC_APP_PipelineStateName(status.state),
         (unsigned long)status.frame_received,
         (unsigned long)status.frame_encoded,
         (unsigned long)status.last_status);
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

/**
  * @brief  Collect @p frames encode samples and print the time model.
  *
  * The encoded output is dropped while this runs. A measurement that can stall
  * on the SD card measures the card, not the encoder. Whatever the setting was
  * is restored afterwards, so a 'bench' does not silently stop recording.
  */
static void run_bench(uint32_t frames)
{
  VENC_APP_Status_t status;
  bool     was_discarding = venc_bench_discard();
  bool     we_started_it  = false;
  uint32_t deadline_s;
  uint32_t waited_s = 0U;

  /* Ten frames a second is a floor rather than an estimate: it only decides
     when to give up on a capture that has gone quiet. */
  deadline_s = ((frames / 10U) * 2U) + 10U;

  venc_bench_set_discard(true);
  venc_bench_reset();

  VENC_APP_GetStatus(&status);
  if (status.state != VENC_APP_PIPELINE_RUNNING)
  {
    /* Start it from here rather than expecting a separate 'start' command.
       Staging the input frame in AXISRAM keeps the encode thread busy for most
       of every frame period, and the console polls the UART one byte at a time
       with no FIFO - a command sent into a running measurement arrives as
       nonsense. Everything the run needs is therefore sent while the board is
       still idle. */
    if (VENC_APP_EncodingStart() != TX_SUCCESS)
    {
      printf("CTRL: the pipeline would not start\n");
      venc_bench_set_discard(was_discarding);
      return;
    }

    while ((waited_s < 10U) && (status.state != VENC_APP_PIPELINE_RUNNING))
    {
      tx_thread_sleep(TX_TIMER_TICKS_PER_SECOND);
      waited_s++;
      VENC_APP_GetStatus(&status);
    }
    if (status.state != VENC_APP_PIPELINE_RUNNING)
    {
      printf("CTRL: the pipeline is %s, not running - no frames to measure\n",
             VENC_APP_PipelineStateName(status.state));
      venc_bench_set_discard(was_discarding);
      return;
    }
    waited_s = 0U;
    we_started_it = true;
    venc_bench_reset();
  }

  while ((venc_bench_frames() < frames) && (waited_s < deadline_s))
  {
    tx_thread_sleep(TX_TIMER_TICKS_PER_SECOND);
    waited_s++;
    printf(".");

    VENC_APP_GetStatus(&status);
    if (status.state != VENC_APP_PIPELINE_RUNNING)
    {
      printf("\nCTRL: the pipeline stopped during the measurement\n");
      break;
    }
  }
  printf("\n");

  if (venc_bench_frames() < frames)
  {
    printf("CTRL: only %lu of %lu frames arrived; reporting what there is\n",
           (unsigned long)venc_bench_frames(), (unsigned long)frames);
  }

  venc_bench_report(NULL);
  venc_bench_set_discard(was_discarding);

  if (we_started_it)
  {
    /* Symmetric with the start above, and not just tidiness: a run that stages
       its input in AXISRAM spends 146 ms of every frame period copying, and
       the console - one polled byte at a time, no FIFO - cannot be driven at
       all while that goes on. Left running, this board would take no further
       command. */
    (void)VENC_APP_EncodingStop();
    printf("CTRL: pipeline stopped again (it was 'bench' that started it)\n");
  }
}

/**
  * @brief  Record @p writes frames to the card and print the write time model.
  *
  * The mirror image of run_bench(): that one throws the output away so the card
  * cannot influence the encode time, this one keeps it so the card is exactly
  * what gets measured.
  */
static void run_sdbench(uint32_t writes)
{
  VENC_APP_Status_t status;
  bool     was_discarding = venc_bench_discard();
  bool     we_started_it  = false;
  uint32_t waited_s = 0U;
  uint32_t deadline_s = ((writes / 10U) * 2U) + 15U;

  venc_bench_set_discard(false);
  venc_bench_sd_reset();

  VENC_APP_GetStatus(&status);
  if (status.state != VENC_APP_PIPELINE_RUNNING)
  {
    if (VENC_APP_EncodingStart() != TX_SUCCESS)
    {
      printf("CTRL: the pipeline would not start\n");
      venc_bench_set_discard(was_discarding);
      return;
    }
    while ((waited_s < 10U) && (status.state != VENC_APP_PIPELINE_RUNNING))
    {
      tx_thread_sleep(TX_TIMER_TICKS_PER_SECOND);
      waited_s++;
      VENC_APP_GetStatus(&status);
    }
    if (status.state != VENC_APP_PIPELINE_RUNNING)
    {
      printf("CTRL: the pipeline is %s, not running - nothing to write\n",
             VENC_APP_PipelineStateName(status.state));
      venc_bench_set_discard(was_discarding);
      return;
    }
    waited_s = 0U;
    we_started_it = true;
    venc_bench_sd_reset();
  }

  while ((venc_bench_sd_writes() < writes) && (waited_s < deadline_s))
  {
    tx_thread_sleep(TX_TIMER_TICKS_PER_SECOND);
    waited_s++;
    printf(".");

    VENC_APP_GetStatus(&status);
    if (status.state != VENC_APP_PIPELINE_RUNNING)
    {
      printf("\nCTRL: the pipeline stopped during the measurement\n");
      break;
    }
  }
  printf("\n");

  if (venc_bench_sd_writes() < writes)
  {
    printf("CTRL: only %lu of %lu writes happened; reporting what there is\n",
           (unsigned long)venc_bench_sd_writes(), (unsigned long)writes);
  }

  venc_bench_sd_report();
  venc_bench_set_discard(was_discarding);

  if (we_started_it)
  {
    (void)VENC_APP_EncodingStop();
    printf("CTRL: pipeline stopped again (it was 'sdbench' that started it)\n");
  }
}

static void handle_command(char *line)
{
  if ((strcmp(line, "help") == 0) || (strcmp(line, "?") == 0))
  {
    printf("CTRL: commands\n"
           "  status              pipeline state and frame counters\n"
           "  start / stop        run or halt the capture+encode pipeline\n"
           "  phy <mbps>          CSI D-PHY bitrate, while stopped\n"
           "  cfg                 what the encoder is configured with\n"
           "  bench [n]           encode-time model over n frames (default 200);\n"
           "                      starts the pipeline if it is stopped and drops\n"
           "                      the encoded output for the duration\n"
           "  sdbench [n]         SD write time over n frames (default 200);\n"
           "                      records to the card, unlike 'bench'\n"
           "  record              keep encoded frames again\n"
           "  format yuyv|nv12    pixel packer and encoder input format\n"
           "  inbuf capture|axisram  where the encoder reads the picture from\n"
           "  cabac <0|1|2>       0 CAVLC, 1 CABAC, 2 CAVLC intra / CABAC inter\n"
           "  t8x8 <0|1|2>        8x8 transform: off, adaptive, always\n"
           "  bitrate <bit/s>     rate control target\n"
           "  the five configuration commands need the pipeline stopped\n");
  }
  else if (strcmp(line, "status") == 0)
  {
    print_status();
  }
  else if (strncmp(line, "phy ", 4) == 0)
  {
    VENC_APP_Status_t status;
    VENC_APP_GetStatus(&status);
    if (status.state != VENC_APP_PIPELINE_STOPPED)
    {
      printf("CTRL: 'stop' the pipeline before changing the CSI PHY bitrate\n");
    }
    else
    {
      uint32_t applied = dcmipp_set_csi_phy_bitrate((uint32_t)atoi(&line[4]));
      printf("CTRL: CSI PHY bitrate set to %lu Mbit/s\n", (unsigned long)applied);
    }
  }
  else if (strcmp(line, "start") == 0)
  {
    UINT status = VENC_APP_EncodingStart();
    printf("CTRL: start %s (%lu)\n", (status == TX_SUCCESS) ? "queued" : "failed", (unsigned long)status);
  }
  else if (strcmp(line, "stop") == 0)
  {
    UINT status = VENC_APP_EncodingStop();
    printf("CTRL: stop %s (%lu)\n", (status == TX_SUCCESS) ? "queued" : "failed", (unsigned long)status);
  }
  else if (strcmp(line, "cfg") == 0)
  {
    venc_bench_print_config();
  }
  else if (strncmp(line, "sdbench", 7) == 0)
  {
    run_sdbench(arg_u32(line, 0U, 200U));
  }
  else if (strncmp(line, "bench", 5) == 0)
  {
    run_bench(arg_u32(line, 0U, 200U));
  }
  else if (strcmp(line, "record") == 0)
  {
    venc_bench_set_discard(false);
    printf("CTRL: encoded frames are recorded again\n");
  }
  else if (strncmp(line, "format ", 7) == 0)
  {
    if (strstr(line, "nv12") != NULL)
    {
      if (venc_bench_set_nv12(true) == 0) { venc_bench_print_config(); }
    }
    else if (strstr(line, "yuyv") != NULL)
    {
      if (venc_bench_set_nv12(false) == 0) { venc_bench_print_config(); }
    }
    else
    {
      printf("CTRL: format takes yuyv or nv12\n");
    }
  }
  else if (strncmp(line, "inbuf ", 6) == 0)
  {
    if (strstr(line, "axisram") != NULL)
    {
      if (venc_bench_set_input_src(VENC_INPUT_FROM_AXISRAM) == 0) { venc_bench_print_config(); }
    }
    else if (strstr(line, "capture") != NULL)
    {
      if (venc_bench_set_input_src(VENC_INPUT_FROM_CAPTURE) == 0) { venc_bench_print_config(); }
    }
    else
    {
      printf("CTRL: inbuf takes capture or axisram\n");
    }
  }
  else if (strncmp(line, "cabac ", 6) == 0)
  {
    if (venc_bench_set_cabac(arg_u32(line, 0U, 1U)) == 0) { venc_bench_print_config(); }
  }
  else if (strncmp(line, "t8x8 ", 5) == 0)
  {
    if (venc_bench_set_transform8x8(arg_u32(line, 0U, 1U)) == 0) { venc_bench_print_config(); }
  }
  else if (strncmp(line, "bitrate ", 8) == 0)
  {
    if (venc_bench_set_bitrate(arg_u32(line, 0U, 2000000U)) == 0) { venc_bench_print_config(); }
  }
  else if (line[0] != '\0')
  {
    printf("CTRL: unknown command '%s'\n", line);
  }
}

void debug_control_thread_func(ULONG arg)
{
  uint8_t ch;
  char line[DEBUG_CONTROL_LINE_SIZE];
  uint32_t pos = 0U;

  (void)arg;
  printf("CTRL: ready on COM1; type 'help'\n");

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
      else if (pos < (DEBUG_CONTROL_LINE_SIZE - 1U))
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