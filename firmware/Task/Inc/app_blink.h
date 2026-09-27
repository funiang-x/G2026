#ifndef APP_BLINK_H
#define APP_BLINK_H
/* ============================================================================
 *  app_blink.h —— Task 层的**适配器**：把 op_blink 的纯结论接上"现在几点"
 *
 *  四步样板的第 3 步（见 docs/10 §A）：
 *      Operation/Inc/op_blink.h + Src/op_blink.c   纯逻辑（吃数字吐数字）
 *      Test/test_op_blink.c                         PC 单测
 *      Task/Src/app_blink.c                         ← 本文件：取时间 / 传时间 / 出结论
 *      Task/Src/app.c                               组合根：把结论落到 GPIO 上
 *
 *  ⚠️ 本文件**只 include `Operation/Inc` 与 `Common/Inc`**，不碰 Device ——
 *     "把结论落到硬件上"是组合根的活（见 app.h 的说明）。
 * ==========================================================================*/
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 注入参数。**由组合根调用**，参数来自 `project_config.h` ——
 * Operation 自己不读配置头，那样它才能被任意参数在 PC 上单测。 */
void app_blink_init(uint32_t on_ms, uint32_t off_ms);

/* 推进一步。返回 **1 = 灯此刻应该亮**，0 = 灭。
 *
 * ⚠️ 返回"应该怎样"而不是"已经怎样"：本函数不碰硬件。
 *    `now_ms` 由调用方传入（这里由组合根从 `bsp_tick_ms()` 取）。 */
int app_blink_poll(uint32_t now_ms);

#ifdef __cplusplus
}
#endif
#endif /* APP_BLINK_H */
