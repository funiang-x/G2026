#ifndef OP_WAVE_H
#define OP_WAVE_H
/* ============================================================================
 *  op_wave —— 峰表 → 波形页的像素列（Operation 层，纯逻辑）
 *
 *  docs/01 §7：波形页画 **1 或 3 个完整周期**，用峰表重建绘制（不是画原始
 *  采样）—— f1=500kHz 时单帧每周期只有 4 个实测点，重建是唯一可行画法；
 *  且重建波形整数周期对齐、零噪声、有干扰时天然干净（题面要的正是"稳定
 *  显示 1 或 3 个周期"）。
 *
 *  输出 y[]（mV，屏幕纵向由 UI 映射），横轴均匀抽点。纯逻辑，PC 可测。
 * ==========================================================================*/
#include <stdint.h>

#include "meas_result.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 重建波形到 y_out（mV）。cycles=1 或 3；横轴覆盖 cycles/f1 秒，均匀 out_n 点。
 * 返回写入点数（=out_n）；-1 参数非法 / 结果体里没有峰。 */
int op_wave_build(const meas_result_t *r, int cycles, float *y_out, int out_n);

#ifdef __cplusplus
}
#endif
#endif /* OP_WAVE_H */
