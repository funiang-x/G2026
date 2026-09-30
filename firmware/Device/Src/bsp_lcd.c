/* ============================================================================
 *  bsp_lcd.c —— ST7796S 液晶屏 · 8080 16 位并口（FSMC Bank1/NE1）· 见 bsp_lcd.h
 *
 *  本文件只做三件事：
 *    ① 背光开关（普通 GPIO 脚；FSMC 时序由 CubeMX 的 `MX_FSMC_Init()` 配，
 *       本层不重复配 —— 见下方"FSMC 句柄"说明）
 *    ② 屏的硬复位（RESET 引脚时序）
 *    ③ 四个搬运原语：写命令 / 写参数 / 开窗 / 写像素
 *  **不含初始化序列、不含图形逻辑** —— 初始化序列由 LVGL 的 ST7796 驱动发。
 *
 *  ---- 8080 并口怎么"不用翻引脚" ----
 *  命令与数据的区分靠 RS（寄存器选择）线。本工程把 RS 接在 FSMC_A16 上，
 *  于是"写命令"和"写数据"只是**两个不同地址**的写操作：
 *      命令 → 0x60000000      （A16 = 0）
 *      数据 → 0x60020000      （A16 = 1，HADDR[17]）
 *  WR/RD/CS 的时序全部由 FSMC 硬件产生，CPU 只管 store。
 *  ⚠️ 地址偏移是 1<<17 不是 1<<16：**16 位数据总线时 FSMC 把 HADDR 右移 1 位**
 *     再接到 FSMC_A 上（RM0090：16 位模式下 HADDR[25:1] → FSMC_A[24:0]）。
 *     写成 1<<16 会打到 A15 上，症状是"屏完全没反应"。
 * ==========================================================================*/
#include "bsp_lcd.h"

#include "board_config.h" /* 板级事实：引脚 / FSMC 时序 / 地址 —— 本文件不写魔数 */
#include "bsp_gpio.h"     /* 复位/背光两个普通脚走它，不自己拼 GPIO_TypeDef */
#include "log.h"

#include "stm32f4xx_hal.h"

/* 8080 的两个窗口：低 16 位数据总线，按 16 位字访问。
 * ⚠️ 必须 `volatile` —— 否则 -Og/-Os 下连续的赋值会被优化掉，
 *    表现是"只写了最后一个字节"（这类 bug 只在优化开启时出现，很难查）。 */
#define LCD_CMD_WR (*((volatile uint16_t *)(uintptr_t)BOARD_LCD_CMD_ADDR))
#define LCD_DAT_WR (*((volatile uint16_t *)(uintptr_t)BOARD_LCD_DATA_ADDR))

/* 复位与背光：普通 GPIO，用 Device 层的通用抽象（bsp_gpio）。
 * ⚠️ 它们**只在本文件出现** —— 引脚事实在 board_config.h，方向在 CubeMX 生成的
 *    gpio.c 里（本层不重配，重复配会与生成物打架）。 */
static const bsp_gpio_t s_rst = {(bsp_gpio_port_t)BOARD_LCD_RST_PORT, BOARD_LCD_RST_PIN};
static const bsp_gpio_t s_bl = {(bsp_gpio_port_t)BOARD_LCD_BL_PORT, BOARD_LCD_BL_PIN};

/* FSMC 句柄**不在本文件**：CubeMX 生成的 `Core/Src/fsmc.c` 里有全局
 * `hsram1`，`MX_FSMC_Init()` 已经用它配好了 8080 16 位 SRAM 时序
 * （AS=1 / DS=15 / AccessMode A，见 .ioc 的 FSMC 参数）。
 * ⇒ 本层**不再配第二遍**：重复 HAL_SRAM_Init 没有意义，而且会把
 *    "时序到底在哪定义"这件事变成两个来源。 */
static int s_ready = 0;

/* 常用 8080 命令（MIPI DCS 标准集，ST7796S / ILI9341 / ST7789 共用） */
#define LCD_CMD_RDDID 0x04U /* 读显示 ID（4 字节） */
#define LCD_CMD_CASET 0x2AU /* 列地址设置 */
#define LCD_CMD_RASET 0x2BU /* 页地址设置 */
#define LCD_CMD_RAMWR 0x2CU /* 写显存 */

/* ------------------------------------------------------------------ 内部 */
static void lcd_hw_reset(void) {
    /* 复位脉冲：低 ≥10ms → 高 → 等 ≥120ms（datasheet 的 tRESW/tREST）。
     * ⚠️ 这里用 HAL_Delay，所以**不能在中断里调 bsp_lcd_init()**。 */
    bsp_gpio_write(s_rst, 0);
    HAL_Delay(20);
    bsp_gpio_write(s_rst, 1);
    HAL_Delay(120);
}

/* ------------------------------------------------------------------ 对外 */
int bsp_lcd_init(void) {
    /* ⚠️ 只做"本层该做的"：背光灭 + 屏硬复位。
     *    FSMC 控制器（时钟 / 引脚 AF12 / 时序）由 CubeMX 的
     *    `MX_FSMC_Init()` 完成，它在 main() 里、本函数之前被调用。
     *    这里**不做**任何校验 —— FSMC 配错了在 8080 上是"屏没反应"，
     *    而那是 bsp_lcd_self_check()（读 ID）负责暴露的事。 */
    bsp_lcd_backlight(0); /* 先关背光：屏还没初始化，此刻是白屏/花屏 */
    lcd_hw_reset();

    s_ready = 1;
    return 0;
}

