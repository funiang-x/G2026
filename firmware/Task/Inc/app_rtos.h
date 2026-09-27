#ifndef APP_RTOS_H
#define APP_RTOS_H
/* ============================================================================
 *  app_rtos.h —— Task 层：调度骨架（**唯一允许出现 FreeRTOS API 的地方之一**）
 *
 *  四层里的位置：Task（调度）→ Operation（纯逻辑）→ Device（硬件）。
 *
 *  ================== 本工程怎么接 FreeRTOS（实测过的取舍） ==================
 *
 *  分工是「**CubeMX 生成内核与配置，调度策略写在 Task/ 层**」：
 *
 *    CubeMX 里只勾 FreeRTOS 内核（Interface = CMSIS_V2），**不要用它的图形
 *    界面建业务任务** —— 那样任务会落在 `Core/Src/freertos.c` 里，而 `Core/`
 *    是生成物（docs/01 规矩 4）。
 *
 *  ⚠️ 但实测：CubeMX 会**强制重建** `defaultTask` 与 `StartDefaultTask` ——
 *     在 `.ioc` 里把 `FREERTOS.IPParameters` 清空、删掉 `Tasks01` 后重新生成，
 *     它照样自动补回来。所以**不跟它斗**，改成：
 *
 *         CubeMX 建空壳任务（栈深在 .ioc 里给够 256 words）
 *                    ↓
 *         `Core/Src/freertos.c` 的 USER CODE 段只留一行胶水：app_rtos_run()
 *                    ↓
 *         真正的循环体、节拍、将来的多任务划分全在**本层**（app_rtos.c）
 *
 *     好处：生成物里没有逻辑，改调度不用碰生成物，也不会被下次生成冲掉。
 * ==========================================================================*/
#ifdef __cplusplus
extern "C" {
#endif

/* 调度器的主任务体。**不返回**（内部是 for(;;)）。
 *
 * 调用点（只有一处）：`Core/Src/freertos.c` 的 `USER CODE BEGIN StartDefaultTask`
 * —— 也就是 CubeMX 那个默认任务的循环体。替换掉它原来的 `osDelay(1)` 空转。
 *
 * 调用时机的保证：`osKernelInitialize()` 已调、`osKernelStart()` 未调，
 * 且 HAL / 时钟 / `app_init()` 都已完成（见 Core/Src/main.c 的 USER CODE BEGIN 2）。
 *
 * ⚠️ 这里**不做初始化** —— 初始化是组合根（app.c）的事。本函数只负责
 *    "每隔 APP_TICK_PERIOD_MS 调一次 app_poll()"。 */
void app_rtos_run(void);

#ifdef __cplusplus
}
#endif
#endif /* APP_RTOS_H */
