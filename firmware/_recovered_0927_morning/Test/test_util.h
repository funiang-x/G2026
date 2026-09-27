#ifndef TEST_UTIL_H
#define TEST_UTIL_H
/* ============================================================================
 *  test_util.h —— PC 单测的极简断言（不引任何测试框架）
 *
 *  为什么不用 Unity / CMock / Ceedling：本工程的单测只需要"断言 + 计数 + 退出码"，
 *  引框架的代价是"多一个第三方依赖 + 多一套构建规则"，而收益为零。
 *  少一个依赖就少一类"构建环境不一致"的问题。
 *
 *  用法：
 *      #include "test_util.h"
 *      static void test_xxx(void) { CHECK(1 + 1 == 2); CHECK_EQ(a, 3); }
 *      int main(void) { test_xxx(); return test_report(); }
 * ==========================================================================*/
#include <stdio.h>

static int g_test_fail = 0;
static int g_test_pass = 0;
static const char *g_test_cur = "?";

#define TEST_CASE(name) (g_test_cur = (name))

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (cond) {                                                            \
            g_test_pass++;                                                     \
        } else {                                                               \
            g_test_fail++;                                                     \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);            \
        }                                                                      \
    } while (0)

#define CHECK_EQ(actual, expect)                                               \
    do {                                                                       \
        long _a = (long)(actual);                                              \
        long _e = (long)(expect);                                              \
        if (_a == _e) {                                                        \
            g_test_pass++;                                                     \
        } else {                                                               \
            g_test_fail++;                                                     \
            printf("  FAIL %s:%d  %s = %ld，期望 %ld\n", __FILE__, __LINE__,     \
                   #actual, _a, _e);                                           \
        }                                                                      \
    } while (0)

/* 返回 0 = 全过。**必须**作为 main() 的返回值 —— 构建脚本认退出码。 */
static inline int test_report(void) {
    printf("\n%d passed, %d failed\n", g_test_pass, g_test_fail);
    if (g_test_pass == 0) {
        printf("!! 一个断言都没跑 —— 这不算通过\n");
        return 1;
    }
    return (g_test_fail == 0) ? 0 : 1;
}

#endif /* TEST_UTIL_H */
