#include "debug_control.h"

#include "stm32n6570_discovery.h"
#include "string.h"
#include "stdio.h"
#include "venc_app.h"

#define DEBUG_CONTROL_LINE_SIZE 32U

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

static void handle_command(char *line)
{
  if ((strcmp(line, "help") == 0) || (strcmp(line, "?") == 0))
  {
    printf("CTRL: commands: status, start, stop, help\n");
  }
  else if (strcmp(line, "status") == 0)
  {
    print_status();
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