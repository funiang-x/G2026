/* ============================================================================
 *  bsp_lcd.c —— ST7796S 液晶屏 · 8080 16 位并口（FSMC Bank1/NE1）· 见 bsp_lcd.h
 *
 *  本文件只做三件事：
 *    ① 背光开关（普通 GPIO 脚；FSMC 时序由 CubeMX 的 `MX_FSMC_Init()` 配，
 *       本层不重复配 —— 见下方"FSMC 句柄"说明）
 *    ② 屏的硬复位（RESET 引脚时序）
 *    ③ 三个搬运原语：写命令 / 写参数 / 写像素
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

#include "FreeRTOS.h"
#include "semphr.h"

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
 * （现行 **AS=2 / DS=8** / AccessMode A，见 `Core/Src/fsmc.c` 的 `MX_FSMC_Init()`
 *  与 `board_config.h` 的 FSMC 段；**改时序去 CubeMX 改 `.ioc`，不在本层加宏**）
 * ⇒ 本层**不再配第二遍**：重复 HAL_SRAM_Init 没有意义，而且会把
 *    "时序到底在哪定义"这件事变成两个来源。 */
static int s_ready = 0;



/* LCD 总线闸（2026-10-02）：采样窗静默的落地机制，见 bsp_lcd.h 的说明。
 * 互斥量 ⇒ 带优先级继承（写屏侧持闸时 engine 来抢，会让写屏侧临时提权把当前块写完）。
 * NULL = 没建起来 ⇒ take/give 退化为空操作。 */
static SemaphoreHandle_t s_bus_mtx = NULL;

