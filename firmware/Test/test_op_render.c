/* ============================================================================
 *  test_op_render —— op_wave / op_spec 渲染几何单测
 * ==========================================================================*/
#include <math.h>
#include <stdio.h>

#include "meas_result.h"
#include "op_spec.h"
#include "op_wave.h"
#include "test_util.h"

/* 手填一个两分量结果体（10.5k/60mV φ=0 + 31.5k/20mV φ=0.7） */
static void fill_result(meas_result_t *r) {
    r->seq = 1U;
    r->scene = MEAS_SCENE_UA;
    r->n_peaks = 2U;
    r->f1_hz = 10500.0f;
    r->upp_mv = 100.0f; /* 本测试不消费 */
    r->urms_mv = 45.0f;
    r->peak[0].freq_hz = 10500.0f;
    r->peak[0].amp_mv = 60.0f;
    r->peak[0].phase_rad = 0.0f;
    r->peak[1].freq_hz = 31500.0f;
    r->peak[1].amp_mv = 20.0f;
    r->peak[1].phase_rad = 0.7f;
    r->peak[2].freq_hz = 0.0f;
    r->peak[2].amp_mv = 0.0f;
    r->peak[2].phase_rad = 0.0f;
}

int main(void) {
    meas_result_t r;
    fill_result(&r);

    /* ---- 波形：1T/3T 重建 ---- */
    TEST_CASE("波形 1T");
    {
        float y1[320];
        CHECK_EQ(op_wave_build(&r, 1, y1, 320), 320);
        /* 值域 |y| ≤ ΣA = 80mV（不越包络） */
        float ymax = 0.0f;
        for (int j = 0; j < 320; j++) {
            if (fabsf(y1[j]) > ymax) {
                ymax = fabs(y1[j]);
            }
        }
        CHECK(ymax <= 80.5f);
        /* 1T 首点：**零相位口径**（2026-10-16 二次修订）⇒ 每个分量 t=0 处都是 sin(0)=0
         * ⇒ 首点必须为 0。（旧口径"用实测相位"时这里是 20·sin(0.7)=12.9 —— 见下方针口用例） */
        CHECK(fabsf(y1[0]) < 0.01f);
        /* 周期性：y[j] == y[j+周期]（基波 10.5k 在 320 点 1T 轴上的周期 =
         * 320/3 点 —— 3 是 10500/10500？不：横轴 1T=1/10.5k，谐波 m=3 的
         * 周期是 T/3，叠加波形周期 = T1（基波）。3T 波形第 3 段应与 1T 全程
         * 相同 —— 用 3T 的第 2/3 段对比 1T 的对应点。 */
    }

    TEST_CASE("波形 3T = 1T 平移");
    {
        float y1[320], y3[320];
        op_wave_build(&r, 1, y1, 320);
        op_wave_build(&r, 3, y3, 320);
        /* 3T 的 j 点时刻 = 3T·j/320；1T 的 (j·3 mod 320) 点时刻 = T·(3j mod 320)/320
         * —— 3T·j/320 = T·(3j/320)，与 (3j mod 320)/320·T 相差整数个周期 T
         * ⇒ 基波同相；谐波 m=3 时相位差 2π·m·(整数周期) = 2π·整数 ⇒ 同值。 */
        for (int j = 0; j < 320; j++) {
            CHECK(fabsf(y3[j] - y1[(j * 3) % 320]) < 0.05f);
        }
    }

    /* ---- 形状口径（2026-10-16 二次修订）：**相位不参与形状重建** ----
     * 题面信号的定义是 `Σ A_k·sin(2π·f_k·t)`（各分量零相位）⇒ 真值形状只由幅值定。
     * 而本机实测相位不可用：① 参考点是**帧起点**、与源无关（无触发）；② 标定逐点
     * 各自一帧 ⇒ 相位表每重标一次就换一组随机偏移（上板实测污染 +19° / ~180°）。
     * 下面两条一起钉住"相位对输出零影响"—— 任何把相位重新引回形状的改动都会红。 */
    TEST_CASE("口径：把两个分量的相位随意拧过，输出逐点不变");
    {
        meas_result_t r2 = r;
        r2.peak[0].phase_rad = -1.1f;
        r2.peak[1].phase_rad = 2.4f;
        float y0[320], y2[320];
        op_wave_build(&r, 1, y0, 320);
        op_wave_build(&r2, 1, y2, 320);
        float dmax = 0.0f;
        for (int j = 0; j < 320; j++) {
            const float d = fabsf(y2[j] - y0[j]);
            if (d > dmax) {
                dmax = d;
            }
        }
        CHECK(dmax < 0.01f); /* 逐点相同（只剩浮点舍入，判据 5mV/100mV 的 1/500） */
    }

    TEST_CASE("口径：形状 = Σ A_k·sin(2π·m_k·f1·t)，左端锚在基波过零点");
    {
        /* 逐点对照零相位解析式（输入里那两根的 φ 是 0 / 0.7，都必须被忽略）：
         *   w(t) = 60·sin(θ) + 20·sin(3θ)，θ = 2π·j/320
         * ⚠️ 若有人把实测相位重新引回重建，本条会当场红（旧口径下首点是 12.9 mV）。 */
        float y1[320];
        CHECK_EQ(op_wave_build(&r, 1, y1, 320), 320);
        float dmax = 0.0f;
        for (int j = 0; j < 320; j++) {
            const float th   = 6.28318530717958647692f * (float)j / 320.0f;
            const float want = 60.0f * sinf(th) + 20.0f * sinf(3.0f * th);
            const float d    = fabsf(y1[j] - want);
            if (d > dmax) {
                dmax = d;
            }
        }
        CHECK(dmax < 0.05f);
    }

    TEST_CASE("波形参数防御");
    {
        float y[8];
        CHECK(op_wave_build(&r, 2, y, 8) == -1); /* cycles 只许 1/3 */
        CHECK(op_wave_build(&r, 1, NULL, 8) == -1);
        {
            meas_result_t zero = {0};
            CHECK(op_wave_build(&zero, 1, y, 8) == -1); /* 没峰 */
        }
    }

    /* ---- 谱线几何：log 轴铺满 + 幅值归一 ---- */
    TEST_CASE("谱线几何");
    {
        op_spec_line_t s[MEAS_MAX_PEAKS];
        CHECK_EQ(op_spec_build(&r, s, MEAS_MAX_PEAKS), 2);
        /* 位置：x = (log f − log 10k)/(log 500k − log 10k) */
        const float lx_lo = log10f(10000.0f);
        const float lx_hi = log10f(500000.0f);
        CHECK(fabs(s[0].x01 - (log10f(10500.0f) - lx_lo) / (lx_hi - lx_lo)) < 1e-5);
        CHECK(fabs(s[1].x01 - (log10f(31500.0f) - lx_lo) / (lx_hi - lx_lo)) < 1e-5);
        CHECK(s[0].x01 < s[1].x01);                 /* 相对位置保序 */
        CHECK(s[0].x01 >= 0.0f && s[1].x01 <= 1.0f); /* 铺满量程语义 */
        CHECK(fabs(s[0].len01 - 1.0f) < 1e-6);       /* 最高峰 = 满长 */
        CHECK(fabs(s[1].len01 - 20.0f / 60.0f) < 1e-5); /* 比例 = 幅值比 */
    }

    TEST_CASE("谱线参数防御");
    {
        op_spec_line_t s[MEAS_MAX_PEAKS];
        meas_result_t zero = {0};
        CHECK(op_spec_build(&zero, s, MEAS_MAX_PEAKS) == -1);
        CHECK(op_spec_build(&r, s, 1) == -1); /* 容量不足 */
    }

    return test_report();
}
