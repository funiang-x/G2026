/* ============================================================================
 *  app_rtos.c —— **Task_Sys** 的任务体与节拍（Task 层）
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
 *
 *  ★ 2026-09-27：本任务就是配置文档 §9 任务表里的 **`Task_Sys`**（优先级 1）。
 *    它就是 CubeMX 生成的那个 `defaultTask` —— `.ioc` 的 FREERTOS.Tasks01
 *    里把优先级设成 1、栈 256 words。（CubeMX 会强制保留这个任务：在 .ioc 里
 *    删掉它，下次 Generate Code 也会被补回来，实测过。）
 *    职责：心跳、栈高水位、开机自检（自检在 app_init 里跑，见 app.c）。
 *
 *  ⚠️ 它是全工程**唯一**动态创建的任务（CMSIS 的 osThreadNew）。其余 4 个
 *     任务在 `Task/Src/app_tasks.c` 里用 `xTaskCreateStatic` 建。
 *     原因：CubeMX 6.15 对"静态任务"的代码生成有 bug（会生成 `.cb_mem = &NULL`，
 *     编译不过），而 CubeMX 又不肯放弃这个任务 ⇒ 让它当 Task_Sys，
 *     把 FreeRTOS 堆的占用压到最小（8KB 只够它 + LVGL 的互斥量）。
 * ==========================================================================*/
#include "app_rtos.h"

#include "app.h"
#include "project_config.h"

#include "cmsis_os.h"

void app_rtos_run(void) {
    /* ★ 循环体只负责"什么时候做"，不负责"做什么" ——
     *   周期逻辑一律做成**显式入口**（app_poll），见 docs/07 §周期逻辑做成显式入口。
     *   这样将来加任务 / 改节拍都在本层动，不用碰生成物。
     *   ⚠️ 节拍 10ms 是"业务轮询"的粒度；LVGL 有自己的循环（Task_Ui，5ms），
     *      两者互不干扰 —— `lv_timer_handler()` **只能在一个任务里调**。 */
    for (;;) {
        app_poll();
        (void)osDelay(APP_TICK_PERIOD_MS);
    }
}
