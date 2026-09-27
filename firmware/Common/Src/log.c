/* ============================================================================
 *  log.c —— 格式化 + 分派到后端（Common 层实现）
 *
 *  ⚠️ 本文件**不许** include 任何硬件头（HAL / FreeRTOS / Device 下的头）——
 *     它会被 Operation 间接依赖，一旦引了硬件头，Operation 在 PC 上就编不过。
 *     这里只允许用 C 标准库（PC 与 MCU 的 newlib 都有）。
 *
 *  后端（log_put）由链接时决定：MCU 走 Device/Src/bsp_log.c，PC 走 Test/stubs/。
 *
 *  ⚠️ 这里**不加级别前缀、不补换行** —— 那是后端的事（见 bsp_log.c 的 log_put）。
 *     理由：换行符在 RTT 与串口上的正确形式不同（串口要 \r\n），
 *     而"补什么"属于后端知识。Common 只管"把格式串变成一行字符串"。
 * ==========================================================================*/
#include "log.h"

#include <stdio.h>

/* 单行上限。注意这是个**栈上的数组**，所以别调太大 ——
 * 日志函数可能被多个任务调用，栈都要各自留出这么多。
 * 160 字节足够放下本工程的所有日志行。 */
#define LOG_LINE_MAX 160U

void log_write(log_level_t lvl, const char *fmt, ...) {
    char buf[LOG_LINE_MAX];
    va_list ap;

    if (fmt == NULL) {
        return;
    }

    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);

    if (n < 0) {
        return;   /* 格式化失败：静默丢弃，不要在这里再打日志（会递归） */
    }
    unsigned len = (n >= (int)sizeof buf) ? (unsigned)(sizeof buf - 1U) : (unsigned)n;
    log_put(lvl, buf, len);
}
