/* ============================================================================
 *  test_op_meas —— DSP 全链端到端 PC 单测（op_dsp → op_peak → op_meas）
 *
 *  用 verify_dsp.py 的 S1~S4 场景做期望值（docs/01 §12 的要求）。理想链路：
 *  纯正弦和 → 12bit ADC 码（含量化）→ 全链 → 与真值比。**无前端 LPF、无标定
 *  仿真** —— 那两项是 verify_dsp 的硬件仿真内容，PC 单测钉的是算法链本身
 *  （复算报告的算法链误差 0.031mV，容差放宽到 2mV / 20Hz 仍有 60× 余量）。
 *
 *  FFT 用 host 朴素 DFT 替身（OP_DSP_HOST_FFT，见 op_dsp.h）—— CMSIS 的 .a
 *  是 M4F 机器码，PC 链不了；窗/找峰/精化/口径与 MCU 是同一份代码。
 * ==========================================================================*/
#include <math.h>
#include <stdio.h>

#include "meas_result.h"
#include "op_dsp.h"
#include "op_meas.h"
#include "op_peak.h"
#include "test_util.h"

#define FS_HZ 2000000.0f
#define LSB_MV (3300.0f / 4096.0f / 4.0f) /* 输入端 LSB，verify_dsp 的 LSB_IN */

/* 静态缓冲：host 上无所谓；大小与 MCU 侧一致，防止"测试悄悄比板子宽裕" */
static uint16_t s_raw[OP_DSP_N];
static float    s_xw[OP_DSP_N];
static float    s_fft[OP_DSP_N];
static float    s_mag[OP_DSP_MAG_LEN];

typedef struct {
    double f;
    double a;
    double ph;
} tone_t;

/* 合成到 ADC 码域：偏置 2048 码 + 输入端 mV/LSB，四舍五入 = 12bit 量化 */
static void synth_code(const tone_t *tones, int n_tones, uint16_t *out) {
    for (uint32_t i = 0; i < OP_DSP_N; i++) {
        const double t = (double)i / FS_HZ;
        double mv = 0.0;
        for (int k = 0; k < n_tones; k++) {
            mv += tones[k].a * sin(2.0 * 3.14159265358979323846 * tones[k].f * t +
                                   tones[k].ph);
        }
        const double code = 2048.0 + mv / (double)LSB_MV;
        long c = lround(code);
        if (c < 0) {
            c = 0;
        }
        if (c > 4095) {
            c = 4095;
        }
        out[i] = (uint16_t)c;
    }
}

/* 全链跑一遍，返回峰数（-1 链路失败） */
static int run_chain(const tone_t *tones, int n_tones, meas_result_t *r) {
    synth_code(tones, n_tones, s_raw);
    op_dsp_init(FS_HZ, LSB_MV);
    if (op_dsp_run(s_raw, OP_DSP_N, s_xw, s_fft, s_mag) != 0) {
        return -1;
    }
    meas_peak_t pk[MEAS_MAX_PEAKS];
    const int n_pk =
        op_peak_run(s_xw, s_mag, OP_DSP_N, FS_HZ, NULL, pk, MEAS_MAX_PEAKS);
    if (n_pk < 0) {
        return -1;
    }
    op_meas_build(pk, n_pk, 1U, MEAS_SCENE_UA, r);
    return n_pk;
}

static void check_tone(const meas_result_t *r, double f_true, double a_true) {
    int found = 0;
    for (int i = 0; i < r->n_peaks; i++) {
        if (fabs(r->peak[i].freq_hz - f_true) < 20.0) { /* 精化后 <1Hz，20Hz 很宽 */
            found = 1;
            CHECK(fabs(r->peak[i].amp_mv - a_true) < 2.0); /* 判据 5mV 的 40% */
            break;
        }
    }
    CHECK(found); /* 频率差 >20Hz 也算失败（没找到这根线） */
}

static void scenario(const char *name, const tone_t *tones, int n_tones,
                     double upp_true, double urms_true) {
    meas_result_t r;
    TEST_CASE(name);
    const int n_pk = run_chain(tones, n_tones, &r);
    CHECK_EQ(n_pk, n_tones); /* 防御四条 + 主瓣合并：不多检不少检 */
    if (n_pk != n_tones) {
        return;
    }
    CHECK(fabs(r.f1_hz - tones[0].f) < 20.0); /* 基频 = 最低峰（场景按升序给） */
    CHECK(fabs(r.upp_mv - upp_true) < 2.0);
    CHECK(fabs(r.urms_mv - urms_true) < 2.0);
    for (int k = 0; k < n_tones; k++) {
        check_tone(&r, tones[k].f, tones[k].a);
    }
}

