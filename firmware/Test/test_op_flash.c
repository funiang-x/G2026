/* ============================================================================
 *  test_op_flash.c —— `op_flash` 的 PC 单测
 *
 *  运行：`python Tools/fw.py test`
 *
 *  ⚠️ 每个用例都带**负向断言**：不只验"该切的切了"，也验
 *     "不该切的没多切""装不下时整体失败而不是返回半截"。
 * ==========================================================================*/
#include "op_flash.h"
#include "test_util.h"

#define PAGE 256U

/* 对齐 + 正好一整页：**不该**被切成多段 */
static void test_aligned_exact(void) {
    op_flash_seg_t seg[8];

    TEST_CASE("aligned_exact");
    int n = op_flash_split_pages(0U, PAGE, PAGE, seg, 8);
    CHECK_EQ(n, 1);
    CHECK_EQ(seg[0].offset, 0U);
    CHECK_EQ(seg[0].len, PAGE);
}

/* 起始未对齐：第一段必须短一截（这是最容易写错的地方） */
static void test_unaligned_start(void) {
    op_flash_seg_t seg[8];

    TEST_CASE("unaligned_start");
    /* 从 200 写 200 字节：200..255 = 56B，然后 256..399 = 144B */
    int n = op_flash_split_pages(200U, 200U, PAGE, seg, 8);
    CHECK_EQ(n, 2);
    CHECK_EQ(seg[0].offset, 0U);
    CHECK_EQ(seg[0].len, 56U);
    CHECK_EQ(seg[1].offset, 56U);
    CHECK_EQ(seg[1].len, 144U);
    /* 负向：不是 200/200 平分成两段 */
    CHECK(seg[0].len != seg[1].len);
}

/* 正好落在页边界起写：**不该**多切一段 */
static void test_exact_boundary(void) {
    op_flash_seg_t seg[8];

    TEST_CASE("exact_boundary");
    int n = op_flash_split_pages(PAGE, PAGE, PAGE, seg, 8);
    CHECK_EQ(n, 1);
    CHECK_EQ(seg[0].len, PAGE);

    /* 跨两个整页 */
    n = op_flash_split_pages(PAGE, PAGE * 2U, PAGE, seg, 8);
    CHECK_EQ(n, 2);
    CHECK_EQ(seg[0].len, PAGE);
    CHECK_EQ(seg[1].len, PAGE);
}

/* 跨多页：段数 = 页数，且每段都 <= page_size */
static void test_multi_page(void) {
    op_flash_seg_t seg[8];

    TEST_CASE("multi_page");
    int n = op_flash_split_pages(0U, PAGE * 4U, PAGE, seg, 8);
    CHECK_EQ(n, 4);
    for (int i = 0; i < n; i++) {
        CHECK(seg[i].len <= PAGE);           /* 负向：没有哪段跨页 */
        CHECK_EQ(seg[i].offset, (uint32_t)i * PAGE);
    }
}

/* 尾段不足一页 */
static void test_short_tail(void) {
    op_flash_seg_t seg[8];

    TEST_CASE("short_tail");
    /* 从 200 写 100：56B + 44B */
    int n = op_flash_split_pages(200U, 100U, PAGE, seg, 8);
    CHECK_EQ(n, 2);
    CHECK_EQ(seg[0].len, 56U);
    CHECK_EQ(seg[1].len, 44U);
    /* 负向：尾段不该被凑成整页 */
    CHECK(seg[1].len < PAGE);
}

/* 段数必须与实际拆分一致（两个入口不能各算各的） */
static void test_count_matches_split(void) {
    op_flash_seg_t seg[16];

    TEST_CASE("count_matches_split");
    const uint32_t cases[][2] = {{0U, 1U},      {255U, 1U},   {255U, 2U},
                                 {1U, 255U},    {1U, 256U},   {100U, 1000U},
                                 {4096U, 4096U}};
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int want = op_flash_page_count(cases[i][0], cases[i][1], PAGE);
        int got = op_flash_split_pages(cases[i][0], cases[i][1], PAGE, seg, 16);
        CHECK(want > 0);
        CHECK_EQ(got, want);
    }
}

/* out 装不下：**整体失败**，不是"返回半截" */
static void test_capacity_too_small(void) {
    op_flash_seg_t seg[2];

    TEST_CASE("capacity_too_small");
    /* 需要 4 段，只给 2 格 */
    CHECK_EQ(op_flash_split_pages(0U, PAGE * 4U, PAGE, seg, 2), -1);
    /* 正好装得下 → 成功 */
    CHECK_EQ(op_flash_split_pages(0U, PAGE * 2U, PAGE, seg, 2), 2);
}

/* 非法参数与溢出 */
static void test_bad_args(void) {
    op_flash_seg_t seg[4];

    TEST_CASE("bad_args");
    CHECK_EQ(op_flash_split_pages(0U, 100U, 0U, seg, 4), -1);   /* page_size=0 */
    CHECK_EQ(op_flash_split_pages(0U, 0U, PAGE, seg, 4), -1);   /* len=0 */
    CHECK_EQ(op_flash_split_pages(0U, 100U, PAGE, NULL, 4), -1);/* out=NULL */
    CHECK_EQ(op_flash_split_pages(0U, 100U, PAGE, seg, 0), -1); /* cap=0 */
    CHECK_EQ(op_flash_page_count(0U, 100U, 0U), -1);

    /* addr + len 溢出：宁可失败，也别算出一个假地址 */
    CHECK_EQ(op_flash_page_count(0xFFFFFFF0U, 0x20U, PAGE), -1);
    CHECK_EQ(op_flash_split_pages(0xFFFFFFF0U, 0x20U, PAGE, seg, 4), -1);
}

int main(void) {
    test_aligned_exact();
    test_unaligned_start();
    test_exact_boundary();
    test_multi_page();
    test_short_tail();
    test_count_matches_split();
    test_capacity_too_small();
    test_bad_args();
    return test_report();
}
