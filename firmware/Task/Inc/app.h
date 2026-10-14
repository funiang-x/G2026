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

/* 报告"某个 HAL 初始化没成功"，但**不停机**。
 *
 * ★ 为什么需要它（2026-09-23 上板实测）：
 *   CubeMX 生成的 `Error_Handler()` 默认实现是 `__disable_irq(); while(1);` ——
 *   **完全无声**，而且它会被**所有** HAL 初始化失败调到（其中曾包括
 *   `MX_SDIO_SD_Init()`：没插 TF 卡时 `HAL_SD_Init()` 必然失败，而 TF 卡是
 *   可选外设，不该让整块板子起不来 —— 那次的调用栈是
 *   `main → MX_SDIO_SD_Init → Error_Handler → 停机`，只留一行 [AI_FAIL]）。
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

/* ---- 上电自检结果（给 UI 第 3 页 STATE 显示）---------------------------
 * ★ 为什么要有这个出口：自检原来**只打 RTT 日志** —— 那条链只有插着 J-Link 的
 *   人看得到，现场评委什么都看不到。而"这台机器知道自己好不好"本身就是技术力
 *   的一部分（grill-me 拷问后定下的），所以把结果搬一份到屏上。
 *
 * ⚠️ 结果是**边跑边填**的：第 ①~⑧ 项在 `app_init()` 里跑（早于 UI 首帧），
 *    但第 ⑤ 项 ADC 自检在 `app_adc_selftest()`（调度器起来之后）才跑 ⇒
 *    **别在 `app_ui_init()` 里读一次就完事**，第 3 页要定时重读。
 *
 * ⚠️ `name` 必然是**英文**：LVGL 内置字体无 CJK 字形，中文会渲染成方块。 */
#define APP_ST_MAX 8
typedef struct {
    const char *name;
    int         ok; /* 1 = 通过，0 = 失败 */
} app_st_item_t;

/* 把已收集到的自检结果拷进调用方给的数组，返回写入的项数。
 * `cap < APP_ST_MAX` 返回 -1（调用方数组不够，宁可不填也不要越界写）。 */
int app_selftest_snapshot(app_st_item_t *out, int cap);

#ifdef __cplusplus
}
#endif
#endif /* APP_H */
