#ifndef BSP_LCD_H
#define BSP_LCD_H
/* ============================================================================
 *  bsp_lcd —— 3.5″ IPS 320×480 液晶屏（驱动 IC **ST7796S**）· 8080 16 位并口
 *
 *  ---- 这一层负责什么 ----
 *  只做"把命令和像素写进屏"，**不含任何图形逻辑**，也不含屏的初始化序列：
 *    · 初始化序列由 LVGL 自带的 ST7796 驱动发（`lv_st7796_create()`），
 *      它是权威来源（LVGL 9.5 从 LovyanGFX 移植），比手抄 datasheet 可靠；
 *    · 本层提供的是它需要的 4 个原语：写命令 / 写参数 / 开窗 / 写像素。
 *
 *  ---- 为什么走 FSMC ----
 *  320×480×2B = 300KB/帧。SPI @36MHz 要 68ms/帧（14fps，只能局部刷新）；
 *  FSMC 8080 16 位每字约 4 HCLK（27.8ns）⇒ 理论 72MB/s，实测 CPU 直写
 *  约 20~35MB/s，整帧 9~15ms。且**时序全部由 FSMC 硬件产生**，
 *  不需要软件翻任何引脚（命令/数据的区分靠地址线 RS 的地址译码）。
 *
 *  ---- FSMC 控制器配置在哪 ----
 *  **在 CubeMX**（.ioc）：GUI 里 LCD Interface / Data 16 bits / AS=1 DS=15，
 *  生成 `MX_FSMC_Init()`（Core/Src/fsmc.c，全局 hsram1）。本层**不配第二遍**
 *  ——两个真相源比一个错的更糟。本层只消费 board_config.h 登记的
 *  地址译码（CMD/DATA 两个窗口地址）。
 *  （2026-09-28 更新：早先"FSMC 参数进不了 .ioc"是工程根 MXTmpFiles 残留所致，
 *   事后查清；FSMC 参数节的完整说明见 board_config.h。）
 *
 *  ---- ⚠️ 未上板验证 ----
 *  FSMC 时序、MADCTL 旋转值、ST7796 初始化序列都**没在实物上跑过**。
 *  首次上板要按 docs 的"分步点亮"顺序来：先纯色，再 LVGL。
 * ==========================================================================*/
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化：配 FSMC 控制器 + 硬复位屏 + 关背光（等 LVGL 初始化完再开）。
 *
 * 返回 0 = 成功；-1 = HAL_SRAM_Init 失败（FSMC 时钟或时序参数有问题）。
 *
 * ⚠️ **不含屏的初始化序列** —— 那是 LVGL 的活（见文件头）。
 *    所以单独调它屏不会亮，这是预期的。
 * ⚠️ 必须在 `MX_FSMC_Init()`（main 里调）之后调 —— 它只保证引脚已配好。 */
int bsp_lcd_init(void);

/* 读屏的 ID（命令 0x04 RDDID，4 个字节）。
 * 返回 0 = 读失败（RD 没接 / 时序太紧 / 屏没供电）。
 * 判据用法见 bsp_lcd_self_check()。 */
int bsp_lcd_read_id(uint8_t id[4]);

/* 自检：读 ID 并打印。返回 0 = 读到（不管值对不对），-1 = 读不到。
 * ⚠️ 为什么只判"读得到"而不判具体值：ST7796S 的 RDDID 值不同批次有差异，
 *    而且 HX8357 兼容片的 ID 完全不同。**能读到 4 字节**已经证明
 *    RS 地址译码 + 写时序 + 读时序 + 数据总线 16 根全部通 —— 那才是重点。 */
int bsp_lcd_self_check(void);

/* 背光开关（1 = 亮）。⚠️ 屏模块自带升压，这里只是使能，不是调光。 */
void bsp_lcd_backlight(int on);

/* ---- 给 LVGL 的 4 个原语（LVGL 的 ST7796 驱动只依赖这四个）---- */

/* 写一个命令字节（RS = 0）。 */
void bsp_lcd_write_cmd(uint8_t cmd);

/* 写参数/数据字节流（RS = 1）。`len` 是**字节数**，内部按 16 位字搬运。 */
void bsp_lcd_write_data(const uint8_t *data, uint32_t len);

/* 开窗（0x2A 列地址 + 0x2B 页地址）。坐标是**闭区间**，与 LVGL 的 area 一致。 */
void bsp_lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);

/* 写像素流（先写 0x2C，再灌数据）。
 * `px` 是 RGB565 **大端**字节流（高字节在前）—— 屏期望的字节序。
 * ⚠️ 本层**不做字节交换**：交换由 LVGL 的 `LV_COLOR_FORMAT_RGB565_SWAPPED` 完成，
 *    在绘制时顺手做掉，比在搬运时多一遍循环快。 */
void bsp_lcd_write_pixels(const uint8_t *px, uint32_t len);

/* ---- 纯色填充（分步点亮用，不经 LVGL）---- */
void bsp_lcd_fill(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t rgb565);

/* 屏的分辨率（= board_config.h 的 BOARD_LCD_WIDTH/HEIGHT）。 */
uint32_t bsp_lcd_width(void);
uint32_t bsp_lcd_height(void);

#ifdef __cplusplus
}
#endif
#endif /* BSP_LCD_H */
