/* ============================================================================
 *  op_blink.c —— 心跳灯闪烁逻辑的实现
 *
 *  ⚠️ 本文件**不许** include 任何 HAL / FreeRTOS / Device 头。
 *     验证方式有两条，都要过：
 *       ① `python Tools/fw.py build --clean-first` —— operation 目标拿不到那些
 *          include 路径，写了 HAL 会直接 `fatal error: stm32f4xx_hal.h: No such file`
 *       ② `python Tools/fw.py test` —— PC 侧没有 HAL / FreeRTOS 符号，
 *          自己 `extern` 声明硬调会在链接期报 `undefined reference`
 * ==========================================================================*/
#include "op_blink.h"

void op_blink_init(op_blink_t *ctx, uint32_t on_ms, uint32_t off_ms) {
    ctx->on_ms = on_ms;
    ctx->off_ms = off_ms;
    ctx->phase_ms = 0U;
}

bool op_blink_step(op_blink_t *ctx, uint32_t now_ms) {
    uint32_t period = ctx->on_ms + ctx->off_ms;

    /* 周期为 0 是调用方的配置错误。这里返回"灭"而不是死循环或除零 ——
     * 纯函数不该有"失败"这个返回值（见 docs/07 §返回值约定），
     * 所以选一个确定的行为，由 Task 层去保证参数合法。 */
    if (period == 0U) {
        return false;
    }

    /* 无符号减法 ⇒ 跨 2^32 回绕仍然得到正确的相位（docs/06） */
    uint32_t elapsed = now_ms - ctx->phase_ms;
    if (elapsed >= period) {
        /* 已经越过一个完整周期：把起点推进到当前周期内。
         * ⚠️ 用取模而不是 `phase += period` 一次 —— 调用间隔远大于周期时
         *    （例如任务被长时间阻塞），一次加法推不到位，相位会一直落后。 */
        ctx->phase_ms = now_ms - (elapsed % period);
        elapsed = elapsed % period;
    }

    return elapsed < ctx->on_ms;
}
