/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
#include <string.h>
#include "venc_buffers.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

UART_HandleTypeDef hlpuart1;

/* USER CODE BEGIN PV */
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
static void MX_GPIO_Init(void);
static void MX_LPUART1_UART_Init(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
static void mem_print(const char *label, uint32_t addr)
{
  char buf[64];
  volatile uint32_t val = *(volatile uint32_t *)addr;
  int len = snprintf(buf, sizeof(buf), "%s [0x%08X] = 0x%08X\r\n", label, (unsigned)addr, (unsigned)val);
  HAL_UART_Transmit(&hlpuart1, (uint8_t *)buf, (uint16_t)len, 100);
}
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_LPUART1_UART_Init();
  /* USER CODE BEGIN 2 */
  
  // a) Power the RAM up by writing 0 to SRAMSD in RAMCFG_AXISRAMxCR
  // b) Read back the value from the RAMCFG (alternatively, wait 40 ns)
  // c) Enable the RAM clock through the RCC. (RCC_MEMENR)
  uint32_t readback;
  // AXISRAM3
  RAMCFG_SRAM3_AXI_NS->CR &= ~RAMCFG_CR_SRAMSD;
  readback = RAMCFG_SRAM3_AXI_NS->CR;
  __HAL_RCC_AXISRAM3_MEM_CLK_ENABLE();
  // AXISRAM4
  RAMCFG_SRAM4_AXI_NS->CR &= ~RAMCFG_CR_SRAMSD;
  readback = RAMCFG_SRAM4_AXI_NS->CR;
  __HAL_RCC_AXISRAM4_MEM_CLK_ENABLE();
  // AXISRAM5
  RAMCFG_SRAM5_AXI_NS->CR &= ~RAMCFG_CR_SRAMSD;
  readback = RAMCFG_SRAM5_AXI_NS->CR;
  __HAL_RCC_AXISRAM5_MEM_CLK_ENABLE();
  // AXISRAM6
  RAMCFG_SRAM6_AXI_NS->CR &= ~RAMCFG_CR_SRAMSD;
  readback = RAMCFG_SRAM6_AXI_NS->CR;
  __HAL_RCC_AXISRAM6_MEM_CLK_ENABLE();

  /* --- AXISRAM: first word of each section --- */
  mem_print("AXISRAM1", 0x34000000UL);
  mem_print("AXISRAM2", 0x34100000UL);
  mem_print("AXISRAM3", 0x34200000UL);
  mem_print("AXISRAM4", 0x34270000UL);
  mem_print("AXISRAM5", 0x342E0000UL);
  mem_print("AXISRAM6", 0x34350000UL);

  /* --- XSPI2 Flash: FSBL header, FSBL vector table, Appli header, Appli vector table --- */
  mem_print("FSBL hdr [0]",  0x70000000UL);
  mem_print("FSBL code[0]",  0x70000400UL);
  mem_print("Appli hdr [0]", 0x70100000UL);
  mem_print("Appli code[0]", 0x70100400UL);

  /* --- PSRAM Ping-Pong Buffer Performance Test (XSPI1 @ 0x90000000) --- */
  {
    /* Enable DWT cycle counter for timing measurements */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;

    typedef struct { const char *name; uint8_t *buf; uint32_t size; } BufDesc;
    const BufDesc bufs[] = {
      { "frame_ping", frame_ping, VENC_FRAME_BUF_SIZE     },
      { "frame_pong", frame_pong, VENC_FRAME_BUF_SIZE     },
      { "bs_ping",    bs_ping,    VENC_BITSTREAM_BUF_SIZE },
      { "bs_pong",    bs_pong,    VENC_BITSTREAM_BUF_SIZE },
    };

    char ubuf[96];
    uint32_t clk = SystemCoreClock;

    for (unsigned i = 0; i < 4U; i++)
    {
      uint8_t  *p  = bufs[i].buf;
      uint32_t  sz = bufs[i].size;

      /* Sequential write: fill with incrementing byte pattern */
      DWT->CYCCNT = 0;
      for (uint32_t j = 0; j < sz; j++) p[j] = (uint8_t)j;
      uint32_t wcycles = DWT->CYCCNT;

      /* Sequential read + verify */
      uint32_t errors = 0;
      DWT->CYCCNT = 0;
      for (uint32_t j = 0; j < sz; j++) if (p[j] != (uint8_t)j) errors++;
      uint32_t rcycles = DWT->CYCCNT;

      uint32_t wmbs = (wcycles > 0U) ?
          (uint32_t)((uint64_t)sz * clk / wcycles / (1024UL * 1024UL)) : 0U;
      uint32_t rmbs = (rcycles > 0U) ?
          (uint32_t)((uint64_t)sz * clk / rcycles / (1024UL * 1024UL)) : 0U;

      int len = snprintf(ubuf, sizeof(ubuf),
          "[PSRAM] %-10s wr:%4u MB/s  rd:%4u MB/s  %s\r\n",
          bufs[i].name, (unsigned)wmbs, (unsigned)rmbs,
          errors == 0U ? "OK" : "FAIL");
      HAL_UART_Transmit(&hlpuart1, (uint8_t *)ubuf, (uint16_t)len, 500);
    }
  }
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    HAL_GPIO_TogglePin(GREEN_LED_GPIO_Port, GREEN_LED_Pin);
    HAL_Delay(200);
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief LPUART1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_LPUART1_UART_Init(void)
{

  /* USER CODE BEGIN LPUART1_Init 0 */

  /* USER CODE END LPUART1_Init 0 */

  /* USER CODE BEGIN LPUART1_Init 1 */

  /* USER CODE END LPUART1_Init 1 */
  hlpuart1.Instance = LPUART1;
  hlpuart1.Init.BaudRate = 115200;
  hlpuart1.Init.WordLength = UART_WORDLENGTH_8B;
  hlpuart1.Init.StopBits = UART_STOPBITS_1;
  hlpuart1.Init.Parity = UART_PARITY_EVEN;
  hlpuart1.Init.Mode = UART_MODE_TX_RX;
  hlpuart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  hlpuart1.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  hlpuart1.Init.ClockPrescaler = UART_PRESCALER_DIV1;
  hlpuart1.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  hlpuart1.FifoMode = UART_FIFOMODE_DISABLE;
  if (HAL_UART_Init(&hlpuart1) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_SetTxFifoThreshold(&hlpuart1, UART_TXFIFO_THRESHOLD_1_8) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_SetRxFifoThreshold(&hlpuart1, UART_RXFIFO_THRESHOLD_1_8) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_DisableFifoMode(&hlpuart1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN LPUART1_Init 2 */

  /* USER CODE END LPUART1_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOE_CLK_ENABLE();
  __HAL_RCC_GPIOO_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GREEN_LED_GPIO_Port, GREEN_LED_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin : GREEN_LED_Pin */
  GPIO_InitStruct.Pin = GREEN_LED_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GREEN_LED_GPIO_Port, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
