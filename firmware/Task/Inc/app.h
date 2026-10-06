#ifndef APP_H
#define APP_H
/* ============================================================================
 *  app.h —— **组合根**的对外接口（Task 层）
 *
 *  ★ `Task/Src/app.c` 是四层结构里**唯一允许跨层**的地方
 *    （Composition Root，见 docs/01）。它同时看 `Operation/` 与 `Device/`，
 *    职责只有三件事：**创建实例 → 注入参数 → 启动**。
 *
 *    ⇒ 因此本工程有一条比参考工程更严的约定：
 *      **`Task/` 下只有 `app.c` 允许 include `Device/Inc`**。
 *      其余 Task 文件（如 `app_blink.c`）只 include `Operation/Inc` 与 `Common/Inc`，
 *      它们负责"取数 / 传时间 / 出结论"，**不负责把结论落到硬件上** ——
 *      那一步回 `app_poll()` 做。
 *
 *      这么切的原因：装配与业务分开后，"谁把 Device 交到 Operation 手上"
 *      只有一个答案（app.c），层间依赖变成可以 grep 的事实。
 * ==========================================================================*/
#include <stdint.h> /* uint32_t / uint8_t —— app_flash_write 的参数 */

#ifdef __cplusplus
extern "C" {
#endif

/* 上电初始化。由 `Core/Src/main.c` 的 `USER CODE BEGIN 2` 调用。
 *
 * 时序（务必保持）：HAL_Init → SystemClock_Config → MX_*_Init → **app_init()**
 * → osKernelInitialize → MX_FREERTOS_Init → osKernelStart
 * ⇒ 也就是说：`app_init()` 运行时**调度器还没启动**，
 *   所以它里面不许取任何 FreeRTOS 原语（锁 / 队列 / 延时）。
 *   需要锁的东西一律惰性创建（见 Device/Src/bsp_log.c 的 log_lock）。 */
void app_init(void);

/* 周期入口。由 `Task/Src/app_rtos.c` 的任务循环体调用，周期 = `APP_TICK_PERIOD_MS`。
 *
 * ★ 做成**显式入口**而不是把实现写进 `for(;;)` 里（docs/07 §周期逻辑做成显式入口）：
 *   将来调整任务划分时不用搬家，而且它本身是"这一拍要做什么"的清单。 */
void app_poll(void);

/* 采样链自检：**必须在调度器启动后跑**，由 `app_rtos.c` 建完任务后调一次。
 *
 * ★ 为什么它不像其它自检那样待在 `app_init()` 的 `app_self_test()` 里
 *   （2026-10-02 上板实测，卡死在这里）：
 *   本项要**真的启动一次采集**，而那条路上有两处 FreeRTOS 原语 ——
 *     · `bsp_adc_start()` 里的 `xSemaphoreTake()`（清陈旧计数）
 *     · DMA2_Stream0 中断里的 `xSemaphoreGiveFromISR()`
 *   调度器启动前跑：ISR 入栈校验 `vPortValidateInterruptPriority()` 会在
 *   还没被 `xPortStartScheduler()` 赋值的 `ucMaxSysCallPriority` 上失败 →
 *   `configASSERT` 展开成 `taskDISABLE_INTERRUPTS(); for(;;);` → **死循环**。
 *   ⇒ 这正是本文件顶部那条"`app_init()` 里不许取任何 FreeRTOS 原语"的兑现。 */
void app_adc_selftest(void);

/* 致命错误路径：**出声 + 停机**，不返回。
 *
 * 调用点（只有一处）：`Core/Src/main.c` 的 `Error_Handler()`。
 *
 * ★ 为什么需要它（实测过的可用性坑）：
 *   CubeMX 生成的 `Error_Handler()` 默认实现是 `__disable_irq(); while(1);` ——
 *   **完全无声**。而它最常被谁调？`SystemClock_Config()` 里 HSE 起振失败
 *   （板子没焊晶振 / 晶振坏 / `HSE_VALUE` 与实际不符）。
 *   那时的现象是"板子毫无反应、RTT 一个字都没有" ——
 *   与"固件根本没烧进去""探针没插"**完全无法区分**，白查半小时。
 *
 * ★ 打的是 `BOARD_BOOT_FAIL_MARKER`（`[AI_FAIL]`）⇒ `fw.py verify` 会返回
 *   **退出码 1（固件主动报错）**，而不是 2（超时、原因未知）。
 *   把"未知"变成"已知"，这是本工程"故障要响亮"原则的直接兑现。
 *
 * ⚠️ 内部只能用 **nolock** 日志：此刻调度器可能没起、时钟可能不对，
 *    取锁 / 等中断都是死路。
 * ⚠️ 它**不返回** —— 别在需要继续执行的地方调。 */
#if defined(__GNUC__)
__attribute__((noreturn))
#endif
void app_panic(const char *reason);

/* 报告"某个 HAL 初始化没成功"，但**不停机**（与 app_panic 语义相反）。
 *
 * ★ 为什么需要它（2026-09-23 上板实测）：
 *   `Error_Handler()` 会被**所有** HAL 初始化失败调到，其中最典型的是
 *   `MX_SDIO_SD_Init()` —— 板子上没插 TF 卡时 `HAL_SD_Init()` 必然失败。
 *   而 TF 卡是**可选外设**，不该让整块板子起不来。
 *
 *   实测到的调用栈：
 *     Reset_Handler → main → MX_SDIO_SD_Init → Error_Handler → app_panic（停机）
 *
 * ⇒ `Error_Handler` 改调本函数：打一行 ERROR 日志，然后**返回**，
 *   让 `main()` 继续走（`app_init()` 照常执行，板子照常跑）。
 *
 * ⚠️ 本函数**故意不打 `[AI_FAIL]` 标记** —— 那是 `fw.py verify` 的"失败"判据，
 *    打出来会把"其实跑起来了"误判成失败。 */
void app_init_failed(const char *reason);

/* 写一段数据到板载 SPI Flash（**跨页自动拆分**）。
 *
 * ★ 这是"组合根"的样板，演示四层怎么走：
 *     `op_flash`（Operation）算拆分方案 → `bsp_spi`（Device）逐段写硬件。
 *   为什么非要绕这一下：拆分是**纯计算**，放 Operation 才能在 PC 上单测
 *   （`Test/test_op_flash.c`，8 个用例）；而 R1 禁止 Device 调 Operation，
 *   所以"组合"这件事只能在组合根做。
 *
 * 返回 0 成功；-1 = 参数非法 / 需要的段数超过 `APP_FLASH_MAX_SEGS` / 写超时。
 * ⚠️ **不负责擦除** —— Flash 只能 1→0，改写已有数据前要先擦扇区，见 bsp_spi.h。 */
int app_flash_write(uint32_t addr, const uint8_t *buf, uint32_t len);

#ifdef __cplusplus
}
#endif
#endif /* APP_H */
