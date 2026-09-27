/* ============================================================================
 *  test_op_blink.c —— `op_blink` 的 PC 单测
 *
 *  运行：`python Tools/fw.py test`
 *  它用 **host gcc** 编 Operation/ 的同一份源码 —— 这就是"业务逻辑能在电脑上
 *  验证"的兑现（docs/01 规矩 2 的钥匙）。
 *
 *  ⚠️ 每个用例都要有**负向断言**（"不该亮的时候确实没亮"）。
 *     只测正向的测试，等于只验证了"顺利的时候是对的"。
 * ==========================================================================*/
#include "op_blink.h"
#include "test_util.h"

/* 亮 500 / 灭 500 的标准用例 */
static void test_basic(void) {
    op_blink_t ctx;
    op_blink_init(&ctx, 500U, 500U);

    TEST_CASE("basic");
    CHECK(op_blink_step(&ctx, 0U));      /* 起点 = 亮 */
    CHECK(op_blink_step(&ctx, 499U));    /* 仍在亮区 */
    CHECK(!op_blink_step(&ctx, 500U));   /* 边界：500 属于灭区（左闭右开） */
    CHECK(!op_blink_step(&ctx, 999U));
    CHECK(op_blink_step(&ctx, 1000U));   /* 下一个周期 */
    CHECK(!op_blink_step(&ctx, 1500U));
}

/* 非对称占空比：确认不是"按 50% 猜的" */
static void test_duty(void) {
    op_blink_t ctx;
    op_blink_init(&ctx, 100U, 900U);

    TEST_CASE("duty");
    CHECK(op_blink_step(&ctx, 0U));
    CHECK(op_blink_step(&ctx, 99U));
    CHECK(!op_blink_step(&ctx, 100U));
    CHECK(!op_blink_step(&ctx, 999U));
    CHECK(op_blink_step(&ctx, 1000U));
}

/* ★ 关键用例：now_ms 跨 2^32 回绕。
 *   这是"用无符号减法"这条约定的守卫 —— 改成带加法的比较就会在这里挂。 */
static void test_wrap(void) {
    op_blink_t ctx;
    op_blink_init(&ctx, 500U, 500U);

    TEST_CASE("wrap");
    /* 先把相位推到接近回绕点 */
    CHECK(!op_blink_step(&ctx, 0xFFFFFE00UL));
    /* 跨过 0xFFFFFFFF：相位应连续，仍处于"灭"区 */
    CHECK(!op_blink_step(&ctx, 0x00000100UL));
    /* 再往前 0x200 ⇒ 累计相位超过一个周期 ⇒ 应回到"亮" */
    CHECK(op_blink_step(&ctx, 0x00000300UL));
}

/* 边界：调用间隔**远大于**周期（任务被长时间阻塞）。
 * 守卫"相位要按取模推进、不能只加一次周期"。 */
static void test_long_gap(void) {
    op_blink_t ctx;
    op_blink_init(&ctx, 100U, 100U);

    TEST_CASE("long_gap");
    CHECK(op_blink_step(&ctx, 0U));
    /* 跳过 1000 个周期后停在周期内相位 50 ⇒ 应为"亮" */
    CHECK(op_blink_step(&ctx, 100050U));
    /* 同一个相位再来一次，结论必须一致（不能因为"落后"而漂移） */
    CHECK(op_blink_step(&ctx, 100050U));
    CHECK(!op_blink_step(&ctx, 100150U));
}

/* 配置错误：周期为 0 必须是**确定行为**，不是死循环 / 除零 */
static void test_zero_period(void) {
    op_blink_t ctx;
    op_blink_init(&ctx, 0U, 0U);

    TEST_CASE("zero_period");
    CHECK(!op_blink_step(&ctx, 0U));
    CHECK(!op_blink_step(&ctx, 12345U));
}

int main(void) {
    test_basic();
    test_duty();
    test_wrap();
    test_long_gap();
    test_zero_period();
    return test_report();
}
