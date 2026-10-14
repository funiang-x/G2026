/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    stm32f4xx_it.c
  * @brief   Interrupt Service Routines.
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
#include "stm32f4xx_it.h"
/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
/* ★ 崩溃取证：HardFault_Handler 要把栈帧交给 Device 层。
 *   Core/ 是生成物，但 USER CODE 段可以 include 手写层的头（同 main.c 的做法）。 */
#include "bsp_fault.h"
/* ★ 2026-09-28：ADC 的 DMA 中断已由 CubeMX 生成的
 *   `DMA2_Stream0_IRQHandler → HAL_DMA_IRQHandler(&hdma_adc1)` 直接接管
 *   （.ioc 勾选了 DMA2_Stream0 中断），旧的 bsp_adc 转发机制退役。 */
/* ★ 2026-10-14（C9③）：曾为"LCD 像素搬运的 DMA 完成中断"在这里 include 过 bsp_lcd.h，
 *   那个中断入口已删除（完成检测改成轮询，见文件尾 USER CODE 1 的说明）⇒ 这里撤掉。 */
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN TD */

/* USER CODE END TD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/* External variables --------------------------------------------------------*/
extern DMA_HandleTypeDef hdma_adc1;
extern TIM_HandleTypeDef htim6;

/* USER CODE BEGIN EV */

/* USER CODE END EV */

/******************************************************************************/
/*           Cortex-M4 Processor Interruption and Exception Handlers          */
/******************************************************************************/
/**
  * @brief This function handles Non maskable interrupt.
  */
void NMI_Handler(void)
{
  /* USER CODE BEGIN NonMaskableInt_IRQn 0 */

  /* USER CODE END NonMaskableInt_IRQn 0 */
  /* USER CODE BEGIN NonMaskableInt_IRQn 1 */
   while (1)
  {
  }
  /* USER CODE END NonMaskableInt_IRQn 1 */
}

/**
  * @brief This function handles Hard fault interrupt.
  */
void HardFault_Handler(void)
{
  /* USER CODE BEGIN HardFault_IRQn 0 */
  /* ★ 崩溃取证（2026-09-23 补上 —— 此前这里只有 `while(1)`，
   *   `bsp_fault.c` 写了却**从来没被调用**，崩溃时静默死循环、RTT 一字不出）。
   *
   * 为什么能在这里拿到栈帧：`lr` 的 bit2 = 1 表示异常前用的是 PSP（任务栈），
   * 否则是 MSP（主栈）。据此选对栈指针，它就是压好的异常栈帧首地址。
   * ⚠️ 不用"naked 裸函数 + 内联汇编跳转"那套 —— 那要改生成区的函数声明，
   *    下次 Generate Code 就被覆盖。写在 USER CODE 段里更稳。
   * ⚠️ `bsp_fault_report()` 是 noreturn，不会返回到下面的 `while(1)`。 */
  {
    uint32_t lr;
    __asm volatile("mov %0, lr" : "=r"(lr));
    uint32_t *sf = (uint32_t *)(((lr & 0x4U) != 0U) ? __get_PSP() : __get_MSP());
    bsp_fault_report(sf);
  }

  /* USER CODE END HardFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_HardFault_IRQn 0 */
    /* USER CODE END W1_HardFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Memory management fault.
  */
void MemManage_Handler(void)
{
  /* USER CODE BEGIN MemoryManagement_IRQn 0 */

  /* USER CODE END MemoryManagement_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_MemoryManagement_IRQn 0 */
    /* USER CODE END W1_MemoryManagement_IRQn 0 */
  }
}

/**
  * @brief This function handles Pre-fetch fault, memory access fault.
  */
void BusFault_Handler(void)
{
  /* USER CODE BEGIN BusFault_IRQn 0 */

  /* USER CODE END BusFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_BusFault_IRQn 0 */
    /* USER CODE END W1_BusFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Undefined instruction or illegal state.
  */
void UsageFault_Handler(void)
{
  /* USER CODE BEGIN UsageFault_IRQn 0 */

  /* USER CODE END UsageFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_UsageFault_IRQn 0 */
    /* USER CODE END W1_UsageFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Debug monitor.
  */
void DebugMon_Handler(void)
{
  /* USER CODE BEGIN DebugMonitor_IRQn 0 */

  /* USER CODE END DebugMonitor_IRQn 0 */
  /* USER CODE BEGIN DebugMonitor_IRQn 1 */

  /* USER CODE END DebugMonitor_IRQn 1 */
}

/******************************************************************************/
/* STM32F4xx Peripheral Interrupt Handlers                                    */
/* Add here the Interrupt Handlers for the used peripherals.                  */
/* For the available peripheral interrupt handler names,                      */
/* please refer to the startup file (startup_stm32f4xx.s).                    */
/******************************************************************************/

/**
  * @brief This function handles TIM6 global interrupt, DAC1 and DAC2 underrun error interrupts.
  */
void TIM6_DAC_IRQHandler(void)
{
  /* USER CODE BEGIN TIM6_DAC_IRQn 0 */

  /* USER CODE END TIM6_DAC_IRQn 0 */
  HAL_TIM_IRQHandler(&htim6);
  /* USER CODE BEGIN TIM6_DAC_IRQn 1 */

  /* USER CODE END TIM6_DAC_IRQn 1 */
}

/**
  * @brief This function handles DMA2 stream0 global interrupt.
  */
void DMA2_Stream0_IRQHandler(void)
{
  /* USER CODE BEGIN DMA2_Stream0_IRQn 0 */

  /* USER CODE END DMA2_Stream0_IRQn 0 */
  HAL_DMA_IRQHandler(&hdma_adc1);
  /* USER CODE BEGIN DMA2_Stream0_IRQn 1 */

  /* USER CODE END DMA2_Stream0_IRQn 1 */
}

/* USER CODE BEGIN 1 */

/* ★ 2026-10-14（C9③）：这里**原本**有一个 `DMA2_Stream1_IRQHandler`（LCD 像素搬运
 * 的 DMA 完成中断，转发到 Device/Src/bsp_lcd.c）。
 * ⚠️ 已删除：上板实测"TC 中断 + 中断里还总线闸"这条路会**把系统钉死**（两次，
 *    心跳 LED 停、日志断在最后一行、无故障转储）⇒ 改成**轮询 EN 位**检测完成
 *    （bsp_lcd.c 的"为什么放弃中断"一节有完整记录）。
 * ⇒ 所以现在**不需要**任何新的中断入口。 */

/* USER CODE END 1 */
