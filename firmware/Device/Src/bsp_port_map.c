/* bsp_port_map 实现（STM32F4 / ST HAL 后端）。
 *
 * ★ 本文件是 BSP 内部**唯一**允许出现 GPIOA..GPIOH 与 __HAL_RCC_GPIOx_CLK_ENABLE
 *   的地方。注意这条是**约定**不是机器检查：R6 只管"对外头文件不得出现 HAL 类型"，
 *   管不到 .c 里的符号。改动本模块时请保持它是唯一的 —— 判据见方案文档 P0-2。
 *
 * ★ 为什么用 switch 而不是基址算术：
 *   F4 上 GPIOA..GPIOH 确实等间距排布（间隔 0x400），
 *       (GPIO_TypeDef *)(GPIOA_BASE + 0x400UL * idx)
 *   一行就能算出来，本模块出现之前 bsp_log.c 就是这么写的。但算术**不过界检查**：
 *   idx = 9 会得到 0x40020000 + 0x2400 这样一个野指针，调用方拿到后写进去
 *   = 静默踩内存，而且症状取决于那块地址上恰好是什么。
 *   switch 版越界返回 0，把"端口号非法"变成一个**可判定的失败**。
 *   代价是 8 个 case —— 换来"失败可见"，值得。
 *
 * ★ 换芯片（F7 / H7 / G4 等）时：GPIO 基址与 RCC 时钟宏都要重查数据手册，
 *   不要假设等间距、也不要假设编号连续。
 */
#include "bsp_port_map.h"

#include "stm32f4xx_hal.h"

/* GPIOA..GPIOH 是宏展开出的指针常量，转成整数基址交给调用方。
 * 双重强转是刻意的：先到 uintptr_t（指针宽度的整数），再收到 uint32_t，
 * 免得在指针宽度不同的平台上触发 -Wpointer-to-int-cast。 */
#define PORT_BASE(p) ((uint32_t)(uintptr_t)(p))

uint32_t bsp_port_map_base(uint32_t idx) {
    switch (idx) {
        case 0U:
            return PORT_BASE(GPIOA);
        case 1U:
            return PORT_BASE(GPIOB);
        case 2U:
            return PORT_BASE(GPIOC);
        case 3U:
            return PORT_BASE(GPIOD);
        case 4U:
            return PORT_BASE(GPIOE);
        case 5U:
            return PORT_BASE(GPIOF);
        case 6U:
            return PORT_BASE(GPIOG);
        case 7U:
            return PORT_BASE(GPIOH);
        default:
            return 0U;
    }
}

void bsp_port_map_clk_enable(uint32_t idx) {
    switch (idx) {
        case 0U:
            __HAL_RCC_GPIOA_CLK_ENABLE();
            break;
        case 1U:
            __HAL_RCC_GPIOB_CLK_ENABLE();
            break;
        case 2U:
            __HAL_RCC_GPIOC_CLK_ENABLE();
            break;
        case 3U:
            __HAL_RCC_GPIOD_CLK_ENABLE();
            break;
        case 4U:
            __HAL_RCC_GPIOE_CLK_ENABLE();
            break;
        case 5U:
            __HAL_RCC_GPIOF_CLK_ENABLE();
            break;
        case 6U:
            __HAL_RCC_GPIOG_CLK_ENABLE();
            break;
        case 7U:
            __HAL_RCC_GPIOH_CLK_ENABLE();
            break;
        default:
            break;
    }
}
