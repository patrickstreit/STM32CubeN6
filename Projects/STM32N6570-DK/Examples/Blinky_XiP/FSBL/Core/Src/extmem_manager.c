/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : extmem_manager.c
  * @version        : 1.0.0
  * @brief          : This file implements the extmem configuration
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
#include "extmem_manager.h"
#include <string.h>

/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* USER CODE BEGIN PV */
/* Private variables ---------------------------------------------------------*/

/* USER CODE END PV */

/* USER CODE BEGIN PFP */
/* Private function prototypes -----------------------------------------------*/

/* USER CODE END PFP */

/*
 * -- Insert your variables declaration here --
 */
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/*
 * -- Insert your external function declaration here --
 */
/* USER CODE BEGIN 1 */

/* USER CODE END 1 */

/**
  * Init External memory manager
  * @retval None
  */
void MX_EXTMEM_MANAGER_Init(void)
{

  /* USER CODE BEGIN MX_EXTMEM_Init_PreTreatment */

  /* USER CODE END MX_EXTMEM_Init_PreTreatment */

  /* Initialization of the memory parameters */
  memset(extmem_list_config, 0x0, sizeof(extmem_list_config));

  /* EXTMEMORY_1 */
  extmem_list_config[0].MemType = EXTMEM_PSRAM;
  extmem_list_config[0].Handle = (void*)&hxspi1;
  extmem_list_config[0].ConfigType = EXTMEM_LINK_CONFIG_16LINES;

  extmem_list_config[0].PsramObject.psram_public.MemorySize = HAL_XSPI_SIZE_256MB;
  extmem_list_config[0].PsramObject.psram_public.FreqMax = 200 * 1000000u;
  extmem_list_config[0].PsramObject.psram_public.NumberOfConfig = 0u;

  /* Memory command configuration */
  extmem_list_config[0].PsramObject.psram_public.ReadREG           = 0x40u;
  extmem_list_config[0].PsramObject.psram_public.WriteREG          = 0xC0u;
  extmem_list_config[0].PsramObject.psram_public.ReadREGSize       = 2u;
  extmem_list_config[0].PsramObject.psram_public.REG_DummyCycle    = 5u;
  extmem_list_config[0].PsramObject.psram_public.Write_command     = 0x80u;
  extmem_list_config[0].PsramObject.psram_public.Write_DummyCycle  = 6u;
  extmem_list_config[0].PsramObject.psram_public.Read_command      = 0x00u;
  extmem_list_config[0].PsramObject.psram_public.WrapRead_command  = 0x00u;
  extmem_list_config[0].PsramObject.psram_public.Read_DummyCycle   = 6u;

  /* EXTMEMORY_2 */
  extmem_list_config[1].MemType = EXTMEM_NOR_SFDP;
  extmem_list_config[1].Handle = (void*)&hxspi2;
  extmem_list_config[1].ConfigType = EXTMEM_LINK_CONFIG_8LINES;

  EXTMEM_Init(EXTMEMORY_1, HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_XSPI1));
  EXTMEM_Init(EXTMEMORY_2, HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_XSPI2));

  /* USER CODE BEGIN MX_EXTMEM_Init_PostTreatment */
  /* APS256XX PSRAM — configure MR0/MR4/MR8 before MapMemory() activates
   * memory-mapped mode.  Must match the EXTMEM params above:
   *   Fixed Latency, LC=6  →  6 read/write dummy cycles
   * After EXTMEM_Init(EXTMEMORY_1) hxspi1 is in READY state at 32 MHz
   * (SAL_XSPI_SetClock already set prescaler=0).
   * Reference: XSPI_PSRAM_MemoryMapped/FSBL/Core/Src/main.c Configure_APMemory()
   */
  {
    XSPI_RegularCmdTypeDef aps_cmd = {0};
    uint8_t mr_val[2];

    /* Common fields for all three register writes */
    aps_cmd.OperationType      = HAL_XSPI_OPTYPE_COMMON_CFG;
    aps_cmd.InstructionMode    = HAL_XSPI_INSTRUCTION_8_LINES;
    aps_cmd.InstructionWidth   = HAL_XSPI_INSTRUCTION_8_BITS;
    aps_cmd.InstructionDTRMode = HAL_XSPI_INSTRUCTION_DTR_DISABLE;
    aps_cmd.Instruction        = 0xC0U;   /* WRITE_REG_CMD */
    aps_cmd.AddressMode        = HAL_XSPI_ADDRESS_8_LINES;
    aps_cmd.AddressWidth       = HAL_XSPI_ADDRESS_32_BITS;
    aps_cmd.AddressDTRMode     = HAL_XSPI_ADDRESS_DTR_ENABLE;
    aps_cmd.AlternateBytesMode = HAL_XSPI_ALT_BYTES_NONE;
    aps_cmd.DataMode           = HAL_XSPI_DATA_8_LINES;
    aps_cmd.DataDTRMode        = HAL_XSPI_DATA_DTR_ENABLE;
    aps_cmd.DataLength         = 2U;
    aps_cmd.DummyCycles        = 0U;
    aps_cmd.DQSMode            = HAL_XSPI_DQS_DISABLE;

    /* MR0 = {0x30, 0x8D}: Fixed Latency, Read Latency Code, Drive Strength */
    aps_cmd.Address = 0x00000000U;
    (void)HAL_XSPI_Command(&hxspi1, &aps_cmd, HAL_XSPI_TIMEOUT_DEFAULT_VALUE);
    mr_val[0] = 0x30U; mr_val[1] = 0x8DU;
    (void)HAL_XSPI_Transmit(&hxspi1, mr_val, HAL_XSPI_TIMEOUT_DEFAULT_VALUE);

    /* MR4 = {0x20, 0xF0}: Write Latency Code */
    aps_cmd.Address = 0x00000004U;
    (void)HAL_XSPI_Command(&hxspi1, &aps_cmd, HAL_XSPI_TIMEOUT_DEFAULT_VALUE);
    mr_val[0] = 0x20U; mr_val[1] = 0xF0U;
    (void)HAL_XSPI_Transmit(&hxspi1, mr_val, HAL_XSPI_TIMEOUT_DEFAULT_VALUE);

    /* MR8 = {0x4B, 0x08}: Burst Type (Linear Burst enabled) */
    aps_cmd.Address = 0x00000008U;
    (void)HAL_XSPI_Command(&hxspi1, &aps_cmd, HAL_XSPI_TIMEOUT_DEFAULT_VALUE);
    mr_val[0] = 0x4BU; mr_val[1] = 0x08U;
    (void)HAL_XSPI_Transmit(&hxspi1, mr_val, HAL_XSPI_TIMEOUT_DEFAULT_VALUE);
  }
  /* USER CODE END MX_EXTMEM_Init_PostTreatment */
}
