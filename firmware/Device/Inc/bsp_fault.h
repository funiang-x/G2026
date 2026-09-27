#ifndef BSP_FAULT_H
#define BSP_FAULT_H
/* bsp_fault —— 崩溃现场取证。
 *
 * Cortex-M 硬件异常（HardFault/MemManage/BusFault/UsageFault）发生时，
 * 把中断压栈的寄存器 + 故障状态寄存器打到日志口，用来判断"崩在哪条指令、
 * 访问了哪个非法地址"。这是嵌入式排错里性价比最高的一步：
 * 没有这份现场，只能靠猜。
 *
 * 用法：平时不用管，Core/Src/stm32f4xx_it.c 会自动调用。
 * 触发后读 RTT/串口日志里的 [FAULT] 段落即可。
 *
 * 本文件还负责 **FreeRTOS 的两个故障钩子**（实现见 bsp_fault.c 末尾）：
 *     vApplicationStackOverflowHook   任务栈溢出
 *     vApplicationMallocFailedHook    堆耗尽
 * 它们由 Core/Inc/FreeRTOSConfig.h 里的开关启用，**不需要在本头文件里声明**
 * —— 内核自己 extern 了（而且 cmsis_os2.c 里有一份 __WEAK 空实现，忘了实现
 * 也不会报错，属于静默失效，所以务必保留 bsp_fault.c 里的强定义）。
 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 由 HardFault_Handler（naked 汇编）跳转进来，stack_frame 指向异常压栈帧。
 * 打印完现场后死循环（不返回）。 */
__attribute__((noreturn)) void bsp_fault_report(uint32_t *stack_frame);

/* 非 HardFault 的其它异常（MemManage/BusFault/UsageFault）走这里。 */
__attribute__((noreturn)) void bsp_fault_panic(const char *reason);

/* 打一行原因后复位 MCU（NVIC_SystemReset）。
 * 给"可自愈的致命错误"用：默认行为是无声死循环的路径（如 LVGL 断言
 * 自旋），复位让设备回到可用状态、并在日志里留下证据 —— 对"挂 1 秒
 * 可恢复"远优于"灰屏等死"的现场设备是正确取舍。 */
__attribute__((noreturn)) void bsp_fault_reset(const char *reason);

#ifdef __cplusplus
}
#endif
#endif /* BSP_FAULT_H */