void bsp_lcd_backlight(int on) {
    /* ⚠️ 数字开关、恒亮、禁 PWM —— PWM 调光的谐波列会全落 10k~500k 分析带，
     *    是固定频率伪峰制造机（干扰源管理红线，见 board_config.h 背光节）。
     *    本层也不提供调光接口 —— 没有调光需求，接口留了就会被用。 */
    bsp_gpio_write(s_bl, on ? 1 : 0);
}

void bsp_lcd_write_cmd(uint8_t cmd) {
    LCD_CMD_WR = cmd;
}

void bsp_lcd_write_data(const uint8_t *data, uint32_t len) {
    /* 8080 的命令参数**一个字节占一次 16 位传输**（走低 8 位），
     * 不是"两个字节打包成一个字" —— 打包了屏会按错误的参数解析。 */
    for (uint32_t i = 0; i < len; i++) {
        LCD_DAT_WR = data[i];
    }
}

void bsp_lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    uint8_t buf[4];

    bsp_lcd_write_cmd(LCD_CMD_CASET);
    buf[0] = (uint8_t)(x0 >> 8);
    buf[1] = (uint8_t)(x0 & 0xFF);
    buf[2] = (uint8_t)(x1 >> 8);
    buf[3] = (uint8_t)(x1 & 0xFF);
    bsp_lcd_write_data(buf, 4);

    bsp_lcd_write_cmd(LCD_CMD_RASET);
    buf[0] = (uint8_t)(y0 >> 8);
    buf[1] = (uint8_t)(y0 & 0xFF);
    buf[2] = (uint8_t)(y1 >> 8);
    buf[3] = (uint8_t)(y1 & 0xFF);
    bsp_lcd_write_data(buf, 4);
}

void bsp_lcd_write_pixels(const uint8_t *px, uint32_t len) {
    /* px 是 RGB565 **大端**字节流（高字节在前），与屏的期望一致。
     * 本层把它重新拼成 16 位字写出去 —— 不是"多一遍转换"，
     * 而是 16 位总线上必须这么拼（字节序的调整已经在 LVGL 侧做掉了，
     * 见 bsp_lcd.h 关于 RGB565_SWAPPED 的说明）。 */
    const uint8_t *p = px;
    uint32_t       n = len / 2U; /* 像素数 */

    bsp_lcd_write_cmd(LCD_CMD_RAMWR);
    for (uint32_t i = 0; i < n; i++) {
        LCD_DAT_WR = (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
        p += 2;
    }
}

void bsp_lcd_fill(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t rgb565) {
    uint32_t n = (uint32_t)(x1 - x0 + 1U) * (uint32_t)(y1 - y0 + 1U);

    bsp_lcd_set_window(x0, y0, x1, y1);
    bsp_lcd_write_cmd(LCD_CMD_RAMWR);
    for (uint32_t i = 0; i < n; i++) {
        LCD_DAT_WR = rgb565;
    }
}

int bsp_lcd_read_id(uint8_t id[4]) {
    if (id == NULL) {
        return -1;
    }
    if (!s_ready) {
        return -1;
    }

    /* 0x04 RDDID 是**读**命令：发完命令后从数据窗口读 4 次，
     * 每次的**低 8 位**才是有效数据（8080 读周期里屏只驱动 D[7:0] 的情况居多，
     * 有些屏会驱动 16 位；取低 8 位对两种情况都成立）。 */
    bsp_lcd_write_cmd(LCD_CMD_RDDID);
    for (int i = 0; i < 4; i++) {
        uint16_t v = LCD_DAT_WR;
        id[i] = (uint8_t)(v & 0xFFU);
    }

    /* 判据：全 0 或全 0xFF 都说明没读到东西（总线浮空 / 时序太紧）。
     * ⚠️ 不要用"ID 值等于某个期望值"做判据 —— ST7796S 批次间有差异，
     *    兼容片 HX8357 的 ID 完全不同（见 bsp_lcd.h 的说明）。 */
    if ((id[0] == 0 && id[1] == 0 && id[2] == 0 && id[3] == 0) ||
        (id[0] == 0xFF && id[1] == 0xFF && id[2] == 0xFF && id[3] == 0xFF)) {
        return -1;
    }
    return 0;
}

int bsp_lcd_self_check(void) {
    uint8_t id[4] = {0};

    if (bsp_lcd_read_id(id) != 0) {
        LOG_W("[SELFTEST] LCD 读不到 ID（RD 没接 / 时序太紧 / 屏没供电）"
              "—— 检查 PE/PD 数据线、PD4(NOE)、PD7(NE1)、PD11(A16)");
        return -1;
    }
    LOG_I("[SELFTEST] LCD ok, RDDID=%02X %02X %02X %02X（能读到即证明 16 位总线 + "
          "RS 地址译码 + 读写时序都通）",
          id[0], id[1], id[2], id[3]);
    return 0;
}

uint32_t bsp_lcd_width(void) {
    return BOARD_LCD_WIDTH;
}

uint32_t bsp_lcd_height(void) {
    return BOARD_LCD_HEIGHT;
}
