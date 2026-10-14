/* ============================================================================
 *  test_op_cal —— 标定表的插值 / 序列化 / CRC 单测
 * ==========================================================================*/
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "op_cal.h"
#include "test_util.h"

static void fill_test_table(op_cal_table_t *t) {
    op_cal_default_freqs(t);
    for (int k = 0; k < OP_CAL_POINTS; k++) {
        /* 造一个光滑的假标定：g 在 0.9~1.1 摆动，p 随频率线性增长 */
        t->g[k] = 1.0f + 0.1f * sinf((float)k * 0.7f);
        t->p[k] = -0.05f * (float)k;
    }
}

int main(void) {
    op_cal_table_t t;

    /* ---- 默认频点：两端 10k/500k，严格递增 ---- */
    TEST_CASE("默认频点");
    op_cal_default_freqs(&t);
    CHECK(fabs(t.f[0] - 10000.0) < 1.0);
    CHECK(fabs(t.f[16] - 500000.0) < 1.0);
    for (int k = 1; k < OP_CAL_POINTS; k++) {
        CHECK(t.f[k] > t.f[k - 1]);
    }

    /* ---- lookup：表内插值单调平滑，端点钳位 ---- */
    TEST_CASE("lookup");
    {
        float g0, p0;
        fill_test_table(&t);
        op_cal_lookup(&t, 10000.0f, &g0, &p0);
        CHECK(fabs(g0 - t.g[0]) < 1e-6); /* 端点 = 表值本身 */
        CHECK(fabs(p0 - t.p[0]) < 1e-6);

        float g1, p1;
        op_cal_lookup(&t, 9000.0f, &g1, &p1); /* 下越界 → 首端点 */
        CHECK(fabs(g1 - t.g[0]) < 1e-6);
        op_cal_lookup(&t, 600000.0f, &g1, &p1); /* 上越界 → 末端点 */
        CHECK(fabs(g1 - t.g[16]) < 1e-6);

        /* 表点正中：log 域插值必落在两端点之间（端点性质——比"等于均值"
         * 更本质：log 域中点与算术中点天然差一个 AM-GM 间隙，不该当断言） */
        op_cal_lookup(&t, (t.f[8] + t.f[9]) * 0.5f, &g1, &p1);
        {
            float glo = t.g[8] < t.g[9] ? t.g[8] : t.g[9];
            float ghi = t.g[8] < t.g[9] ? t.g[9] : t.g[8];
            CHECK(g1 >= glo && g1 <= ghi);
        }
        {
            float plo = t.p[8] < t.p[9] ? t.p[8] : t.p[9];
            float phi = t.p[8] < t.p[9] ? t.p[9] : t.p[8];
            CHECK(p1 >= plo && p1 <= phi);
        }

        /* NULL 表不改输出（未标定语义） */
        float g2 = 1.0f, p2 = 0.0f;
        op_cal_lookup(NULL, 100000.0f, &g2, &p2);
        CHECK(fabs(g2 - 1.0f) < 1e-9 && fabs(p2) < 1e-9);
    }

    /* ---- pack/unpack 往返：逐字段一致 ---- */
    TEST_CASE("pack/unpack 往返");
    {
        uint8_t buf[OP_CAL_BLOB_SIZE];
        op_cal_table_t back;
        memset(&back, 0, sizeof(back));
        fill_test_table(&t);
        const int n = op_cal_pack(&t, buf, sizeof(buf));
        CHECK_EQ(n, (int)OP_CAL_BLOB_SIZE);
        CHECK_EQ(op_cal_unpack(buf, sizeof(buf), &back), 0);
        for (int k = 0; k < OP_CAL_POINTS; k++) {
            CHECK(fabs(back.f[k] - t.f[k]) < 1e-3);
            CHECK(fabs(back.g[k] - t.g[k]) < 1e-6);
            CHECK(fabs(back.p[k] - t.p[k]) < 1e-6);
        }
    }

    /* ---- CRC 篡改与魔数：任何一坏都拒绝 ---- */
    TEST_CASE("CRC/魔数防篡改");
    {
        uint8_t buf[OP_CAL_BLOB_SIZE];
        op_cal_table_t back;
        fill_test_table(&t);
        (void)op_cal_pack(&t, buf, sizeof(buf));

        buf[100] ^= 0xFFU; /* 翻转表数据一个字节 */
        CHECK(op_cal_unpack(buf, sizeof(buf), &back) == -1);
        buf[100] ^= 0xFFU; /* 恢复 */
        CHECK(op_cal_unpack(buf, sizeof(buf), &back) == 0);

        buf[212] ^= 0x01U; /* 翻转 CRC 域本身 */
        CHECK(op_cal_unpack(buf, sizeof(buf), &back) == -1);
        buf[212] ^= 0x01U;

        buf[0] = 'X'; /* 魔数错 */
        CHECK(op_cal_unpack(buf, sizeof(buf), &back) == -1);
        buf[0] = 'G';
        buf[4] = 2U; /* 版本不认 */
        CHECK(op_cal_unpack(buf, sizeof(buf), &back) == -1);
    }

    /* ---- 参数防御 ---- */
    TEST_CASE("参数防御");
    {
        uint8_t small[8];
        CHECK(op_cal_pack(&t, small, sizeof(small)) == -1);
        CHECK(op_cal_pack(NULL, small, sizeof(small)) == -1);
        CHECK(op_cal_unpack(small, sizeof(small), &t) == -1);
    }

    /* ---- ★ 相位展开（回归：2026-10-07 实测的 180° 插值错） ----
     * 造一条**真实形状**的相位曲线：8 阶 LC 的相移随频率单调下降、穿过 -180°，
     * 逐点测量拿到的是主值 ⇒ 序列里会跳 360°。用 op_cal_set_point 存表后，
     * 查表结果必须跟连续真值只差"小量"，不能差 180°。
     * （用不着真网表：只要曲线单调 + 跨 ±180°，就复现原缺陷。） */
    TEST_CASE("相位展开（跨 ±180°）");
    {
        op_cal_table_t w;
        const float PI = 3.14159265358979323846f;
        op_cal_default_freqs(&w);
        /* 真值：从 -0.05 rad 起，按 log-f 二次曲线降到 -5.0 rad（跨越 -π） */
        float ptrue[OP_CAL_POINTS];
        for (int k = 0; k < OP_CAL_POINTS; k++) {
            const float x = (float)k / (float)(OP_CAL_POINTS - 1);
            ptrue[k] = -0.05f - 4.95f * x * x;
        }
        for (int k = 0; k < OP_CAL_POINTS; k++) {
            /* 逐点"测量"：取主值（等价于固件里的 atan2 + π/2 再折回） */
            float meas = ptrue[k];
            while (meas > PI) {
                meas -= 2.0f * PI;
            }
            while (meas <= -PI) {
                meas += 2.0f * PI;
            }
            op_cal_set_point(&w, k, 1.0f, meas);
        }
        /* ① 展开后必须单调（真值单调 ⇒ 展开正确就单调） */
        for (int k = 1; k < OP_CAL_POINTS; k++) {
            CHECK(w.p[k] < w.p[k - 1]);
        }
        /* ② 展开值必须等于真值（mod 2π 之外，还要同支） */
        for (int k = 0; k < OP_CAL_POINTS; k++) {
            CHECK(fabsf(w.p[k] - ptrue[k]) < 1e-4f);
        }
        /* ③ 中点查表：与真值差 < 0.05 rad（约 3°）。未展开时这里是 ~3.1 rad。 */
        float g1, p1, worst = 0.0f;
        for (int k = 0; k + 1 < OP_CAL_POINTS; k++) {
            const float fm = sqrtf(w.f[k] * w.f[k + 1]); /* log 域中点 */
            const float xm = ((float)k + 0.5f) / (float)(OP_CAL_POINTS - 1);
            const float ptrue_m = -0.05f - 4.95f * xm * xm;
            op_cal_lookup(&w, fm, &g1, &p1);
            const float e = fabsf(p1 - ptrue_m);
            if (e > worst) {
                worst = e;
            }
        }
        CHECK(worst < 0.05f);
        /* ④ 直接写 t->p 的老路径会漏掉展开 —— 这条断言就是"为什么必须走 set_point" */
        {
            op_cal_table_t raw;
            op_cal_default_freqs(&raw);
            for (int k = 0; k < OP_CAL_POINTS; k++) {
                float meas = ptrue[k];
                while (meas > PI) {
                    meas -= 2.0f * PI;
                }
                while (meas <= -PI) {
                    meas += 2.0f * PI;
                }
                raw.p[k] = meas; /* 绕过展开 */
            }
            float bad = 0.0f;
            for (int k = 0; k + 1 < OP_CAL_POINTS; k++) {
                const float fm = sqrtf(raw.f[k] * raw.f[k + 1]);
                const float xm = ((float)k + 0.5f) / (float)(OP_CAL_POINTS - 1);
                op_cal_lookup(&raw, fm, &g1, &p1);
                const float e = fabsf(p1 - (-0.05f - 4.95f * xm * xm));
                if (e > bad) {
                    bad = e;
                }
            }
            CHECK(bad > 1.0f); /* 绕过展开 ⇒ 差 >1 rad，缺陷确实存在 */
        }
    }

    return test_report();
}
