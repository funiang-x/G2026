/* ============================================================================
 *  op_spec.c —— 峰表 → 谱线几何 · 见 op_spec.h
 * ==========================================================================*/
#include "op_spec.h"

#include <math.h>

int op_spec_build(const meas_result_t *r, op_spec_line_t *out, int cap) {
    /* ⚠️ 2026-10-15：`cap` 不再要求 ≥ MEAS_MAX_PEAKS —— UI 只显示前 `UI_SPEC_LINES`(5)
     *    根，而峰表容量已升到 16（那是给波形重建用的）。只要 cap > 0 即可。 */
    if (r == NULL || out == NULL || cap <= 0 || r->n_peaks == 0U) {
        return -1;
    }

    /* log10 横轴铺满 [10k, 500k]（约 1.7 个十倍程） */
    const float lx_lo = log10f(10000.0f);
    const float lx_hi = log10f(500000.0f);
    const float lx_span = lx_hi - lx_lo;

    float amp_max = 0.0f;
    for (int i = 0; i < r->n_peaks; i++) {
        if (r->peak[i].amp_mv > amp_max) {
            amp_max = r->peak[i].amp_mv;
        }
    }
    if (amp_max <= 0.0f) {
        return -1;
    }

    for (int i = 0; i < r->n_peaks; i++) {
        float lx = log10f(r->peak[i].freq_hz);
        if (lx < lx_lo) {
            lx = lx_lo; /* 峰理论上不会出带（op_peak 终判过），防御钳位 */
        }
        if (lx > lx_hi) {
            lx = lx_hi;
        }
        out[i].x01 = (lx - lx_lo) / lx_span;
        out[i].len01 = r->peak[i].amp_mv / amp_max;
    }
    return r->n_peaks;
}
