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
     * 与 op_meas 的 Upp 重建同口径（sin），只是横轴换成 cycles 个周期。 */
    const double t_end = (double)cycles / (double)r->f1_hz;
    for (int j = 0; j < out_n; j++) {
        const double t = t_end * (double)j / (double)out_n;
        double w = 0.0;
        for (int i = 0; i < r->n_peaks; i++) {
            int m = (int)(r->peak[i].freq_hz / r->f1_hz + 0.5f);
            if (m < 1) {
                m = 1;
            }
            w += (double)r->peak[i].amp_mv *
                 sin(2.0 * 3.14159265358979323846 * (double)m * (double)r->f1_hz * t +
                     (double)r->peak[i].phase_rad);
        }
        y_out[j] = (float)w;
    }
    return out_n;
}
