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
        /* 1T 首点 = Σ A·sin(φ) = 0 + 20·sin(0.7) */
        CHECK(fabsf(y1[0] - 20.0f * sinf(0.7f)) < 0.1f);
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
