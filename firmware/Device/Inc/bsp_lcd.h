#ifndef BSP_LCD_H
#define BSP_LCD_H
/* ============================================================================
 *  bsp_lcd —— 3.5″ IPS 320×480 液晶屏（驱动 IC **ST7796S**）· 8080 16 位并口
 *
 *  ---- 这一层负责什么 ----
 *  只做"把命令和像素写进屏"，**不含任何图形逻辑**，也不含屏的初始化序列：
 *    · 初始化序列由 LVGL 自带的 ST7796 驱动发（`lv_st7796_create()`），
 *      它是权威来源（LVGL 9.5 从 LovyanGFX 移植），比手抄 datasheet 可靠；
 *    · 本层提供的是它需要的 3 个原语：写命令 / 写参数 / 写像素。
 *
 *  ---- 为什么走 FSMC ----
 *  320×480×2B = 300KB/帧。SPI @36MHz 要 68ms/帧（14fps，只能局部刷新）；
 *  FSMC 8080 16 位每字约 4 HCLK（27.8ns）⇒ 理论 72MB/s，实测 CPU 直写
 *  约 20~35MB/s，整帧 9~15ms。且**时序全部由 FSMC 硬件产生**，
 *  不需要软件翻任何引脚（命令/数据的区分靠地址线 RS 的地址译码）。
 *
 *  ---- FSMC 控制器配置在哪 ----
 *  **在 CubeMX**（.ioc）：GUI 里 LCD Interface / Data 16 bits / AS=2 DS=8，
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

/* ---- 给 LVGL 的 3 个原语（LVGL 的 ST7796 驱动只依赖这三个）---- */

/* 写一个命令字节（RS = 0）。 */
void bsp_lcd_write_cmd(uint8_t cmd);

/* 写参数/数据字节流（RS = 1）。`len` 是**字节数**，内部按 16 位字搬运。 */
void bsp_lcd_write_data(const uint8_t *data, uint32_t len);

/* 写像素流（先写 0x2C，再灌数据）。
 * `px` 是**原生小端的 RGB565** —— 就是 LVGL 的 `LV_COLOR_FORMAT_RGB565` 放在绘制
 *    缓冲里的样子（`lv_display_set_color_format(s_disp, RGB565)` 定的）。
 * ★ 2026-10-14（C9①）**更正**：本层**不做任何字节序交换**。旧注释说"搬运循环里把
 *   高低字节对调（屏要大端）"是**算错的** —— `(p[1]<<8)|p[0]` 在 ARM 小端上就等于
 *   一次原生半字读（`*(const uint16_t *)p`），写出去的就是屏要的那个 16 位字。
 *   ⚠️ 这条更正有实际后果：**DMA 半字搬运得到的正是同一个值** ⇒
 *      「DMA 要先解决字节序」是假障碍，一个字节都不用动。
 * ⚠️ **一次窗口只许调它一次**：本屏每收到一次 `0x2C` 就把写地址复位到窗口起点
 *    （2026-10-12 实测，见 bsp_lcd.c 的说明）。分块续写用下面的 `_cont`。 */
void bsp_lcd_write_pixels(const uint8_t *px, uint32_t len);

/* 续写像素流：**不发任何命令**，接着上一次的写地址继续灌。
 * 用法：开窗（0x2A/0x2B）→ `bsp_lcd_write_pixels()` 写第一块 → 后续每块用本函数。
 * ⚠️ 中间不许再发 `0x2C`（会复位地址，前功尽弃）；`0x2A/0x2B` 会重新开窗，可以再发。 */
void bsp_lcd_write_pixels_cont(const uint8_t *px, uint32_t len);

/* ★ 2026-10-14 八轮：写吞吐基准那套接口（`bsp_lcd_bench_report()` 与实现里的
 *   `lcd_bench_write`/`fsmc_fix_write_timing`/`s_bench_*`）**已删除**：
 *   它服务的假说（"写周期走了 BWTR 复位值 270 周期"）被实测当场证伪
 *   （**100 ns/字 ≈ 10 Mpx/s**，非扩展模式下 BWTR 本来就不参与），
 *   而基准本身会写坏当前 GRAM 窗口 ⇒ 留着只有风险。结论留在 `bsp_lcd.c` 的注释里。 */

/* 计时：用 `bsp_tick_cyc_now()` / `bsp_tick_cyc_to_us()`（见 bsp_tick.h）——
 * 2026-10-14 八轮从本层搬走：那是通用时基，跟 LCD 无关，家应该在时基层。 */


/* ================= LCD 总线闸（2026-10-02 新增）=================
 * 目的：**采样窗静默**（硬件方案 02 §6 O1）的真正落地机制。一次 flush
 * （480×48px，现行 AS=2/DS=8 ⇒ ≈1.17ms）与 engine 的采样窗 2.048ms 同量级；
 * 只靠"flush 前看一眼标志"挡不住已经开始的写屏。
 *
 * 用法（两边都调这两个函数）：
 *   · engine（app_engine.c）：**开采集之前** take、采完（或启动失败）give；
 *   · 写屏侧（app_lvgl.c 的 flush）：每写 `APP_LCD_CHUNK_PX` 像素 take/give 一次。
 * ⇒ 采样窗内一次 FSMC 写都不会发生；代价是 engine 起采最多晚 1 块（≈0.11ms）、
 *    flush 被推迟 ≤2ms（屏刷新率本就不敏感）。
 *
 * 语义：标准互斥量（带优先级继承 —— 写屏侧持闸时 engine 来抢，会让写屏侧
 * 临时提权把当前块写完再让闸）。`bsp_lcd_bus_take` 超时返回 0。
 * 闸没建起来（init 失败）时两者都是空操作：不阻塞、不崩。
 * ⚠️ 放在 Device 层是因为**总线的主人**在这里；task_root 与 task_ops 都链 device，
 *    放 Task 层会引入 task_root→task_ops 的跨库依赖（那要改 CMake 目标划分）。 */
int  bsp_lcd_bus_take(uint32_t timeout_ms);
void bsp_lcd_bus_give(void);
#ifdef __cplusplus
}
#endif
#endif /* BSP_LCD_H */
