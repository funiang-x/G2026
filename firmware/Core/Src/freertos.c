/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : freertos.c
  * Description        : Code for freertos applications
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
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "cmsis_os.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
/* ★ 本文件在 `Core/` 下（最底层、CubeMX 生成物），它是**程序入口**而不是
 *   四层里的一层 —— 所以"Core 调 Task"不算越层（docs/01 §组合根）。
 *   规矩：这里**只写胶水**，任何逻辑都回 Task/ 层。
 *
 * 注意只需要 app_rtos.h：初始化由 main.c 的 app_init() 负责（时序见 app.h）。 */
#include "app_rtos.h"
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
/* USER CODE BEGIN Variables */

/* USER CODE END Variables */
/* Definitions for defaultTask */
osThreadId_t defaultTaskHandle;
const osThreadAttr_t defaultTask_attributes = {
  .name = "defaultTask",
  .stack_size = 256 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */

/* USER CODE END FunctionPrototypes */

void StartDefaultTask(void *argument);

void MX_FREERTOS_Init(void); /* (MISRA C 2004 rule 8.1) */

/**
  * @brief  FreeRTOS initialization
  * @param  None
  * @retval None
  */
void MX_FREERTOS_Init(void) {
  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* USER CODE BEGIN RTOS_MUTEX */
  /* add mutexes, ... */
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  /* add queues, ... */
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of defaultTask */
  defaultTaskHandle = osThreadNew(StartDefaultTask, NULL, &defaultTask_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  /* add threads, ... */
  /* ★ 这里**故意是空的**。理由（实测）：CubeMX 会强制重建 defaultTask 与
   *   StartDefaultTask —— 在 .ioc 里清空 FREERTOS.IPParameters、删掉 Tasks01
   *   后重新生成，它照样补回来。所以不跟它斗：
   *       本任务（defaultTask）就是本工程的**唯一周期任务**，
   *       它的栈深 / 优先级在 .ioc 的 FREERTOS.Tasks01 里给（256 words / 优先级 24），
   *       循环体在 Task/Src/app_rtos.c 的 app_rtos_run()。
   *   ⇒ 加新任务时，在 Task/ 层用 xTaskCreate 建，参数写进 project_config.h。
   *     不要在这里建 —— 这是生成物（docs/01 规矩 4）。 */
  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  /* add events, ... */
  /* USER CODE END RTOS_EVENTS */

}

/* USER CODE BEGIN Header_StartDefaultTask */
/**
  * @brief  Function implementing the defaultTask thread.
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_StartDefaultTask */
void StartDefaultTask(void *argument)
{
  /* USER CODE BEGIN StartDefaultTask */
  (void)argument; /* 未使用参数：显式标注，否则 -Wextra 会报 */

  /* ★ 只留一行胶水：循环体、节拍、将来的多任务划分全在 Task/ 层。
   *   这样"改调度"不用碰生成物，也不会被下次 CubeMX 生成冲掉。
   *   ⚠️ app_rtos_run() **不返回** —— 返回等于把生成物的 osDelay(1) 空转循环
   *      跑起来，那是另一套节拍了。 */
  app_rtos_run();
  /* USER CODE END StartDefaultTask */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */

/* USER CODE END Application */

