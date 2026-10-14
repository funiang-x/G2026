#ifndef OP_MEAS_H
#define OP_MEAS_H
/* ============================================================================
 *  op_meas —— 口径四条的落点：峰表 → MeasResult（Operation 层，纯逻辑）
 *
 *  口径（docs/01 §5，**任何页面只读这里**，verify_dsp 的 scenario() 同款）：
 *  ① 幅值 = 精化频率上的单点 DFT（op_peak 已做）
 *  ② Upp = sin 口径重建一个基波周期 → max−min。禁止 2ΣA（偏 +65mV）、
 *     禁止 cos 口径（偏 +25mV）—— 复算④ 的两个翻车口径；
 *  ③ Urms = √(ΣA_k²/2)，交流频域口径 —— 时域直接算会把 u_J 算进去；
 *  ④ 频率 = 精化 f_c，最低峰即基波；级次 m_k = round(f_k/f1) 只用于 Upp 重建。
 *  ⑤ THD = √(Σ_{k≥2} A_k²) / A1（A1 = 基波幅值；n_peaks<2 时为 0）。
 *
 *  纯逻辑：输入峰表，输出 meas_result_t（Common），PC 可单测。
 * ==========================================================================*/
#include <stdint.h>

#include "meas_result.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 峰表 → 结果体。n_peaks 由 op_peak_run 返回；seq/scene 是 Task 层的上下文。
 * n_peaks==0 时仍产出（f1/upp/urms 填 0）—— UI 显示"未检出"而不是空转。 */
void op_meas_build(const meas_peak_t *peaks, int n_peaks, uint32_t seq,
                   uint8_t scene, meas_result_t *out);

#ifdef __cplusplus
}
#endif
#endif /* OP_MEAS_H */