int bsp_lcd_bus_take(uint32_t timeout_ms) {
    if (s_bus_mtx == NULL) {
        return 0; /* 没有闸：不阻塞、不崩（退化为整帧级避让） */
    }
    return (xSemaphoreTake(s_bus_mtx, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) ? 1 : 0;
}

void bsp_lcd_bus_give(void) {
    if (s_bus_mtx != NULL) {
        (void)xSemaphoreGive(s_bus_mtx);
    }
}

/* 常用 8080 命令（MIPI DCS 标准集，ST7796S / ILI9341 / ST7789 共用）
 * ⚠️ `0x2C` 由本层发：LVGL 是把它当**参数**传进 `lcd_send_color()` 的，那一层不吃它
 *    （见 `bsp_lcd_write_pixels()` 与 app_lvgl.c 的 flush 实现）。 */
#define LCD_CMD_RDDID 0x04U /* 读显示 ID（4 字节） */
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

/* ---- 写吞吐：**结论存档**（2026-10-13 六轮量过，2026-10-14 八轮把仪器删了）----
 * 当时的怀疑：写周期走了 BWTR 的复位值（270 周期 = 1.875µs/字），因为
 *   `MX_FSMC_Init()` 里 `ExtendedMode = DISABLE` 且 `HAL_SRAM_Init(..., &Timing, NULL)`
 *   ⇒ HAL 从不写 BWTR。
 * 实测结论（基准 + J-Link 读寄存器）：**怀疑是错的** ——
 *   · 写吞吐实测 **100 ns/字 ≈ 10 Mpx/s**（BTR 理论 76.4 ns/字，多出来的是循环与 AHB）；
 *   · 非扩展模式下 FSMC 的写周期用 **BTR**，BWTR 不参与（与 RM 一致）；
 *   · 于是"波形一卡一卡"的真凶是 **LVGL 的软件渲染**，不是写屏 —— flush 全程只占
 *     ~4% CPU（见 app_lvgl.c 的 `[LVGL] diag`）。
 * ⚠️ 教训留在这里：**"算得通"不等于"就是它"** —— 该量的时候就量。当时若顺着怀疑
 *   去改 `.ioc` 时序，就会在"改完发现没用"的同时还留下一个未验证的配置。
 * ⚠️ 仪器（`lcd_bench_write` / `fsmc_fix_write_timing` / `s_bench_*` /
 *   `bsp_lcd_bench_report` / 每 5s 打的那三个字段）**已删除**：假说被证伪、BWTR 在
 *   非扩展模式下本来就不参与（"显式写一遍免得日后踩雷"是推测性用途），而基准还会
 *   写坏当前 GRAM 窗口 ⇒ 留着只有风险没有收益。 */


/* ------------------------------------------------------------------ 对外 */
int bsp_lcd_init(void) {
    /* ⚠️ 只做"本层该做的"：背光灭 + 屏硬复位。
     *    FSMC 控制器（时钟 / 引脚 AF12 / 时序）由 CubeMX 的
     *    `MX_FSMC_Init()` 完成，它在 main() 里、本函数之前被调用。
     *    这里**不做**任何校验 —— FSMC 配错了在 8080 上是"屏没反应"，
     *    而那是 bsp_lcd_self_check()（读 ID）负责暴露的事。 */
    bsp_lcd_backlight(0); /* 先关背光：屏还没初始化，此刻是白屏/花屏 */
    lcd_hw_reset();

    /* LCD 总线闸：**必须在调度器启动前建**（本函数由组合根在 boot 早期调用）。
     * ⚠️ 同 bsp_adc_init()/app_engine_init()：建完把中断放回来 —— 否则 BASEPRI
     *    被钉住、HAL 时基失效，后面任何 HAL_Delay() 都会死循环（bsp_log.c 记过机理）。 */
    if (s_bus_mtx == NULL) {
        s_bus_mtx = xSemaphoreCreateMutex();
        portENABLE_INTERRUPTS();
    }
    if (s_bus_mtx == NULL) {
        LOG_W("[LCD] 总线闸创建失败 —— 采样窗静默退化为整帧级避让（见 bsp_lcd.h）");
    }

    /* ★ 2026-10-14：这里**曾经**有一个 DMA 像素搬运的初始化（C9③）。
     *   已按用户指示清理 —— 它卡在"`EN` 不能证明总线真的静默"上，而
     *   采样窗静默（O1）不能赌；两次上板失败（ISR 里打日志 / 中断里还互斥量）
     *   的教训留在 `docs/03_LVGL刷屏优化评估-20261014.md §8.1 C`。 */

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

/* ★★ 写像素流：**`0x2C` 只能在整个窗口的第一块之前发一次**（2026-10-12 实测）。
 * 实测证据（J-Link 直接对屏做，脚本 `_work/lcd_flashscreen_20261012/ramwr_restart.jlink`）：
 *   窗口设成 8 列 × 2 行 → 先 0x2C 写 8 个值 0111..0118 → **再 0x2C** 写 0121..0128
 *   → 读回窗口：`0121..0128` + 8 个 `FFFF` ⇒ **第二批把第一批整个覆盖**。
 * ⇒ 本屏每收到一次 `0x2C` 都会把写地址**复位到窗口起点**（不是"接着上次的指针写"）。
 *   所以我们把一次 flush 切成 `APP_LCD_CHUNK_PX` 块时，**绝不能每块都重发 `0x2C`**
 *   —— 那样每块都回到窗口第一行、反复覆写最前面几行，其余行永远是上电随机 GRAM
 *   （现象就是"整屏雪花 + 几条均匀横线上有界面"）。 */
void bsp_lcd_write_pixels(const uint8_t *px, uint32_t len) {
    bsp_lcd_write_cmd(LCD_CMD_RAMWR);
    bsp_lcd_write_pixels_cont(px, len);
}

void bsp_lcd_write_pixels_cont(const uint8_t *px, uint32_t len) {
    /* px 是 RGB565 的**原生小端**字节流（低字节在前），本层把它拼成 16 位字写出去。
     *
     * ★★ 2026-10-14（C9①）**更正一条传了很久的错觉：这里并没有"字节序交换"。**
     *   算术事实（不依赖任何硬件假设）：ARM Cortex-M 恒为小端 ⇒
     *   `(p[1]<<8)|p[0]` **就等于** `*(const uint16_t *)p`（px 两字节对齐时），
     *   也就是一次**原生半字读**。它写出去的值 = 屏要的那个 16 位字 V。
     *   （写成这种形式而不是直接半字读，只是为了不依赖 px 的对齐 —— 值完全相同。）
     *
     *   ⚠️ 这条更正有实际后果，别再把"字节序"当障碍：
     *   **DMA 半字搬运（内存 → FSMC 数据口）得到的是同一个 V** ⇒
     *   「F407 的 DMA 不能交换字节，所以 DMA 刷屏要先解决字节序」是**假障碍**，
     *   一个字节都不用动。（原判断见 `docs/03_LVGL刷屏优化评估-20261014.md` §5.3，
     *   同日更正为"不适用"。）
     *
     * ⚠️ 本函数**不发任何命令**：调用者必须已经发过 `0x2C`，且中间不许再发
     *    （理由见上）。 */
    const uint8_t *p = px;
    uint32_t       n = len / 2U; /* 像素数 */

    for (uint32_t i = 0; i < n; i++) {
        LCD_DAT_WR = (uint16_t)(((uint16_t)p[1] << 8) | (uint16_t)p[0]);
        p += 2;
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

    /* 判据：**四字节全相同 ⇒ 判失败**（这一条同时盖住"全 0 / 全 0xFF"）。
     *
     * ★ 2026-10-09 加固，起因是一次**假通过**：单核心板（屏根本没接）时读到
     *   `04 04 04 04`，原判据只挡"全 0 / 全 0xFF" ⇒ 判成"读到了"，日志打出
     *   `[SELFTEST] LCD ok`，把"屏没接"报成了"屏通了"。
     *   机理：上面刚往 CMD 窗口写了 `0x04`（RDDID 命令本身），而屏没接时数据线上
     *   **没有任何器件驱动** ⇒ FSMC 读到的就是**自己上一次驱动的值在总线电容上的回声**。
     *
     * 两个已知场景被本条干净分开（都是实测值）：
     *   · 屏没接  → `04 04 04 04`（= 命令字节回声）⇒ 四字节全同 ⇒ **拒绝**
     *   · 屏接上  → `00 FF FF FF`（`hardware/04-pinout连接表.md`）⇒ 非全同 ⇒ 通过
     * 真实 ID 也都不是四字节全同（ST7796S=00 77 96 00 · ILI9341=00 93 41 00 ·
     * HX8357=00 D3 …）⇒ 本条**不会误杀真屏**。
     *
     * ⚠️ 仍然**不要**改成"ID 值等于某个期望值"——ST7796S 批次间有差异，兼容片
     *    HX8357 的 ID 完全不同（见 bsp_lcd.h 的说明）。本判据只判"是不是在读一个
     *    真器件"，不判"是哪个器件"。 */
    if (id[0] == id[1] && id[1] == id[2] && id[2] == id[3]) {
        return -1;
    }
    return 0;
}

int bsp_lcd_self_check(void) {
    uint8_t id[4] = {0};

    if (bsp_lcd_read_id(id) != 0) {
        /* 把**读到的原始 4 字节**打出来 —— 这是"屏没接"和"屏接了但时序不对"
         * 唯一的区分点，别只说一句"读不到"：
         *   · 四字节全同（典型 `04 04 04 04`）= 命令字节回声 ⇒ **屏没接 / 没供电**
         *     （数据线上没人驱动，读到的是自己刚写下去的值）；
         *   · 其它组合 = 总线上有器件但 ID 不像话 ⇒ 查 PE/PD 数据线、PD4(NOE)、
         *     PD7(NE1)、PD11(A16) 与 FSMC 时序。
         * 2026-10-09：正是靠这个区分才发现"单核心板时被判成了 LCD ok"。 */
        LOG_W("[SELFTEST] LCD 读不到 ID：读到 %02X %02X %02X %02X"
              "（四字节全同 ⇒ 那是**刚写进去的命令字节的回声**，屏没接 / 没供电；"
              "否则查 PE/PD 数据线、PD4(NOE)、PD7(NE1)、PD11(A16)）",
              id[0], id[1], id[2], id[3]);
        return -1;
    }
    LOG_I("[SELFTEST] LCD ok, RDDID=%02X %02X %02X %02X（能读到即证明 16 位总线 + "
          "RS 地址译码 + 读写时序都通）",
          id[0], id[1], id[2], id[3]);
    return 0;
}
