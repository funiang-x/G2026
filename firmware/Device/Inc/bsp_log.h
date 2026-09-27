#ifndef BSP_LOG_H
#define BSP_LOG_H
/* ============================================================================
 *  bsp_log.h —— 板级日志**后端**的初始化与控制（Device 层）
 *
 *  ★ 本文件**不再定义 LOG_E/W/I/D 宏**。全工程的日志宏只有一套，
 *    在 `Common/Inc/log.h`（那是零依赖的抽象接口，Operation 层也能用）。
 *    本文件只负责"后端怎么起、级别怎么设、原始数据怎么写"。
 *
 *  ⚠️ 这是与参考工程 `template/` 的一处**有意分歧**：那边 `Common/log.h` 与
 *    `Device/bsp_log.h` 各定义了一套同名 LOG_* 宏，两者会撞，而且"哪个生效"
 *    取决于 include 顺序 —— 那是静默失效的典型形态。
 *    本工程把它收成一套：**接口在 Common，实现在 Device**（见 log.h 的文件头）。
 *
 *  后端二选一，由 CMake 的 `LOG_BACKEND` 决定，编译期切换：
 *      RTT （默认）：走 J-Link 的 SEGGER RTT，不占用串口，速度极快
 *      UART        ：走 board_config.h 里配置的串口，无需调试探针
 *
 *  并发注意：
 *    实现内部用一把**递归互斥锁**把"加前缀 + 写出"整段包住（见 bsp_log.c），
 *    多任务并发打印是**安全的**，不会串行错乱。
 *    ⚠️ 但**中断上下文里不要打印** —— ISR 里取信号量是非法操作；
 *       故障路径请改用 bsp_log_printf_nolock()（见下）。
 *    ⚠️ **调度器启动前**（main 早期）取锁同样非法（会进 FreeRTOS 临界区、
 *       把 BASEPRI 永久钉住）；实现内部用 xTaskGetSchedulerState() 作判据
 *       自动跳过 —— 那段时间本来就是单线程，不需要锁。
 * ==========================================================================*/
#include <stdint.h>

#include "log.h" /* log_level_t 与 LOG_* 宏的唯一来源 */

#ifdef __cplusplus
extern "C" {
#endif

/* 日志级别沿用 Common 层的 log_level_t —— 全工程一个枚举，不再各定义一套。 */
typedef log_level_t bsp_log_level_t;

#define BSP_LOG_ERROR LOG_LVL_ERROR
#define BSP_LOG_WARN  LOG_LVL_WARN
#define BSP_LOG_INFO  LOG_LVL_INFO
#define BSP_LOG_DEBUG LOG_LVL_DEBUG

/* 初始化日志后端。
 * ★ RTT 无需外部资源；UART 后端由本函数**自己**拉起（串口初始化从 main.c
 *   下沉到这里，main() 不必知道用的是哪种后端）。
 * ⚠️ 本函数在调度器启动前被调用，所以它内部只做初始化、不建锁。 */
void bsp_log_init(void);

void bsp_log_set_level(bsp_log_level_t level);
bsp_log_level_t bsp_log_get_level(void);

/* 原始数据直写：不加级别前缀、不补换行。供 newlib 的 _write 重定向使用。 */
void bsp_log_write_raw(const char *data, unsigned len);

/* ★ 故障路径专用：同 LOG_E，但**不取互斥锁、不碰任何 RTOS 原语**。
 *
 *  为什么需要它（FreeRTOS 钩子的可重入陷阱）：
 *    vApplicationMallocFailedHook 是在**堆分配失败**时被调用的。若它调
 *    log_write → log_put，而此刻互斥锁尚未惰性创建，日志内部会去
 *    xSemaphoreCreateRecursiveMutex() → pvPortMalloc() → 再次失败 →
 *    再次进入钩子 → **递归直到栈溢出**。恰好在最需要诊断的时候失效。
 *
 *  ⚠️ 使用约束：只允许在"系统即将停机"的路径上用（故障钩子 / panic）。
 *    它与正常日志并发时会交错输出 —— 因为没有锁。正常路径请用 LOG_E。 */
#if defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
void bsp_log_printf_nolock(bsp_log_level_t level, const char *fmt, ...);

#ifdef __cplusplus
}
#endif
#endif /* BSP_LOG_H */
