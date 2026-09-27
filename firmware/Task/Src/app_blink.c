/* ============================================================================
 *  app_blink.c —— 心跳灯的 Task 层适配器
 *
 *  ⚠️ 本文件**不碰 Device**：它只回答"灯此刻该亮吗"。
 *     把结论落到 GPIO 上是组合根（app.c）的活 —— 见 app.h 的说明。
 * ==========================================================================*/
#include "app_blink.h"

#include "op_blink.h"

static op_blink_t s_blink;

void app_blink_init(uint32_t on_ms, uint32_t off_ms) {
    op_blink_init(&s_blink, on_ms, off_ms);
}

int app_blink_poll(uint32_t now_ms) {
    return op_blink_step(&s_blink, now_ms) ? 1 : 0;
}
