/* ============================================================================
 *  test_op_meas —— DSP 全链端到端 PC 单测（op_dsp → op_peak → op_meas）
 *
 *  覆盖 verify_dsp.py 的 S1~S12 场景，外加自研的 S13（docs/01 §12 的要求）。理想链路：
 *  纯正弦和 → 12bit ADC 码（含量化）→ 全链 → 与真值比。**无标定仿真** ——
 *  那是 verify_dsp 的硬件仿真内容，PC 单测钉的是算法链本身
 *  （复算报告的算法链误差 0.031mV，容差放宽到 2mV / 20Hz 仍有 60× 余量）。
 *
 *  ---- S13 是什么（2026-09-28 新增）----
 *  专钉 **Upp 重建的抽点密度**：f1=10k + 500k 分量 ⇒ m_max=50，固定 512 点/周期
 *  时只有 10.2 点/周期，实测 Upp 偏 −1.75mV。修复见 op_meas.c 顶部说明。
 *
 *  ---- 干扰场景（S5~S12）怎么建 ----
 *  把 u_J **以源端幅度加在 f_J 上**（f_J ≥ 1MHz），前面串一个 8 阶巴特沃斯
 *  fc=800k 的幅频响应 —— 与 verify_dsp.py 的 synth() 同式。≥fs/2 的 f_J 由
 *  **采样本身**折回带内（镜像），这正是要测的那条路径。判据有两条：
 *  ① 镜像不得变成一根伪谱线（检出峰数 = 真分量数）；
 *  ② 各真分量幅值误差 ≤ 2mV（含 S9「镜像恰好落在真分量上」的最坏情形）。
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

/* u_J 源端峰值（mVpk）：题面 200mVpp。与 verify_dsp.py 的 UJ_PEAK 同值。 */
#define UJ_PEAK_MV 100.0

/* 抗混叠低通的角频率（Hz）：8 阶巴特沃斯 fc=800k，硬件定稿值。 */
#define LPF_FC_HZ 800000.0

/* 静态缓冲：host 上无所谓；大小与 MCU 侧一致，防止"测试悄悄比板子宽裕"。
 * ⚠️ s_fft 是 **2N** —— CFFT 的交织复数缓冲（2026-09-28：原先给 N，
 *    比 MCU 侧更早暴露"写穿"；host_dft 也只用到前 N+2 个，所以那 2 个越界
 *    恰好落在 s_mag 上、又被后续写入盖掉，测不出来。见 docs/01 §5.1）。 */
static uint16_t s_raw[OP_DSP_N];
static float    s_xw[OP_DSP_N];
static float    s_fft[2 * OP_DSP_N];
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

/* ---- 干扰场景的两个助手 ---- */

/* 8 阶巴特沃斯低通幅频响应：|H(f)| = 1/√(1+(f/fc)^16)。
 * 与 verify_dsp.py 的 bw_response()、hardware/verify_afe_v2_report.md §1 同式 ——
 * 前端模型**只有这一处**，不硬编码衰减数字（改 fc 只需改 LPF_FC_HZ）。 */
static double bw8_mag(double f_hz) {
    const double r = f_hz / LPF_FC_HZ;
    return 1.0 / sqrt(1.0 + pow(r, 16.0));
}

/* 真值（与 verify_dsp.py 的 true_params 同式）：
 * Upp = 8192 点整周期重建的 max−min；Urms = √(ΣA²/2)（只算真分量，不含 u_J）。 */
static void truth(const tone_t *tones, int n_tones, double *upp, double *urms) {
    const double f1 = tones[0].f;
    double wmax = -1e30, wmin = 1e30, u = 0.0;
    for (int j = 0; j < 8192; j++) {
        const double t = (double)j / (8192.0 * f1);
        double w = 0.0;
        for (int k = 0; k < n_tones; k++) {
            w += tones[k].a * sin(2.0 * 3.14159265358979323846 * tones[k].f * t + tones[k].ph);
        }
        if (w > wmax) {
            wmax = w;
        }
        if (w < wmin) {
            wmin = w;
        }
    }
    for (int k = 0; k < n_tones; k++) {
        u += tones[k].a * tones[k].a / 2.0;
    }
    *upp = wmax - wmin;
    *urms = sqrt(u);
}

