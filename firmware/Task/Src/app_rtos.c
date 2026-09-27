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
#include "project_config.h"

#include "cmsis_os.h"

void app_rtos_run(void) {
    /* ★ 循环体只负责"什么时候做"，不负责"做什么" ——
     *   周期逻辑一律做成**显式入口**（app_poll），见 docs/07 §周期逻辑做成显式入口。
     *   这样将来加任务 / 改节拍都在本层动，不用碰生成物。 */
    for (;;) {
        app_poll();
        osDelay(APP_TICK_PERIOD_MS);
    }
}
