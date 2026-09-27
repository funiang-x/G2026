#ifndef OP_BLINK_H
#define OP_BLINK_H
/* ============================================================================
 *  op_blink.h —— 心跳灯闪烁逻辑（**纯逻辑样板**）
 *
 *  本文件是 Operation 层：**不许**出现 HAL / FreeRTOS / Device 的任何东西。
 *  时间由调用方以 `now_ms` 参数传入（见 docs/01 规矩 2）。
 *
 *  ★ 它的存在是为了给"新增业务逻辑"提供一个可模仿的最小样板（docs/10 §A）。
 *    完整四步：
 *      Operation/Inc/op_blink.h + Src/op_blink.c   纯逻辑（本文件）
 *      Test/test_op_blink.c                         PC 单测，`fw.py test` 秒级跑完
 *      Task/Src/app_blink.c                         取时间 / 传时间 / 按结论驱动 Device
 * ==========================================================================*/
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t on_ms;    /* 亮多久 */
    uint32_t off_ms;   /* 灭多久 */
    uint32_t phase_ms; /* 内部：本次周期的起点（调用方不要读） */
} op_blink_t;

/* 初始化。`on_ms` 与 `off_ms` 由 Task 层从 project_config.h 注入 ——
 * Operation **不读配置头**，那样它才能被任意参数在 PC 上单测。 */
void op_blink_init(op_blink_t *ctx, uint32_t on_ms, uint32_t off_ms);

/* 推进一步：给定"现在过了几毫秒"，返回灯**此刻应该亮吗**。
 *
 * ⚠️ 判据用**无符号减法**算周期内相位，因此天然处理 now_ms 的 2^32 回绕 ——
 *    不要改写成 `if (now_ms > ctx->phase_ms + ctx->on_ms)` 那种带加法的比较，
 *    那会在回绕点出错。Test/test_op_blink.c 里有跨回绕的用例守着这条。 */
bool op_blink_step(op_blink_t *ctx, uint32_t now_ms);

#endif /* OP_BLINK_H */
