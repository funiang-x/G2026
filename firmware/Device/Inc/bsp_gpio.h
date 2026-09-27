#ifndef BSP_GPIO_H
#define BSP_GPIO_H
/* bsp_gpio —— 通用 GPIO 抽象。
 * 上层只说"哪个端口、哪个引脚、什么模式"，永远不出现 HAL_GPIO_InitTypeDef
 * 和 GPIOA 这类芯片符号都挡在 BSP 内部，上层不必认识它们。 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BSP_GPIO_PA = 0,
    BSP_GPIO_PB,
    BSP_GPIO_PC,
    BSP_GPIO_PD,
    BSP_GPIO_PE,
    BSP_GPIO_PF,
    BSP_GPIO_PG,
    BSP_GPIO_PH,
    BSP_GPIO_PORTS
} bsp_gpio_port_t;
/* ★ 接口诚实性：枚举列出 PA..PH（8 个），但**目标 STM32F407VGT6 是 LQFP100，
 *   实际只引出 PA / PB / PC / PD / PE，加上 PH0 / PH1（晶振脚）**。
 *   PF / PG 在这个封装上**没有键合出来**。
 *   ⚠️ 注意：传 PF / PG **不会**返回错误 —— bsp_gpio_config 会把它映射到
 *   内存映射里存在的端口基址并"成功"返回 0，只是那个引脚物理上不存在，
 *   配了也毫无效果。这比返回 -1 更隐蔽（没有失败信号）。
 *   当前 bsp_gpio 不知道目标封装，无法替你拦；用错端口只能靠"引脚电平
 *   读不回来"发现。留全枚举是为了换到引脚更全的封装时零改动。 */

typedef struct {
    bsp_gpio_port_t port; /* BSP_GPIO_PA..PH */
    uint8_t pin;           /* 0..15 */
} bsp_gpio_t;

typedef enum {
    BSP_GPIO_OUT_PP = 0,  /* 推挽输出 */
    BSP_GPIO_OUT_OD,      /* 开漏输出（I2C、电平转换等）*/
    BSP_GPIO_IN_FLOAT,    /* 浮空输入 */
    BSP_GPIO_IN_PULLUP,   /* 上拉输入（按键常用）*/
    BSP_GPIO_IN_PULLDOWN, /* 下拉输入 */
    BSP_GPIO_ANALOG,      /* 模拟（ADC/DAC 引脚必须设这个，否则读数飘）*/
    BSP_GPIO_AF_PP        /* 复用推挽（UART/SPI/PWM 用）*/
} bsp_gpio_mode_t;

/* 配置引脚。mode 为 BSP_GPIO_AF_PP 时用 af 指定复用号（见芯片数据手册
 * "Alternate function mapping" 表）；其它模式忽略 af。
 * 返回 0 成功，-1 参数非法。 */
int bsp_gpio_config(bsp_gpio_t g, bsp_gpio_mode_t mode, uint32_t af);

/* 输出电平：level 非 0 = 高。未配置为输出时调用行为未定义。 */
void bsp_gpio_write(bsp_gpio_t g, int level);

/* 读电平，返回 0/1；参数非法返回 -1。 */
int bsp_gpio_read(bsp_gpio_t g);

/* 翻转电平。 */
void bsp_gpio_toggle(bsp_gpio_t g);

/* 快速开关需要的端口掩码（避免每次算），返回 0 成功。 */
int bsp_gpio_mask(bsp_gpio_t g, uint16_t *out_mask);

/* 复杂外设（SPI/UART/PWM）要配复用脚，但调用方不该自己拼 bsp_gpio_mode_t。
 * 这个便捷函数等价于 bsp_gpio_config(g, BSP_GPIO_AF_PP, af)，
 * 让 BSP 内部各外设文件读起来是一行。返回 0 成功。 */
static inline int bsp_gpio_config_af(bsp_gpio_t g, uint32_t af) {
    return bsp_gpio_config(g, BSP_GPIO_AF_PP, af);
}

/* 输出推挽的便捷写法（CS/DC/RST 这类控制脚）。返回 0 成功。 */
static inline int bsp_gpio_config_out(bsp_gpio_t g) {
    return bsp_gpio_config(g, BSP_GPIO_OUT_PP, 0U);
}

#ifdef __cplusplus
}
#endif
#endif /* BSP_GPIO_H */
