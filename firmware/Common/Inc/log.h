#ifndef COMMON_LOG_H
#define COMMON_LOG_H
/* ============================================================================
 *  log.h —— 统一日志接口（Common 层，**全工程可用**）
 *
 *  设计要点：**接口与实现分离**，这是让 Operation 层能在 PC 上编译的关键。
 *
 *    ┌── 调用方：任何层（Task / Operation / Device）都调 LOG_*
 *    │
 *    ├── 本文件（Common）：只声明 + 定义宏，**不含任何硬件头**
 *    │
 *    ├── Common/Src/log.c：格式化（vsnprintf）后交给 log_put()
 *    │
 *    └── log_put() 的**实现由链接的那一侧提供** —— 这就是 C 里的依赖注入：
 *          · MCU 构建（固件）：Device/Src/bsp_log.c 提供
 *            （RTT 或 UART 出口 + 递归互斥锁，多任务安全）
 *          · PC 构建（Test/）：Test/stubs/log_host.c 提供（printf 到控制台）
 *        ⇒ 同一个符号名，链接时二选一。Operation 因此可以完全不认识硬件。
 *
 *  ⚠️ 本文件与 Common/Src/log.c **都不许** include HAL / FreeRTOS / Device 的头。
 *     一旦引了，所有依赖 log 的 Operation 代码在 PC 上都会编译失败 ——
 *     那等于把"可测试"这个最大价值丢掉了。
 *
 *  ⚠️ **全工程只有这一处定义 LOG_E/W/I/D**。Device/Inc/bsp_log.h 刻意不再定义
 *     一套同名宏 —— 两套宏会撞，而且"哪个生效"取决于 include 顺序。
 * ==========================================================================*/
#include <stdarg.h>

typedef enum {
    LOG_LVL_ERROR = 0,
    LOG_LVL_WARN,
    LOG_LVL_INFO,
    LOG_LVL_DEBUG,
} log_level_t;

/* 格式化一行日志并交给后端。全工程唯一的"打印"入口。 */
#if defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
void log_write(log_level_t lvl, const char *fmt, ...);

/* ★ 后端钩子：**由链入方提供**（见文件头）。不要在 Common 里实现它。
 *
 * 签名里带 level 是刻意的：级别过滤要在**后端**做（后端才知道当前阈值，
 * 而阈值属于"产品行为"，由 Task 层在启动时设 —— 那正是依赖注入的方向）。 */
void log_put(log_level_t lvl, const char *buf, unsigned len);

#define LOG_E(...) log_write(LOG_LVL_ERROR, __VA_ARGS__)
#define LOG_W(...) log_write(LOG_LVL_WARN, __VA_ARGS__)
#define LOG_I(...) log_write(LOG_LVL_INFO, __VA_ARGS__)
#define LOG_D(...) log_write(LOG_LVL_DEBUG, __VA_ARGS__)

#endif /* COMMON_LOG_H */
