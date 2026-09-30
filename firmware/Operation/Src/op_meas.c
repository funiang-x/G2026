/* ============================================================================
 *  op_meas.c —— 口径四条 · 见 op_meas.h
 * ==========================================================================*/
#include "op_meas.h"

#include <math.h>

/* 重建一个基波周期用的采样点数 —— **必须跟着最高分量阶次 m_max 走**。
 *
 * ★ 2026-09-28 修正（实测发现）：抽点密度 = P/m_max 点/周期，ptp 的格点误差
 *   约为 (1−cos(π·m_max/P))。**固定 512** 在 m_max≤8 时够用（0.03%），
 *   但 m_max=50（f1=10k 基波 + 500k 分量）时只有 10.2 点/周期 ——
 *   实测 Upp 偏 **−1.75mV**（判据 5mV 的 35%，且是系统性误差不是噪声）。
 *   旧注释"512 点的 ptp 与 8192 点差 <0.01mV"只在低阶次下成立，是错的。
 *   ⇒ 取 max(512, 32·m_max)：m≤16 时**零额外开销**（仍是 512 点），
 *     大阶次自动加密（m=50 → 1600 点）。上限 4096 兜住耗时。
 *   ⚠️ 这改的是**实现**不是口径（口径仍是"sin 重建取 max−min"，docs/01 §5 ②）。 */
#define OP_MEAS_REBUILD_MIN_PTS 512U
#define OP_MEAS_REBUILD_PTS_PER_HARMONIC 32U
#define OP_MEAS_REBUILD_MAX_PTS 4096U

void op_meas_build(const meas_peak_t *peaks, int n_peaks, uint32_t seq,
                   uint8_t scene, meas_result_t *out) {
    if (out == NULL) {
        return;
    }
    out->seq = seq;
    out->scene = scene;
    out->n_peaks = 0U;
    out->f1_hz = 0.0f;
    out->upp_mv = 0.0f;
    out->urms_mv = 0.0f;
    for (int i = 0; i < MEAS_MAX_PEAKS; i++) {
        out->peak[i].freq_hz = 0.0f;
        out->peak[i].amp_mv = 0.0f;
        out->peak[i].phase_rad = 0.0f;
    }
    if (peaks == NULL || n_peaks <= 0) {
        return; /* 未检出：全 0，UI 显示占位 */
    }

    /* 拷贝有效峰（cap 由 op_peak 的调用保证 ≤ MEAS_MAX_PEAKS，这里再防御一次） */
    int n = n_peaks;
    if (n > MEAS_MAX_PEAKS) {
        n = MEAS_MAX_PEAKS;
    }
    for (int i = 0; i < n; i++) {
        out->peak[i] = peaks[i];
    }
    out->n_peaks = (uint8_t)n;

    /* ④ 基频 = 最低峰（op_peak 输出升序，peaks[0] 即最低 —— 但防御起见再扫） */
    float f1 = peaks[0].freq_hz;
    for (int i = 1; i < n; i++) {
        if (peaks[i].freq_hz < f1) {
            f1 = peaks[i].freq_hz;
        }
    }
    out->f1_hz = f1;

    /* ③ Urms = √(ΣA_k²/2) —— 频域交流口径，天然只在分析带内（不含 u_J） */
    double sum_a2 = 0.0;
    for (int i = 0; i < n; i++) {
        sum_a2 += (double)peaks[i].amp_mv * (double)peaks[i].amp_mv;
    }
    out->urms_mv = (float)sqrt(sum_a2 / 2.0);

    /* ② Upp = sin 口径重建一个基波周期 → max−min。
     *    m_k = round(f_k/f1)（级次）—— 相位用各自测得的 φ_k（源端口径）。
     *    ⚠️ 重建点数按最高阶次自适应（见文件顶的说明），否则大阶次信号会偏。 */
    int m_k[MEAS_MAX_PEAKS];
    int m_max = 1;
    for (int i = 0; i < n; i++) {
        int m = (int)(peaks[i].freq_hz / f1 + 0.5f);
        if (m < 1) {
            m = 1;
        }
        m_k[i] = m;
        if (m > m_max) {
            m_max = m;
        }
    }
    uint32_t pts = OP_MEAS_REBUILD_MIN_PTS;
    {
        const uint32_t need = OP_MEAS_REBUILD_PTS_PER_HARMONIC * (uint32_t)m_max;
        if (need > pts) {
            pts = need;
        }
        if (pts > OP_MEAS_REBUILD_MAX_PTS) {
            pts = OP_MEAS_REBUILD_MAX_PTS;
        }
    }

    double w_max = -1e30;
    double w_min = 1e30;
    for (uint32_t j = 0; j < pts; j++) {
        const double t = (double)j / ((double)f1 * (double)pts);
        double w = 0.0;
        for (int i = 0; i < n; i++) {
            w += (double)peaks[i].amp_mv *
                 sin(2.0 * 3.14159265358979323846 * (double)m_k[i] * (double)f1 * t +
                     (double)peaks[i].phase_rad);
        }
        if (w > w_max) {
            w_max = w;
        }
        if (w < w_min) {
            w_min = w;
        }
    }
    out->upp_mv = (float)(w_max - w_min);
}
