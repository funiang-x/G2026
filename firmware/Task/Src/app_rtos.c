/* ============================================================================
 *  app_rtos.c —— 任务体与节拍（Task 层）
 *
 *  ⚠️ 本文件**只 include `app.h`**，不直接碰 Device / HAL。
 *     取时间是组合根（app.c）内部的事 —— 那样"Task 里谁能碰硬件"
 *     就只有唯一答案，可以 grep 出来。
 *
 *  ⚠️ 本文件也不 include Operation 的头：任务只调 `app_poll()` 这一个入口。
 *     业务变复杂时，在 app.c 的 app_poll() 里挂更多显式入口，而不是在这里堆。
 *
 *  ★ 为什么循环体在本文件、而不在 `Core/Src/freertos.c`：
 *    见 app_rtos.h 的文件头（CubeMX 会强制重建 defaultTask，不跟它斗）。
 * ==========================================================================*/
#include "app_rtos.h"

#include "app.h"
#include "app_tasks.h"
#include "log.h"
#include "project_config.h"

#include "cmsis_os.h"

void app_rtos_run(void) {
    /* ★ 循环体只负责"什么时候做"，不负责"做什么" ——
     *   周期逻辑一律做成**显式入口**（app_poll），见 docs/07 §周期逻辑做成显式入口。
     *   这样将来加任务 / 改节拍都在本层动，不用碰生成物。
     *
     * ★ 业务（测量 / UI）不在本任务里 —— 它们在 app_tasks_init() 建的两个任务中
     *   （Task_Engine / Task_Ui）。本任务（CubeMX 的 defaultTask）只跑轻量周期：
     *   心跳、闪灯、RTC 复查。
     * ⚠️ 建任务失败时**不停机**：app_tasks_ok() 变 0，组合根会把 engine/ui
     *   内联回 app_poll（降级路径），装置照常可用。 */
    if (app_tasks_init() != 0) {
        LOG_E("[RTOS] 多任务未建起 —— 降级为单循环（engine/ui 内联在 app_poll 里）");
    }

    /* ★ 采样链自检放在这里，不放 app_init()：它要真启动一次采集，路上有
     *   `xSemaphoreTake` 与 DMA 中断里的 `xSemaphoreGiveFromISR` 两处 FreeRTOS
     *   原语，调度器启动前跑会被 ISR 入栈校验断言拦成死循环（2026-10-02 实测）。
     *   建任务成功与否都跑 —— 降级路径下采样链照样要能用。
     *
     * ★ 2026-10-03：**先等模拟轨建立**。`VBIAS` 由 `2.0kΩ`（R9//R10//R2//R5//R7）
     *   充 `C13 = 10µF` ⇒ τ = 20ms、到 0.1% 要 138ms；不等就采，帧直流均值会偏出
     *   半量程带 ⇒ 报一行**假的**「模拟轨可疑」。
     *   改前靠 `bsp_lcd_init()` 里 140ms 的 HAL_Delay 侥幸盖住 —— 那是屏的时序需求，
     *   不该被当成 AFE 的依赖。机理与取值见 `board_config.h` 的 `BOARD_AFE_SETTLE_MS`。 */
    osDelay(APP_AFE_SETTLE_MS);
    app_adc_selftest();

    for (;;) {
        app_poll();
        osDelay(APP_TICK_PERIOD_MS);
    }
}
