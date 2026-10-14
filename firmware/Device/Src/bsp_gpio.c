/* bsp_gpio 实现（STM32F4 / ST HAL 后端）。
 * 本文件允许出现 HAL 类型 —— BSP 就是唯一能用 HAL 的层。
 * "端口号 → 寄存器"映射**只在这一份**（下方 static switch）：原先抽过
 * bsp_port_map.c，理由是"三处重复实现"；2026-10-07 起 bsp_pwm.c 已不存在、
 * bsp_log.c 也不再碰 GPIO，消费者只剩本文件 ⇒ 折回来，模块删除。
 * Device/Inc/bsp_gpio.h 对外只有"端口 + 引脚 + 模式"。 */
#include "bsp_gpio.h"

#include "stm32f4xx_hal.h"

/* ★ 为什么用 switch 而不是基址算术：F4 上 GPIOA..H 等间距（0x400），
 *    一行算术就能出基址，但算术**不过界检查** —— 端口号错一个数字就得到
 *    野指针、静默踩内存。switch 越界返回 0，把非法端口号变成**可判定的失败**。
 *    换芯片时基址与时钟宏都要重查手册，别假设等间距/连续。
 * ★ 双重强转（uintptr_t → uint32_t）避免指针更宽的平台上 -Wpointer-to-int-cast。 */
static uint32_t port_base(uint32_t idx) {
    switch (idx) {
        case 0U: return (uint32_t)(uintptr_t)GPIOA;
        case 1U: return (uint32_t)(uintptr_t)GPIOB;
        case 2U: return (uint32_t)(uintptr_t)GPIOC;
        case 3U: return (uint32_t)(uintptr_t)GPIOD;
        case 4U: return (uint32_t)(uintptr_t)GPIOE;
        case 5U: return (uint32_t)(uintptr_t)GPIOF;
        case 6U: return (uint32_t)(uintptr_t)GPIOG;
        case 7U: return (uint32_t)(uintptr_t)GPIOH;
        default: return 0U;
    }
}

static void port_clk_enable(uint32_t idx) {
    switch (idx) {
        case 0U: __HAL_RCC_GPIOA_CLK_ENABLE(); break;
        case 1U: __HAL_RCC_GPIOB_CLK_ENABLE(); break;
        case 2U: __HAL_RCC_GPIOC_CLK_ENABLE(); break;
        case 3U: __HAL_RCC_GPIOD_CLK_ENABLE(); break;
        case 4U: __HAL_RCC_GPIOE_CLK_ENABLE(); break;
        case 5U: __HAL_RCC_GPIOF_CLK_ENABLE(); break;
        case 6U: __HAL_RCC_GPIOG_CLK_ENABLE(); break;
        case 7U: __HAL_RCC_GPIOH_CLK_ENABLE(); break;
        default: break; /* 越界：与 base 返回 0 同义，静默忽略 */
    }
}

/* 端口枚举 → HAL 端口指针。越界（含 BSP_GPIO_PORTS 这个哨兵值）返回 0。 */
static GPIO_TypeDef *port_of(bsp_gpio_port_t port) {
    return (GPIO_TypeDef *)port_base((uint32_t)port);
}

static uint16_t pin_mask(uint8_t pin) {
    return (uint16_t)(1U << pin);
}

int bsp_gpio_config(bsp_gpio_t g, bsp_gpio_mode_t mode, uint32_t af) {
    GPIO_TypeDef *port = port_of(g.port);
    if (port == 0 || g.pin > 15U) {
        return -1;
    }
    port_clk_enable((uint32_t)g.port);

    GPIO_InitTypeDef init = {0};
    init.Pin = pin_mask(g.pin);
    init.Pull = GPIO_NOPULL;
    init.Speed = GPIO_SPEED_FREQ_LOW;

    switch (mode) {
        case BSP_GPIO_OUT_PP:
            init.Mode = GPIO_MODE_OUTPUT_PP;
            break;
        case BSP_GPIO_OUT_OD:
            init.Mode = GPIO_MODE_OUTPUT_OD;
            break;
        case BSP_GPIO_IN_FLOAT:
            init.Mode = GPIO_MODE_INPUT;
            break;
        case BSP_GPIO_IN_PULLUP:
            init.Mode = GPIO_MODE_INPUT;
            init.Pull = GPIO_PULLUP;
            break;
        case BSP_GPIO_IN_PULLDOWN:
            init.Mode = GPIO_MODE_INPUT;
            init.Pull = GPIO_PULLDOWN;
            break;
        case BSP_GPIO_ANALOG:
            init.Mode = GPIO_MODE_ANALOG;
            break;
        case BSP_GPIO_AF_PP:
            init.Mode = GPIO_MODE_AF_PP;
            init.Alternate = af;
            init.Speed = GPIO_SPEED_FREQ_HIGH;
            break;
        default:
            return -1;
    }
    HAL_GPIO_Init(port, &init);
    return 0;
}

void bsp_gpio_write(bsp_gpio_t g, int level) {
    GPIO_TypeDef *port = port_of(g.port);
    if (port == 0 || g.pin > 15U) {
        return;
    }
    HAL_GPIO_WritePin(port, pin_mask(g.pin), (level != 0) ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

int bsp_gpio_read(bsp_gpio_t g) {
    GPIO_TypeDef *port = port_of(g.port);
    if (port == 0 || g.pin > 15U) {
        return -1;
    }
    return (HAL_GPIO_ReadPin(port, pin_mask(g.pin)) == GPIO_PIN_SET) ? 1 : 0;
}
