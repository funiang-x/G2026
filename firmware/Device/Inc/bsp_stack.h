#ifndef BSP_STACK_H
#define BSP_STACK_H
/* bsp_stack —— 主栈（MSP）只读查询。
 *
 * 为什么存在：App 层禁止 include 芯片头（`AGENTS.md` §3，分层检查 R1b 拦
 * core_cm4.h），而运行期内存审计需要读 MSP 判断"主栈用到多深"。
 * 按"谁需要硬件事实谁找 BSP"的规矩，把 __get_MSP 包在这里，
 * App 层拿到的只是一个 uint32_t 地址值。
 *
 * MSP 在本工程的身份：**调度器启动前的主栈 + 之后全部 ISR 的栈**
 * （任务走各自的 PSP 栈）。它的峰值深度值得盯 ——
 * 这就是 canary 方案（app_mem）的物理基础。
 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 当前 MSP 寄存器值。只读查询，无副作用。 */
uint32_t bsp_stack_msp_get(void);

#ifdef __cplusplus
}
#endif
#endif /* BSP_STACK_H */
