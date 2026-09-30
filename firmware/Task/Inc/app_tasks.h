#ifndef APP_TASKS_H
#define APP_TASKS_H
/* ============================================================================
 *  app_tasks —— 任务创建（全静态）· 见 project_config.h 的参数小节
 *
 *  ---- 任务表（2026-09-28 定）----
 *      Task_Engine  26  ·  Task_Ui  23  ·  defaultTask(CubeMX)  24
 *
 *      Task_Engine  测量编排：阻塞等 ADC 帧信号量 → DSP 链 → 发结果（app_engine.c）
 *      Task_Ui      LVGL 主循环 + 四页面刷新（app_ui.c）—— 全工程唯一碰 LVGL 的任务
 *      defaultTask  心跳 / 闪灯 / RTC（app_rtos.c 的 app_rtos_run）—— CubeMX 生成
 *
 *  ---- 跨任务数据通路 ----
 *      Task_Engine ──[结果 mailbox：1 深队列 + xQueueOverwrite，≤64B]──▶ Task_Ui
 *      帧就绪       ──[ADC 帧二值信号量：DMA 中断给、engine 取]──▶ Task_Engine
 *      重测请求     ──[任务通知：UI 发、engine 收]──▶ Task_Engine
 *
 *  ---- 为什么全部用 `xTaskCreateStatic` ----
 *  栈与 TCB 一律是本文件的 **static 数组**，不动 FreeRTOS 堆（堆只留给内核对象
 *  与 LVGL 的互斥量，configTOTAL_HEAP_SIZE = 15KB）。代价是"栈深写在编译期"，
 *  换来的是**不会有运行期分配失败**。
 *
 *  ---- 降级 ----
 *  `app_tasks_init()` 失败时返回非 0；组合根（app.c）据 `app_tasks_ok()`
 *  退回旧形态：在 app_poll 里每 10ms 内联跑 engine/ui（保底可用，不静默变砖）。
 * ==========================================================================*/

#ifdef __cplusplus
extern "C" {
#endif

/* 建 Task_Engine / Task_Ui（静态）。0 = 成功，-1 = 失败。
 * ⚠️ 由 `app_rtos_run()`（defaultTask 体）在调度器启动后调一次。 */
int app_tasks_init(void);

/* 1 = 上面两个任务确实建起来了（组合根据此决定是否走降级路径）。 */
int app_tasks_ok(void);

/* 把两个任务的**栈高水位**（剩余栈最小值，单位 word）打进日志。
 *
 * ★ 为什么需要它：栈溢出在裸机上是"随机跑飞"，极难归因。高水位只降不升，
 *   所以"它贴近 0"就是栈快不够的**提前预警** —— 这是 `configCHECK_FOR_STACK_OVERFLOW`
 *   之外唯一的观测手段。
 * ⚠️ 要**上板跑够久**才有意义：至少覆盖 DSP 全链 + LVGL 重绘 + 一次标定
 *   （标定在 UI 任务里跑同步测量，是 UI 栈的最坏路径）。
 * 由组合根在 APP_STACK_REPORT_MS 时调一次（见 app.c）。 */
void app_tasks_report_stack(void);

#ifdef __cplusplus
}
#endif
#endif /* APP_TASKS_H */
