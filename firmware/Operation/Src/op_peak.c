/* ============================================================================
 *  op_peak.c —— 找峰 + 频率精化 + 单点 DFT · 见 op_peak.h
 * ==========================================================================*/
#include "op_peak.h"

#include <math.h>

#include "op_dsp.h" /* OP_DSP_CG —— 幅值归一要同一张窗的相干增益 */

/* 静态默认值 = 与 project_config.h 的 APP_PEAK_* **同值**的兜底：
 * 生产路径由 app_engine_init() 显式 op_peak_init() 注入，不读它；
 * PC 单测（Test/test_op_meas.c）不调 init，靠这份默认跑 ——
 * ⚠️ 改 APP_PEAK_* 时必须同步改这里（Operation 层不许 include 配置头）。 */
static op_peak_params_t s_p = {
    9000.0f, 502000.0f, 2.5f,
};

/* 递推重置步长：误差有界的最小保险丝（见 op_peak.h 说明）。256 步内
 * f32 递推的相位漂移 ~1e-4 rad，对 5mV 判据而言是噪声级。 */
#define PEAK_TWIDDLE_RESET 256U

void op_peak_init(const op_peak_params_t *p) {
    if (p != NULL) {
        s_p = *p;
    }
}

/* 单点 DFT：X(f) = Σ xw[i]·e^{−j2πf·i/fs}，实部/虚部经递推旋转因子。
 * ⚠️ 与 verify_dsp 的 np.dot(xw, exp(−2jπf·n/fs)) 逐点同口径 —— 差异只在
 *    f32 舍入（复算容差内）。
 * 累加用 **Kahan 补偿**：朴素 float 求和 4096 项的误差可达 mV 级（Σ|xw|~1e5
 * × n·eps），会吃掉 5mV 判据的预算；double 累加在 F4 上是软浮点（~35ms）。
 * Kahan 在硬件 FPU 上 4 flops/项 ≈ 0.5ms/频点，误差 ~eps·|Σ| ≈ 0.01mV。 */
static void dft_at(const float *xw, uint32_t n, float fs_hz, float f_hz,
                   float *re_out, float *im_out) {
    /* 旋转步进：e^{−j2πf/fs}
     * ★ 2026-10-14 八轮（P3.3）：`cos/sin`(double) → `cosf/sinf`(float)。本工程只有
     *   单精度 FPU ⇒ double 三角函数是软浮点（~500~1500 周期/次），而这里每帧要调
     *   384 次（3 峰 × 4 次 dft_at × 16 次锚点重置 × 2）。角度仍用 double 算
     *   （i 大时精度更好）、只在调用处转 float：锚点角的 float 误差 ~1e-4 rad
     *   只影响 1/256 的样本 ⇒ 对 |X| 的影响 ~1e-7，远在 5mV 判据之内。 */
    const double w = -2.0 * 3.14159265358979323846 * (double)f_hz / (double)fs_hz;
    const float step_re = cosf((float)w);
    const float step_im = sinf((float)w);

    float re = 0.0f;
    float im = 0.0f;
    float re_c = 0.0f;
    float im_c = 0.0f;
    float c_re = 1.0f;
    float c_im = 0.0f;
    for (uint32_t i = 0; i < n; i++) {
        if ((i % PEAK_TWIDDLE_RESET) == 0U) {
            const double th = w * (double)i; /* 锚点重置：误差不跨步累积 */
            c_re = cosf((float)th);
            c_im = sinf((float)th);
        }
        {
            const float prod_re = xw[i] * c_re;
            const float prod_im = xw[i] * c_im;
            /* Kahan：t = sum+y；sum = t + (comp−…) —— 实部虚部各一套补偿 */
            const float tre = prod_re - re_c;
            const float nre = re + tre;
            re_c = (nre - re) - tre;
            re = nre;
            const float tim = -prod_im - im_c;
            const float nim = im + tim;
            im_c = (nim - im) - tim;
            im = nim;
        }
        /* 递推到下一点 */
        const float nr = c_re * step_re - c_im * step_im;
        const float ni = c_re * step_im + c_im * step_re;
        c_re = nr;
        c_im = ni;
    }
    *re_out = re;
    *im_out = im;
}

