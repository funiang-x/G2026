#ifndef BSP_PORT_MAP_H
#define BSP_PORT_MAP_H
/* bsp_port_map —— 数字端口号 → GPIO 寄存器基址 / 时钟使能。
 *
 * ★ 为什么要有这个模块：在它出现之前，"端口号 → 寄存器"这张映射表在 BSP 内部
 *   被**独立实现了三遍**，而且用了三种不同写法：
 *     · bsp_gpio.c  —— switch 返回 GPIOA..GPIOH（入参是 bsp_gpio_port_t 枚举）
 *     · bsp_pwm.c   —— switch 返回 GPIOA..GPIOH（入参是 uint32_t，另一份）
 *     · bsp_log.c   —— 基址算术 GPIOA_BASE + 0x400 * idx
 *   三份里算术那份在 idx 越界时返回**野指针**（算术不过界检查），而 switch 版
 *   返回 0。同一个"端口号"，三处的失败语义并不一致 —— 这类不一致不会立刻出错，
 *   只会在换板子那天集中爆发。
 *   现在只保留一份，统一语义：**越界返回 0**。
 *
 * ★ 接口为什么不返回 GPIO_TypeDef *：
 *   调用方全部在 BSP 内部，各自强转一次即可。对外只暴露 uint32_t，
 *   上层代码就不随 HAL 类型 / 外设实例变化而改动。
 *   （原文写的是"按层契约 check_layering 的 R6 不得出现 HAL 类型" ——
 *     该检查器已随强制分层一起删除，2026-09-17。现在这只是一条**风格建议**，
 *     没有机器兜底，违反了也不会报错。）
 *
 * ★ 端口号约定：0 = PA，1 = PB，…，7 = PH。
 *   与 bsp_gpio_port_t 的枚举值同序，也与 board_config.h 里 BOARD_*_PORT
 *   的数字语义一致。三条约定必须保持同序 —— 这是本模块能统一的前提。
 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 端口号 → GPIO 寄存器基址。返回 0 表示端口号越界。
 * 调用方用法：(GPIO_TypeDef *)bsp_port_map_base(idx) */
uint32_t bsp_port_map_base(uint32_t idx);

/* 使能该端口的 RCC 时钟。端口号越界时不动作（静默忽略，与 base 返回 0 同义）。 */
void bsp_port_map_clk_enable(uint32_t idx);

#ifdef __cplusplus
}
#endif
#endif /* BSP_PORT_MAP_H */
