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

    return test_report();
}
