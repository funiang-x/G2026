/* bsp_gpio 实现（STM32F4 / ST HAL 后端）。
 * 本文件允许出现 HAL 类型 —— BSP 就是唯一能用 HAL 的层。
 * 但"端口号 → 寄存器"的映射**不在**这里：它集中在 bsp_port_map.c，
 * 免得同一张表在 BSP 内部被实现好几遍（2026-09-14 三合一）。
 * Device/Inc/bsp_gpio.h 对外只有"端口 + 引脚 + 模式"。 */
#include "bsp_gpio.h"

#include "bsp_port_map.h"
#include "stm32f4xx_hal.h"

/* 端口枚举 → HAL 端口指针。越界（含 BSP_GPIO_PORTS 这个哨兵值）返回 0。
 * 只是把 bsp_port_map 的 uint32_t 基址转成 HAL 指针类型，不做第二份映射表。 */
static GPIO_TypeDef *port_of(bsp_gpio_port_t port) {
    return (GPIO_TypeDef *)bsp_port_map_base((uint32_t)port);
}

static uint16_t pin_mask(uint8_t pin) {
    return (uint16_t)(1U << pin);
}

int bsp_gpio_mask(bsp_gpio_t g, uint16_t *out_mask) {
    if (out_mask == 0 || port_of(g.port) == 0 || g.pin > 15U) {
        return -1;
    }
    *out_mask = pin_mask(g.pin);
    return 0;
}

int bsp_gpio_config(bsp_gpio_t g, bsp_gpio_mode_t mode, uint32_t af) {
    GPIO_TypeDef *port = port_of(g.port);
    if (port == 0 || g.pin > 15U) {
        return -1;
    }
    bsp_port_map_clk_enable((uint32_t)g.port);

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

void bsp_gpio_toggle(bsp_gpio_t g) {
    GPIO_TypeDef *port = port_of(g.port);
    if (port == 0 || g.pin > 15U) {
        return;
    }
    HAL_GPIO_TogglePin(port, pin_mask(g.pin));
}
