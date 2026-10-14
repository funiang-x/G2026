/* ============================================================================
 *  op_wave.c —— 峰表 → 波形像素列 · 见 op_wave.h
 * ==========================================================================*/
#include "op_wave.h"

#include <math.h>

int op_wave_build(const meas_result_t *r, int cycles, float *y_out, int out_n) {
    if (r == NULL || y_out == NULL || out_n <= 0 ||
        (cycles != 1 && cycles != 3) || r->n_peaks == 0U || r->f1_hz <= 0.0f) {
        return -1;
    }

    /* 横轴：t ∈ [0, cycles/f1)。逐点叠各分量 A·sin(2π·m·f1·t+φ) ——
     * 与 op_meas 的 Upp 重建同口径（sin），只是横轴换成 cycles 个周期。
     *
     * ★ 2026-10-14 八轮（P3.3）：**double → float + `sinf`**，并把级次 m 与角频率
     *   ω_k = 2π·m·f1 挪到点循环**外面**（原来每点每分量都要重算一次浮点除法与乘法）。
     *   为什么敢换：本工程只有单精度 FPU（`-mfpu=fpv4-sp-d16`）⇒ 所有 double 都是
     *   软浮点，而这里每次刷新 ≤800 次 sin（UI 5~10Hz ⇒ ≤8000 次/s）；判据 5mV/100mV
     *   = 5e-5 远宽于 `sinf` 的 ~1e-7 相对误差。 */
    const float two_pi = 6.28318530717958647692f;
    const float t_end  = (float)cycles / r->f1_hz;
    const int   n      = (int)r->n_peaks;
    float       wk[MEAS_MAX_PEAKS]; /* ω_k */
    float       ak[MEAS_MAX_PEAKS]; /* A_k */
    float       ph[MEAS_MAX_PEAKS]; /* φ_k */

    for (int i = 0; i < n; i++) {
        int m = (int)(r->peak[i].freq_hz / r->f1_hz + 0.5f);
        if (m < 1) {
            m = 1;
        }
        wk[i] = two_pi * (float)m * r->f1_hz;
        ak[i] = r->peak[i].amp_mv;
        ph[i] = r->peak[i].phase_rad;
    }
    for (int j = 0; j < out_n; j++) {
        const float t = t_end * (float)j / (float)out_n;
        float       w = 0.0f;
        for (int i = 0; i < n; i++) {
            w += ak[i] * sinf(wk[i] * t + ph[i]);
        }
        y_out[j] = w;
    }
    return out_n;
}