int main(void) {
    /* S1 题面示例：10.5k 基波，m=(1,3,4)。真值口径 = ΣA sin（全零相位），
     * Upp/Urms 用 verify_dsp 的 true_params 同式数值复算（8192 点）。 */
    {
        const tone_t t1[] = {{10500.0, 60.0, 0.0}, {31500.0, 30.0, 0.0}, {42000.0, 20.0, 0.0}};
        double wmax = -1e30, wmin = 1e30, urms = 0.0;
        for (int j = 0; j < 8192; j++) {
            const double t = (double)j / (8192.0 * 10500.0);
            double w = 0.0;
            for (unsigned k = 0; k < sizeof(t1) / sizeof(t1[0]); k++) {
                w += t1[k].a * sin(2.0 * 3.14159265358979323846 * t1[k].f * t + t1[k].ph);
            }
            if (w > wmax) wmax = w;
            if (w < wmin) wmin = w;
        }
        for (unsigned k = 0; k < sizeof(t1) / sizeof(t1[0]); k++) {
            urms += t1[k].a * t1[k].a / 2.0;
        }
        scenario("S1 题面示例 f1=10.5k m=(1,3,4)", t1, 3, wmax - wmin, sqrt(urms));
    }

    /* S2 最小信号（Upp≈50mV：9/11/11） */
    {
        const tone_t t2[] = {{10000.0, 9.0, 0.0}, {20000.0, 11.0, 0.0}, {30000.0, 11.0, 0.0}};
        double wmax = -1e30, wmin = 1e30, urms = 0.0;
        for (int j = 0; j < 8192; j++) {
            const double t = (double)j / (8192.0 * 10000.0);
            double w = t2[0].a * sin(2.0 * 3.14159265358979323846 * 10000.0 * t) +
                       t2[1].a * sin(2.0 * 3.14159265358979323846 * 20000.0 * t) +
                       t2[2].a * sin(2.0 * 3.14159265358979323846 * 30000.0 * t);
            if (w > wmax) wmax = w;
            if (w < wmin) wmin = w;
        }
        for (unsigned k = 0; k < 3; k++) {
            urms += t2[k].a * t2[k].a / 2.0;
        }
        scenario("S2 最小信号 f1=10k m=(1,2,3)", t2, 3, wmax - wmin, sqrt(urms));
    }

    /* S3 高频边缘（250k/500k：f_hi 边界上的峰必须能检出） */
    {
        const tone_t t3[] = {{250000.0, 60.0, 0.0}, {500000.0, 40.0, 0.0}};
        double wmax = -1e30, wmin = 1e30, urms = 0.0;
        for (int j = 0; j < 8192; j++) {
            const double t = (double)j / (8192.0 * 250000.0);
            double w = t3[0].a * sin(2.0 * 3.14159265358979323846 * 250000.0 * t) +
                       t3[1].a * sin(2.0 * 3.14159265358979323846 * 500000.0 * t);
            if (w > wmax) wmax = w;
            if (w < wmin) wmin = w;
        }
        for (unsigned k = 0; k < 2; k++) {
            urms += t3[k].a * t3[k].a / 2.0;
        }
        scenario("S3 高频边缘 f1=250k m=(1,2)", t3, 2, wmax - wmin, sqrt(urms));
    }

    /* S4 全带组合（f1=10k，m=(1,4,8)：40k/80k 靠中带） */
    {
        const tone_t t4[] = {{10000.0, 50.0, 0.0}, {40000.0, 25.0, 0.0}, {80000.0, 25.0, 0.0}};
        double wmax = -1e30, wmin = 1e30, urms = 0.0;
        for (int j = 0; j < 8192; j++) {
            const double t = (double)j / (8192.0 * 10000.0);
            double w = t4[0].a * sin(2.0 * 3.14159265358979323846 * 10000.0 * t) +
                       t4[1].a * sin(2.0 * 3.14159265358979323846 * 40000.0 * t) +
                       t4[2].a * sin(2.0 * 3.14159265358979323846 * 80000.0 * t);
            if (w > wmax) wmax = w;
            if (w < wmin) wmin = w;
        }
        for (unsigned k = 0; k < 3; k++) {
            urms += t4[k].a * t4[k].a / 2.0;
        }
        scenario("S4 全带组合 f1=10k m=(1,4,8)", t4, 3, wmax - wmin, sqrt(urms));
    }

    /* 未检出路径：全零输入 → n_peaks=0，结果体全 0（UI 显示占位不空转） */
    {
        meas_result_t r;
        TEST_CASE("未检出路径");
        const tone_t none[1] = {{0.0, 0.0, 0.0}};
        (void)none;
        for (uint32_t i = 0; i < OP_DSP_N; i++) {
            s_raw[i] = 2048U; /* 直流码：去 DC 后全 0，无峰 */
        }
        op_dsp_init(FS_HZ, LSB_MV);
        CHECK_EQ(op_dsp_run(s_raw, OP_DSP_N, s_xw, s_fft, s_mag), 0);
        meas_peak_t pk[MEAS_MAX_PEAKS];
        const int n_pk = op_peak_run(s_xw, s_mag, OP_DSP_N, FS_HZ, NULL, pk, MEAS_MAX_PEAKS);
        CHECK_EQ(n_pk, 0);
        op_meas_build(pk, n_pk, 7U, MEAS_SCENE_UA, &r);
        CHECK_EQ(r.n_peaks, 0);
        CHECK(r.f1_hz == 0.0f);
        CHECK_EQ(r.seq, 7U);
    }

    /* 参数防御：n 错 / NULL 缓冲 */
    {
        TEST_CASE("参数防御");
        op_dsp_init(FS_HZ, LSB_MV);
        CHECK(op_dsp_run(s_raw, 1024U, s_xw, s_fft, s_mag) == -1);
        CHECK(op_dsp_run(NULL, OP_DSP_N, s_xw, s_fft, s_mag) == -1);
        CHECK(op_peak_run(NULL, s_mag, OP_DSP_N, FS_HZ, NULL, NULL, 3) == -1);
    }

    return test_report();
}