int op_peak_run(const float *xw, const float *mag, uint32_t n, float fs_hz,
                const op_cal_table_t *cal, meas_peak_t *out, int cap) {
    if (xw == NULL || mag == NULL || out == NULL || cap <= 0 ||
        n != (uint32_t)OP_DSP_N || fs_hz <= 0.0f) {
        return -1;
    }

    const float df = fs_hz / (float)n; /* 栅格间隔 488.28Hz */
    const int k_lo = (int)(s_p.f_lo_hz / df);
    const int k_hi = (int)((s_p.f_hi_hz / df) < (float)(n / 2U - 1U)
                               ? (s_p.f_hi_hz / df)
                               : (float)(n / 2U - 1U));

    /* ② log 地板：max(mag)×1e−9（verify_dsp 同值）—— 只防 log(0)，不改变
     *    真峰的抛物线形状。 */
    float mag_max = 0.0f;
    for (int k = k_lo; k <= k_hi; k++) {
        if (mag[k] > mag_max) {
            mag_max = mag[k];
        }
    }
    const float floor_v = (mag_max > 1e-30f ? mag_max : 1e-30f) * 1e-9f;

    int n_pk = 0;
    int k = k_lo + 1;
    while (k < k_hi - 3 && n_pk < cap) {
        /* 局部极大（左≤右< —— verify_dsp 的 mag[k]>=mag[k-1] && mag[k]>mag[k+1]） */
        if (mag[k] >= mag[k - 1] && mag[k] > mag[k + 1]) {
            /* ① 栅格预筛：粗幅值 = 2·mag/(N·CG)。借能量的伪峰在这里被挡住，
             *    省掉它后面 3 次精化点积。 */
            if (2.0f * mag[k] / ((float)n * OP_DSP_CG) >= s_p.gate_mv) {
                /* log 域三点抛物线 → 顶点偏移 d ∈ [−0.5, 0.5]（③ 限幅） */
                const float a = logf(mag[k - 1] > floor_v ? mag[k - 1] : floor_v);
                const float b = logf(mag[k] > floor_v ? mag[k] : floor_v);
                const float c = logf(mag[k + 1] > floor_v ? mag[k + 1] : floor_v);
                const float den = a - 2.0f * b + c;
                float d = 0.0f;
                if (den != 0.0f) {
                    d = 0.5f * (a - c) / den;
                }
                if (d > 0.5f) {
                    d = 0.5f;
                }
                if (d < -0.5f) {
                    d = -0.5f;
                }
                const float fc0 = ((float)k + d) * df;

                /* 三点 |X| 抛物线精化：fc0 ± Δf/8（复算③：±Δf/8 三点最优） */
                float m3[3];
                for (int j = 0; j < 3; j++) {
                    float re, im;
                    dft_at(xw, n, fs_hz, fc0 + ((float)j - 1.0f) * df / 8.0f, &re, &im);
                    m3[j] = sqrtf(re * re + im * im);
                }
                const float den2 = m3[0] - 2.0f * m3[1] + m3[2];
                float d2 = 0.0f;
                if (den2 != 0.0f) {
                    d2 = 0.5f * (m3[0] - m3[2]) / den2;
                }
                if (d2 > 0.5f) {
                    d2 = 0.5f;
                }
                if (d2 < -0.5f) {
                    d2 = -0.5f;
                }
                const float f_c = fc0 + d2 * df / 8.0f;

                /* 单点 DFT 读数（口径①：幅值在精化频率上，不是栅格直读） */
                float re, im;
                dft_at(xw, n, fs_hz, f_c, &re, &im);
                float amp = 2.0f * sqrtf(re * re + im * im) / ((float)n * OP_DSP_CG);
                float phi = atan2f(im, re);

                /* 标定修正（④ 终判前做 —— 与 verify_dsp 一致：先修正后终判） */
                float g = 1.0f, p_corr = 0.0f;
                op_cal_lookup(cal, f_c, &g, &p_corr);
                if (g > 0.0f) {
                    amp /= g;
                }
                phi -= p_corr;

                /* sin 载波补偿：DFT 对 A·sin(2πft+φ0) 的读数相位 = φ0 − π/2
                 * （Σ sin(θ)e^{−jθ} = −j·N'·CG/2，见复算）。补回 π/2 后输出的
                 * 相位就是**源端口径**（docs/01 §5 ②："测得相位≈源端相位"），
                 * op_meas/op_wave 的 sin 重建拿去直接用 —— 没有这一步，重建
                 * 会落在 cos 口径上，Upp 偏 +25mV（复算④ 点名警告的那个数）。 */
                phi += 1.5707963267948966f;

                /* ④ 终判：精化后仍过门限 + 频带内 */
                if (amp >= s_p.gate_mv && f_c >= s_p.f_lo_hz && f_c <= s_p.f_hi_hz) {
                    out[n_pk].freq_hz = f_c;
                    out[n_pk].amp_mv = amp;
                    out[n_pk].phase_rad = phi;
                    n_pk++;
                }
            }
            k += 5; /* 命中（或预筛命中形态）跳过本主瓣 —— BH4 −6dB 半宽 ~2.2 bin */
        } else {
            k++;
        }
    }
    return n_pk;
}