/* 干扰场景：真分量 + 一个 u_J 音（源端幅度 ×|H(f_J)|，f_J 的镜像由采样折回）。
 * n_pk 必须**等于真分量数** —— 镜像不算谱线；真分量误差 ≤2mV（含 S9 最坏）。 */
static void scenario_uj(const char *name, const tone_t *tones, int n_tones, double uj_f,
                        double upp_true, double urms_true) {
    tone_t ext[8];
    meas_result_t r;

    TEST_CASE(name);
    for (int k = 0; k < n_tones; k++) {
        ext[k] = tones[k];
    }
    ext[n_tones].f = uj_f;
    ext[n_tones].a = UJ_PEAK_MV * bw8_mag(uj_f); /* 前端 LPF 之后的残留 */
    ext[n_tones].ph = 0.0;

    const int n_pk = run_chain(ext, n_tones + 1, &r);
    CHECK_EQ(n_pk, n_tones);
    if (n_pk != n_tones) {
        return;
    }
    CHECK(fabs(r.f1_hz - tones[0].f) < 20.0);
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

    /* S13 高次谐波重建（**2026-09-28 新增**）：f1=10k + 500k 分量 ⇒ m_max=50。
     * 这条专钉 Upp 重建的**抽点密度**：op_meas 原先固定 512 点/基波周期，
     * m=50 时只有 10.2 点/周期 → 实测 Upp 偏 **−1.75mV**（判据 5mV 的 35%，
     * 系统性误差）。已改为按 m_max 自适应（max(512, 32·m_max)，见 op_meas.c）。
     * 本用例在修复前必失败、修复后 |ΔUpp|<0.01mV —— 是那个修复的回归保护。 */
    {
        const tone_t t5[] = {{10000.0, 60.0, 0.0}, {20000.0, 30.0, 0.0}, {500000.0, 20.0, 0.0}};
        double wmax = -1e30, wmin = 1e30, urms = 0.0;
        for (int j = 0; j < 8192; j++) {
            const double t = (double)j / (8192.0 * 10000.0);
            double w = t5[0].a * sin(2.0 * 3.14159265358979323846 * 10000.0 * t) +
                       t5[1].a * sin(2.0 * 3.14159265358979323846 * 20000.0 * t) +
                       t5[2].a * sin(2.0 * 3.14159265358979323846 * 500000.0 * t);
            if (w > wmax) wmax = w;
            if (w < wmin) wmin = w;
        }
        for (unsigned k = 0; k < 3; k++) {
            urms += t5[k].a * t5[k].a / 2.0;
        }
        scenario("S13 高次谐波重建 f1=10k m=(1,2,50)", t5, 3, wmax - wmin, sqrt(urms));
    }

    /* ---- S5~S12：单频干扰 u_J（题面第 3 项）----
     * 真分量固定为 10.5k/60 + 31.5k/30 + 42k/20（与 S1 同，便于对照）。
     * 镜像落点/残留见 hardware/verify_afe_v2_report.md §3；这里由 bw8_mag 现算。 */
    {
        const tone_t ts[] = {{10500.0, 60.0, 0.0}, {31500.0, 30.0, 0.0}, {42000.0, 20.0, 0.0}};
        double upp, urms;
        truth(ts, 3, &upp, &urms);
        scenario_uj("S5 干扰 u_J=1.0MHz（Nyquist 整）", ts, 3, 1.0e6, upp, urms);
        scenario_uj("S6 干扰 u_J=1.2MHz", ts, 3, 1.2e6, upp, urms);
        scenario_uj("S7 干扰 u_J=1.49MHz（镜像带外边缘）", ts, 3, 1.49e6, upp, urms);
        scenario_uj("S8 干扰 u_J=1.5MHz（折叠带起点·最坏）", ts, 3, 1.5e6, upp, urms);
        scenario_uj("S10 干扰 u_J=2.5MHz（二次折叠）", ts, 3, 2.5e6, upp, urms);
        scenario_uj("S11 干扰 u_J=3.0MHz", ts, 3, 3.0e6, upp, urms);
        scenario_uj("S12 干扰 u_J=5.0MHz", ts, 3, 5.0e6, upp, urms);
    }

    /* S9：u_J=1.5MHz 的镜像**恰好落在真实存在的 500k 分量上** —— 全带最坏点。
     * 同频无法靠频率分离，误差上界 = 镜像残留 0.655mV（<2mV 容差，判据 5mV 的 13%）。
     *
     * ⚠️ 与 verify_dsp.py 的 S9 不同：那边用 4 个分量（10.5k/31.5k/42k/500k），
     *    而器件峰表上限 `MEAS_MAX_PEAKS = 3`（题面"基波 + 1~2 个谐波"）。
     *    这里取 3 分量版，且 500k 取 10k 的整数次谐波（m=50）—— 重建口径精确，
     *    不被 Upp 重建的谐波取整误差掩盖。 */
    {
        const tone_t ts9[] = {{10000.0, 60.0, 0.0}, {20000.0, 30.0, 0.0}, {500000.0, 20.0, 0.0}};
        double upp, urms;
        truth(ts9, 3, &upp, &urms);
        scenario_uj("S9 干扰 u_J=1.5MHz + 真有 500k 分量（最坏·3 分量版）", ts9, 3, 1.5e6, upp,
                    urms);
    }

    /* S14 上电自检的判据（钉住 `app_engine_selftest()` 用的那几个数）。
     * 自检在 MCU 上合成"100mV 单音落在第 100 个 bin"→ 全链 → 校验频率/幅值。
     * 这里用**同样的合成与判据**在 PC 上跑一遍：
     *   · 它证明自检的**判据本身是对的**（期望频率/幅值/门限没写错）；
     *   · 但它**不覆盖 MCU 的 arm_cfft_f32**（host 走朴素 DFT 替身）——
     *     FFT 实例/表装配错只能靠上电自检在板子上抓。 */
    {
        const uint32_t k0 = 100U;
        const double   amp = 100.0;
        const double   f_exp = (double)FS_HZ * (double)k0 / (double)OP_DSP_N;
        const double   df = (double)FS_HZ / (double)OP_DSP_N;
        const tone_t   ts14[] = {{48828.125, amp, 0.0}}; /* fs·100/4096 = 48.828125kHz */
        meas_result_t  r;
        int            n_pk;

        TEST_CASE("S14 上电自检判据（bin100 / 100mV）");
        CHECK(fabs(ts14[0].f - f_exp) < 0.01); /* 频率对得上才谈得上判据 */
        n_pk = run_chain(ts14, 1, &r);
        CHECK_EQ(n_pk, 1);
        if (n_pk == 1) {
            CHECK(fabs((double)r.peak[0].freq_hz - f_exp) < df / 4.0); /* 自检的频率门限 */
            CHECK(fabs((double)r.peak[0].amp_mv - amp) < 1.0);         /* 自检的幅值门限 */
        }
    }

    /* S15 FFT 输入装配（钉住 `op_dsp_pack_real_to_cfft`）。
     * ★ 这条专钉 2026-09-28 查出的那处缺陷：装配必须是**逐点交织 + 虚部清零**，
     *   不能把样本连续放（那样等价于"偶数样本当实部、奇数样本当虚部"）。
     *   它是"把 MCU 独有步骤搬进 Operation"的直接受益者 —— 在 PC 上就能验。
     *   ⚠️ 若把 pack 改回 `cfft_buf[i] = xw[i]`，本条必失败。 */
    {
        static float s_packed[2 * OP_DSP_N]; /* 32KB：必须 static，不能放测试栈 */
        int bad_re = 0;
        int bad_im = 0;

        TEST_CASE("S15 FFT 输入装配（逐点交织 + 虚部清零）");
        for (uint32_t i = 0; i < OP_DSP_N; i++) {
            s_xw[i] = (float)i; /* 值 = 下标，便于区分"交织"与"连续" */
        }
        op_dsp_pack_real_to_cfft(s_xw, s_packed, OP_DSP_N);
        for (uint32_t i = 0; i < OP_DSP_N; i++) {
            if (s_packed[2U * i] != s_xw[i]) {
                bad_re++;
            }
            if (s_packed[2U * i + 1U] != 0.0f) {
                bad_im++;
            }
        }
        CHECK_EQ(bad_re, 0); /* 实部逐点对上（连续存放会让 i>0 全错） */
        CHECK_EQ(bad_im, 0); /* 虚部恒 0（实序列） */
        op_dsp_pack_real_to_cfft(NULL, s_packed, OP_DSP_N); /* 参数防御：不应崩 */
        op_dsp_pack_real_to_cfft(s_xw, NULL, OP_DSP_N);
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
