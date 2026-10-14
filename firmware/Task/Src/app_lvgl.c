/* ============================================================================
 *  app_lvgl.c —— LVGL 9.5 适配层（显示 + 触摸）· 见 app_lvgl.h
 *
 *  ---- 一条数据通路（显示）----
 *      LVGL 渲染到 draw buffer（SRAM1）
 *        → lv_lcd_generic_mipi 的 flush_cb
 *            → 我们的 send_cmd  ：0x2A/0x2B 开窗（走 bsp_lcd）
 *            → 我们的 send_color：0x2C + 像素流（走 bsp_lcd）
 *        → lv_display_flush_ready()   ← 漏了这行界面会**永久卡死**（不再重绘）
 *
 *  ---- 内存落点（三条铁律）----
 *    · **LVGL 的堆**：CCM 高半边（32KB）—— 在 lv_conf.h 里由
 *      `BOARD_LVGL_MEM_ADDR` 指定，不占 .bss；
 *    · **draw buffer**：SRAM1 —— 本文件的 `s_draw_buf`。放 CCM 会怎样？
 *      本工程走 FSMC 直写（CPU 搬运，不用 DMA），所以 CCM 其实也能跑；
 *      但仍放 SRAM1，因为**将来若改用 DMA 搬 FSMC**，CCM 会静默失效（不报错）；
 *    · **采集缓冲**：绝不能在 CCM（见 bsp_adc.h）。
 * ==========================================================================*/
#include "app_lvgl.h"

#include "board_config.h"
#include "bsp.h"
#include "log.h"
#include "project_config.h"

#include "lvgl.h"

/* draw buffer：**逻辑宽** × N 行 × 2B。**放 SRAM1**（普通 .bss）。
 * ⚠️ 宽度必须用 `APP_LVGL_SCR_W`（**逻辑**宽 480），不是 `BOARD_LCD_WIDTH`（面板物理宽
 *    320）：LVGL 的 PARTIAL 模式按"逻辑水平分辨率"切块，一行 = 480×2 = 960 B。
 *    2026-10-13 之前界面是竖屏、两者恰好相等（都是 320）所以看不出问题；一改横屏，
 *    用 320 算的话 LVGL 每块只装得下 32 行（30720/960），块数凭空多 1/3。
 * ⚠️ 尺寸不能太小：LVGL 的 PARTIAL 模式下每次重绘至少要能装下"一行"。
 *    APP_LVGL_DRAW_BUF_LINES 越大越省 flush 次数，但也越吃 RAM。 */
static uint8_t s_draw_buf[APP_LVGL_SCR_W * APP_LVGL_DRAW_BUF_LINES * 2U];

/* ★ LVGL 的**第二块内存池**（SRAM1，普通 .bss；尺寸与理由见 board_config.h 的
 *   `BOARD_LVGL_MEM_EXT_KB`）：主池在 CCM 只有 32KB，4 页业务界面把它用满了。
 *   ⚠️ **必须在 `lv_init()` 之后**才 add（之前池子系统还没建）。
 *   ⚠️ 它买的是"运行时余量"：`lv_malloc` 失败只按 INFO 打，本工程日志级 = WARN
 *      ⇒ 池耗尽在屏上是"缺块/卡死"，日志里一个字都没有。
 *   ⚠️ 只对 `LV_USE_STDLIB_MALLOC = LV_STDLIB_BUILTIN` 有效（本工程就是这个口径，
 *      与 lv_conf.h 里 `LV_MEM_SIZE`/`LV_MEM_ADR` 用的是同一道闸）。 */
#if LV_USE_STDLIB_MALLOC == LV_STDLIB_BUILTIN
static uint8_t s_lvgl_pool_ext[BOARD_LVGL_MEM_EXT_KB * 1024U];
#endif

static lv_display_t *s_disp = NULL;
#if APP_TOUCH_ENABLE /* 触摸总开关关闭时不留这个句柄（否则 -Wunused-variable） */
static lv_indev_t   *s_indev = NULL;
#endif
static uint32_t      s_last_ms = 0;
static int           s_tick_started = 0;

/* ------------------------------------------------------------------ 日志 */
/* LVGL 自己的日志 → RTT（LV_USE_LOG=1 才有这条通路，lv_conf.h）。
 * ⚠️ 为什么不用 `LV_LOG_PRINTF = 1`：本工程的 printf **没有重定向**
 *    （syscalls.c 的 `__io_putchar` 是空弱实现），开了等于日志静默丢弃；
 *    而且 printf 无互斥，与 RTT 后端会抢输出。走回调最干净。
 * ⚠️ `buf` 自带结尾换行，所以这里用 %s 会多一个空行 —— 可接受。 */
static void lvgl_log_cb(lv_log_level_t level, const char *buf) {
    /* ★ 2026-10-14 八轮（P0.3）：**级别映射原来是反的**。
     *   LVGL 的枚举是 TRACE(0) < INFO(1) < WARN(2) < **ERROR(3) < USER(4)** < NONE(5)，
     *   而旧写法是 `level <= WARN ? LOG_W : LOG_D` ⇒ **ERROR/USER 被降成 DEBUG**，
     *   再被本工程的 INFO 阈值丢掉 ⇒ `LV_ASSERT_MALLOC` 失败（LVGL 堆耗尽）**一个字不打**。
     *   几轮"屏卡死但日志干净"就是栽在这（`app_ui.c` 文件头那句"日志级=WARN"也早已过期，
     *   实际是 INFO，见 app.c 的 bsp_log_set_level）。现在按级映射，断言恢复可见。 */
    if (level == LV_LOG_LEVEL_ERROR || level == LV_LOG_LEVEL_USER) {
        LOG_E("[LVGL] %s", buf);
    } else if (level == LV_LOG_LEVEL_WARN) {
        LOG_W("[LVGL] %s", buf);
    } else if (level == LV_LOG_LEVEL_INFO) {
        LOG_I("[LVGL] %s", buf);
    } else {
        LOG_D("[LVGL] %s", buf);
    }
}

/* ------------------------------------------------------------------ 显示回调 */

/* LVGL 要发命令 + 参数（开窗、MADCTL、COLMOD 等都走这里）。
 * ⚠️ `cmd` / `param` 是**字节流**，不是 16 位字 —— 8080 的命令参数
 *    一个字节占一次 16 位传输，转换在 bsp_lcd_write_data 里做。 */
static void lcd_send_cmd(lv_display_t *disp, const uint8_t *cmd, size_t cmd_size,
                         const uint8_t *param, size_t param_size) {
    LV_UNUSED(disp);
    for (size_t i = 0; i < cmd_size; i++) {
        bsp_lcd_write_cmd(cmd[i]);
    }
    if (param != NULL && param_size > 0U) {
        bsp_lcd_write_data(param, (uint32_t)param_size);
    }
}

/* LVGL 要发像素流。
 * ⚠️ 开窗（0x2A/0x2B）**已经由 flush_cb 通过 send_cmd 发过了**，这里只发
 *    写显存命令 + 数据 —— 再发一次开窗不会错，但会白费时间。
 * ⚠️ `cmd` 恒为 0x2C（LV_LCD_CMD_WRITE_MEMORY_START）。bsp_lcd_write_pixels
 *    内部自己会发这条命令，所以这里**故意忽略 cmd 参数**。
 * 顺带统计 flush 像素：顶栏的"等效 fps"= 每秒刷屏像素 / 整帧像素 ——
 * 性能可观测（PARTIAL 模式下一次重绘会被切成多块，数"块数"没有意义）。 */
/* flush 像素累加器（**单调递增**）：两个消费者各自差分 ——
 *   ① `[LVGL] diag` 每 5 s 打窗口值（**同时打实际窗口 ms**，分母不再硬编码）；
 *   ② `app_lvgl_flush_kpx_s()` 每秒给页 2 的 BANDWIDTH 卡。
 * ★ 2026-10-14 八轮归一口径：删掉了 2026-10-13 加的第二累加器 `s_flush_px_win`
 *   （它被 5 s 打印清零，而 1 s 消费者也在读 ⇒ 谁清零都会让另一个读出负数），
 *   也删掉了 `app_lvgl_fps_x10()`（它曾给出"超过 FSMC 物理上限"的假吞吐 —— 根因是
 *   判读口径：diag 分母曾硬编码 5s、RTT 上行满 1024B 还会静默丢行）。 */
static uint32_t s_flush_px = 0;
/* ★ flush 自身的耗时（DWT 周期累计，2026-10-13 六轮）：把"渲染"与"写屏"分开。
 * 为什么必须分开：`[UI] 1s` 的分段探针测到 LVGL 一次调用花 ~207ms，而写像素只要
 * 100ns/字（bsp_lcd 的基准）⇒ 想定案就必须知道这 207ms 里有多少是 flush。
 * 判读（见 diag 行）：`flush N µs/s` 若远小于 207ms ⇒ 时间花在**软件渲染**上。 */
static uint32_t s_flush_cyc = 0;
/* `lv_timer_handler()` 自身耗时（DWT 周期累计）：与 flush 一起打印 ⇒ 判"190ms 在不在 LVGL 里"。*/
static uint32_t s_lvth_cyc = 0;
static uint32_t s_flush_calls = 0;  /* 窗口内 flush 调用次数（脏区被切成几块）*/
static uint32_t s_flush_max_px = 0; /* 单次 flush 的最大像素（一块 draw buffer = 23040）*/

/* flush 收尾：记数 + 回握手（两处调用点共用 ⇒ "每块都按同一套口径记账"）。 */
static void lcd_flush_finish(lv_display_t *disp, uint32_t cyc0, uint32_t px) {
    s_flush_px += px;
    s_flush_cyc += (bsp_tick_cyc_now() - cyc0);
    s_flush_calls++;
    if (px > s_flush_max_px) {
        s_flush_max_px = px;
    }
    /* ★ 必须回这一句：LVGL 的 flush 是**异步握手** —— 它发完一块就等
     *   `lv_display_flush_ready()`，等到才继续下一块（见 LVGL 源码
     *   `drivers/display/lcd/lv_lcd_generic_mipi.c` 里那句 note）。
     *   ⚠️ 2026-10-02 上板实测漏了它的现象：屏上只出现**一条白带**（= 一个 draw
     *      buffer），其余保持上电雪花、界面永不出现。"注释不是实现"。
     *   ⚠️ 2026-10-14：本工程的写屏是**同步**的（`bsp_lcd_write_pixels` 是 CPU 直写
     *      FSMC、写完才返回）⇒ 紧接着回 ready 就是正确的。
     *      曾经试过 DMA 异步 flush（那时这句要等双缓冲 + 搬运完成才敢回），
     *      **结论与两条上板教训**见 `docs/03_LVGL刷屏优化评估-20261014.md §8.1 C`。 */
    lv_display_flush_ready(disp);
}

static void lcd_send_color(lv_display_t *disp, const uint8_t *cmd, size_t cmd_size,
                           uint8_t *param, size_t param_size) {
    /* flush 计时：走 bsp_tick 的周期计数器接口（Task 层不引 HAL/DWT）*/
    const uint32_t cyc0 = bsp_tick_cyc_now();

    LV_UNUSED(cmd); /* ★ LVGL 把 0x2C 当参数传进来，本层**不吃**它 —— 0x2C 由 bsp_lcd
                     *   的写像素原语发（**一整带只发一次**，见其注释） */
    LV_UNUSED(cmd_size);


    /* ---- 分块写 + 每块抢一次 LCD 总线闸（2026-10-02）----
     * 采样窗静默要求"采样窗内 FSMC 一次都不动"，而一次 flush（480×48px，现行
     * FSMC 时序 AS=2/DS=8 ⇒ 76.4 ns/字 ⇒ **≈1.17 ms**）与采样窗（2.048 ms）同量级
     * ⇒ 只有切成 `APP_LCD_CHUNK_PX` 像素一块、每块**开写前**抢闸，engine 才能在
     * 块边界把总线拿过去。抢不到就在块边界阻塞，等采样窗结束再接着写
     * （flush 被推迟 ≤2ms，屏刷新率本就不敏感）。
     * 闸与理由见 bsp_lcd.h 的"LCD 总线闸"、project_config.h 的 APP_LCD_CHUNK_PX。
     * ⚠️ 多次调 bsp_lcd_write_pixels 会重复发 0x2C —— 无害（控制器接着当前
     *    GRAM 指针继续写），只是每块多一次命令开销。 */
    const uint32_t chunk = APP_LCD_CHUNK_PX * 2U; /* RGB565：2 B/px */
    uint32_t       off   = 0;
    int            first = 1;

    while (off < (uint32_t)param_size) {
        uint32_t n = (uint32_t)param_size - off;
        if (n > chunk) {
            n = chunk;
        }
        /* ★ 2026-10-14 八轮（P0.1）：**take 成功才 give**（原来是无条件 give）。
         *   闸被 engine 持有时（正是 take 会超时的那种情形）无条件 give 会**真的把锁放掉**
         *   ⇒ engine 的 2.048ms 采样窗失去互斥、采样窗静默（O1）静默失效；engine 之后那次
         *   give 还会变成多余的第二次 give。engine 侧早就是这条规则（见 app_engine.c 的
         *   acquire_frame），写侧一直没跟上。
         *   ⚠️ 没抢到闸时**照常写**（只是没有互斥）—— 这是既有的退化口径
         *      （见 project_config.h 的 APP_LCD_BUS_WAIT_MS 段），不是"拿不到就不写"。 */
        const int got_bus = bsp_lcd_bus_take(APP_LCD_BUS_WAIT_MS);
        /* ★ `0x2C` 只在**第一块**发：本屏每收到一次就把写地址复位到窗口起点，
         *   分块里重发 ⇒ 每块都回到第一行、反复覆写最前面几行，其余行永远不写
         *   （2026-10-12 实测，见 bsp_lcd.c）。后续块用 `_cont` 续写。 */
        if (first != 0) {
            bsp_lcd_write_pixels(param + off, n);
            first = 0;
        } else {
            bsp_lcd_write_pixels_cont(param + off, n);
        }
        if (got_bus != 0) {
            bsp_lcd_bus_give();
        }
        off += n;
    }
    /* ★ 每次 flush 的像素数分布（2026-10-13 六轮）：LVGL 9 把"脏区矩形"藏在
     * private 结构里、公共 API 拿不到（不动 vendored LVGL）⇒ 用**可量化的代理**：
     * 数"调用次数 + 单次最大"就能反推"一次刷新到底重绘了多少面积"
     * （例：11 次 × 23040 ≈ 253k px ≈ 1.7 个整屏）。记数与收尾统一在
     * `lcd_flush_finish()` 里（两条路共用，保证 A/B 可比）。 */
    lcd_flush_finish(disp, cyc0, (uint32_t)param_size / 2U);
}

/* flush 像素速率（**kpx/s**，1 秒窗口）—— 页 2 的 BANDWIDTH 卡读它。
 * 口径 = 这一秒里"交给屏的像素数 / 1s"，静屏时只有 sysmon 浮层的零星重绘（kpx/s 级）。
 * ⚠️ 它**不是帧率**：增量绘制下这个数本来就应该小（打满整屏 153.6 kpx × 60 才会很大）。
 * ⚠️ 与 `[LVGL] diag` 共用**同一个累加器** `s_flush_px`，各自差分 —— 谁都不要去清零它。 */
uint32_t app_lvgl_flush_kpx_s(uint32_t now_ms) {
    static uint32_t s_win_ms = 0;
    static uint32_t s_win_px = 0;
    static uint32_t s_kpx = 0;
    static uint8_t  s_init = 0;

    if (s_init == 0U) {
        s_init = 1U;
        s_win_ms = now_ms;
        s_win_px = s_flush_px;
        return 0U;
    }
    const uint32_t dt = now_ms - s_win_ms;
    if (dt < 1000U) {
        return s_kpx;
    }
    const uint32_t px = s_flush_px - s_win_px; /* 无符号差分，天然处理回绕 */
    s_kpx = (uint32_t)((uint64_t)px / (uint64_t)dt); /* px/ms == kpx/s */
    s_win_ms = now_ms;
    s_win_px = s_flush_px;
    return s_kpx;
}

/* ------------------------------------------------------------------ 触摸回调 */
#if APP_TOUCH_ENABLE

/* ★ 2026-10-13 四轮加的四样东西（起因：上板"复位后帧率要么稳定 40、要么稳定 90；
 *   40 那一档触摸几秒后失效"）
 *   ① **可观测**：按下 / 抬起 / 读失败 / **每秒一行摘要**。在这之前"触摸不通"
 *      在 RTT 里**一个字都没有**，只能靠猜"是没读到按"还是"一直按着"。
 *   ② **卡死保护（2026-10-13 五轮修正判据）**：FT6336 若一直报按下，LVGL 会把指针
 *      当成"按住不放" ⇒ 手指不动也持续拖 tileview（每帧重绘 ⇒ fps 进 90 那一档），
 *      而真实点击再也进不来（"触摸失效"）。保护 = 判卡死就**强制抬起**并压制，直到
 *      读到一次真正的抬起。
 *      ⚠️⚠️ 判据必须是「**按着却连续读不到有效点**」，**绝不能是"按得久"**：
 *      四轮那版写成"单次按下超过 1.2s 就判卡死"，结果**把真实长按全掐了** ——
 *      本界面有 `HOLD TO CAL` 长按，人按住不放本来就会超过 1.2s；2026-10-13 五轮
 *      RTT 实测：手指一按下去 1.2s 就被判卡死并压制 ⇒ 用户看到的现象正是
 *      **"触摸几秒后失效"**。真手指按住时**点会持续到来**（实测：按住期间每秒
 *      40+ 个有效点），所以只有"连点都读不到"才是真卡死；阈值 3s。
 *   ③ **两拍确认（去抖）**：单拍的"按下"**不上报**，必须连续两拍都读到按才认。
 *      why：总线垃圾会蹦出孤立的单拍假按下，它足以把一次真实点击搅掉；
 *      真手指按住几百毫秒，多等一拍（≈16ms）无感。
 *   ④ **读失败宽限窗**：读失败 **≠ 松手** —— LVGL 只认连续的按下序列才认拖动，
 *      "失败就报抬起"会把正在进行的滑动打断（现象："怎么滑都不换页"）。
 *   ⚠️ 本函数在**触摸路径**上，不在 flush 路径上（flush 一个字没动）。 */
#define TOUCH_HOLD_MAX_MS 3000U     /* 卡死判据：**按着却连续这么久没有有效点**（见 ②）*/
#define TOUCH_LOG_MIN_MS 200U       /* 日志去抖：人手再快也没这么密 */
#define TOUCH_FAIL_GRACE_MS 200U    /* 读失败宽限窗（见 ④）*/
#define TOUCH_STAT_PERIOD_MS 1000U  /* 每秒摘要周期 */

static uint32_t s_hold_ms = 0;   /* 本次按下的起始时刻 */
static uint32_t s_valid_ms = 0;  /* 最近一次**读到有效点**的时刻（卡死判据用，见 ②）*/
static uint8_t  s_hold_on = 0;   /* 1 = 正在按 */
static uint8_t  s_arm = 0;       /* 1 = 已看到一拍"按下"，等下一拍确认（③）*/
static uint8_t  s_stuck = 0;     /* 1 = 已判卡死，等一次真正的抬起 */
static uint32_t s_log_ms = 0;    /* 上一条触摸日志的时刻（去抖用）*/
static uint32_t s_err_cnt = 0;   /* 窗口内 I2C 读失败次数（5s 汇总用）*/
static uint32_t s_err_ms = 0;    /* 上一次汇总打印的时刻 */
static uint32_t s_fail_ms = 0;   /* 当前这串读失败的首个时刻（0 = 不在失败串里）*/
static lv_coord_t s_last_x = 0;  /* 最近一次**成功**读到的点（失败期间沿用）*/
static lv_coord_t s_last_y = 0;
static uint32_t s_sum_ms = 0;    /* 每秒摘要的时刻 */
static uint32_t s_sum_n = 0;     /* 窗口内采样次数 */
static uint32_t s_sum_fail = 0;  /* 窗口内读失败次数 */
/* ★ 2026-10-14：**一次 I²C 读到底多久**的探针（只统计真正读了的那些次，INT 门跳过的不算）。
 * 为什么要有它：这个数在 `docs/03` 里一直是个谜 —— 文档记 0.7ms，实测摊出来 **2.34ms**，
 * 差 3 倍，挂着"待查"。I²C 从 **100kHz 提到 400kHz** 之后，这一行就是**判据**：
 * 读耗时应掉到 ~1/4 附近；若几乎不动，说明瓶颈不在波特率（那就要查 HAL 的轮询/超时开销）。 */
static uint32_t s_read_cyc = 0;
static uint32_t s_read_n = 0;

/* ★ 2026-10-14 C7-B 探针：**面板 INT（PC5）的电平** —— 判"能不能用 INT 做门"。
 *
 * why：静屏实测**触摸轮询常驻 23.4% CPU**（关触摸后 `handler` 26% → **1.9%**，
 *   `docs/03_LVGL刷屏优化评估-20261014.md` §5.10；摊到每次读 2.34 ms，比文档记的 0.7 ms 高 3 倍）。
 *   若"**没触摸时 INT 为高、按下时变低**"成立，就能**先读这个 GPIO（纳秒级）再决定要不要读 I²C**
 *   ⇒ 静止时那 23% 归零，而触摸时仍按 100 Hz 读，**不牺牲跟手**。
 * ⚠️ 危险前提：FT6336 的 `G_MODE` 若停在"轮询模式"，INT 可能**一直不动** ⇒
 *   那一招会把触摸彻底关死。**所以本轮只读电平打日志**（不影响触摸功能），
 *   按一下 / 松一下各看几秒再决定。
 * ⚠️ PC5 由 CubeMX 配成 `GPIO_Input + PULLUP`（`.ioc` 的 `PC5.GPIO_Label=TP_INT`），
 *   本层只**读**、不配置 —— 重复配置会与生成物打架。 */
static const bsp_gpio_t s_tp_int = {(bsp_gpio_port_t)BOARD_TOUCH_INT_PORT, BOARD_TOUCH_INT_PIN};

/* 每秒一行触摸摘要：**"是不是有个假手指一直按着"**在日志里要能直接看见。
 * 判读：没人碰屏时应是 `状态 抬起 · 按住 0 ms`；若写着 `按下` 且坐标半天不动
 * ⇒ 那就是假手指（它会拖屏、并吃掉真实点击）。 */
static void touch_stat_tick(uint32_t now) {
    if (s_sum_ms == 0U) {
        s_sum_ms = now;
        return;
    }
    if ((uint32_t)(now - s_sum_ms) < TOUCH_STAT_PERIOD_MS) {
        return;
    }
    LOG_I("[TOUCH] 1s: 采样 %lu · 失败 %lu · I2C 读均 %lu us/%lu 次 · 状态 %s · 按住 %lu ms · 坐标 (%d,%d) · INT=%d",
          (unsigned long)s_sum_n, (unsigned long)s_sum_fail,
          (unsigned long)((s_read_n != 0U) ? (bsp_tick_cyc_to_us(s_read_cyc) / s_read_n) : 0U),
          (unsigned long)s_read_n,
          (s_stuck != 0) ? "压制" : ((s_hold_on != 0) ? "按下" : "抬起"),
          (unsigned long)((s_hold_on != 0) ? (now - s_hold_ms) : 0U), (int)s_last_x,
          (int)s_last_y, bsp_gpio_read(s_tp_int));
    s_sum_ms = now;
    s_sum_n = 0;
    s_sum_fail = 0;
    s_read_cyc = 0;
    s_read_n = 0;
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
    bsp_touch_point_t pt;
    const uint32_t now = bsp_tick_ms();

    LV_UNUSED(indev);
    s_sum_n++;

    /* ★★ 2026-10-14 C8：**INT 门** —— 没触摸就不碰 I²C。
     *
     * 依据（**已上板实测**，`docs/03_LVGL刷屏优化评估-20261014.md` §5.10）：
     *   · PC5(TP_INT) 与触摸状态**逐秒 100% 一致**（抬起=1 / 按下=0，45 s 零反例）；
     *   · 100 Hz 空轮询**常驻 22~23% CPU** —— 关掉触摸后静屏 `handler` 从 **22.7% 掉到 1.9%**；
     *   · 且那 45 s 里 100 Hz 的读**不会**把 INT 拉高 ⇒ 它是**跟随触摸状态的电平**，
     *     不是"读一次就复位"的脉冲（否则门会在按住期间误判松手）。
     *
     * 语义：`INT=1`（面板说没触摸）**且**不在拖动中 ⇒ 直接报"抬起"，跳过 I²C。
     *
     * ⚠️⚠️ 条件里这两个 `== 0` **缺一不可**，否则会把触摸弄坏：
     *   · `s_hold_on != 0`（上一次是按下）时必须**照读** —— 否则松手那刻 INT 若抖一下，
     *     拖动会被误判成抬起（跟手断）；
     *   · `s_stuck != 0`（假手指压制态）时必须**照读** —— 压制态**只能靠"读到真正的抬起"
     *     解除**（见下面 `hit == 0` 分支），而压制态下 `s_hold_on` 已被清零
     *     ⇒ 若被门挡住，那次解除读永远来不了，**触摸会永久失效**。
     * ⚠️ 换模组时的判据：`[TOUCH] 1s` 的 `INT=` 列必须随手指变 0；若恒为 1，门会让触摸失效。
     *   （PC5 是**面板的输出**，本层只读、不配置 —— 配置在 CubeMX 的 `PC5.GPIO_Label=TP_INT`。）*/
    if (s_hold_on == 0 && s_stuck == 0 && bsp_gpio_read(s_tp_int) == 1) {
        touch_stat_tick(now);
        data->point.x = s_last_x;
        data->point.y = s_last_y;
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    /* ★ 2026-10-14：量一次 I²C 读的耗时（理由见上面 s_read_cyc 的说明）*/
    const uint32_t rc0 = bsp_tick_cyc_now();
    const int hit = bsp_touch_read(&pt);
    s_read_cyc += (bsp_tick_cyc_now() - rc0);
    s_read_n++;
    touch_stat_tick(now);

    if (hit < 0) { /* I2C 失败：计数，5s 汇总一行（别把日志刷爆）*/
        s_err_cnt++;
        s_sum_fail++;
        if (s_err_ms == 0U || (uint32_t)(now - s_err_ms) >= 5000U) {
            LOG_W("[TOUCH] 5s 内 I2C 读失败 %lu 次（总线被写屏噪声打断？见 bsp_touch.c）",
                  (unsigned long)s_err_cnt);
            s_err_cnt = 0U;
            s_err_ms = now;
        }
        /* ④ 读失败 ≠ 松手：宽限窗内沿用上一次的状态与坐标（拖动不断）；
         *    超窗才按抬起处理（真松手不会被卡住；极端卡死另有 ② 兜底）。 */
        if (s_fail_ms == 0U) {
            s_fail_ms = now;
        }
        if ((uint32_t)(now - s_fail_ms) <= TOUCH_FAIL_GRACE_MS) {
            data->point.x = s_last_x;
            data->point.y = s_last_y;
            data->state =
                (s_hold_on != 0) ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
            return;
        }
        /* 失败太久：落到下面的"抬起"分支（与真抬起同一条路）*/
    } else {
        s_fail_ms = 0U; /* 这串失败结束 */
    }

    if (hit == 1) {
        s_last_x = pt.x;
        s_last_y = pt.y;
        s_valid_ms = now; /* ★ 有有效点 ⇒ 触摸是活的（卡死判据看的是这个时间戳）*/
        if (s_stuck != 0) {
            data->state = LV_INDEV_STATE_RELEASED; /* 压制中：一律报抬起 */
            return;
        }
        if (s_hold_on == 0) {
            /* ③ 两拍确认：第一拍只"上膛"，第二拍才上报 */
            if (s_arm == 0) {
                s_arm = 1;
                data->point.x = pt.x;
                data->point.y = pt.y;
                data->state = LV_INDEV_STATE_RELEASED;
                return;
            }
            s_hold_on = 1;
            s_hold_ms = now;
            if (s_log_ms == 0U || (uint32_t)(now - s_log_ms) >= TOUCH_LOG_MIN_MS) {
                s_log_ms = now;
                LOG_I("[TOUCH] 按下 (%u,%u)", (unsigned)pt.x, (unsigned)pt.y);
            }
        } else if ((uint32_t)(now - s_valid_ms) > TOUCH_HOLD_MAX_MS) {
            /* 按着，但**连续 3s 一个有效点都没读到** ⇒ 这才是卡死。
             * 真手指按住时点会持续到来（s_valid_ms 一直在刷新）⇒ 长按永不误判。 */
            s_hold_on = 0;
            s_stuck = 1;
            LOG_W("[TOUCH] 按着但连续 %u ms 读不到有效点 —— 判卡死：强制抬起并压制，"
                  "等一次真正的抬起（触摸 IC 卡住 / 总线被写屏噪声打断？）",
                  (unsigned)TOUCH_HOLD_MAX_MS);
            data->state = LV_INDEV_STATE_RELEASED;
            return;
        }
        data->point.x = pt.x;
        data->point.y = pt.y;
        data->state = LV_INDEV_STATE_PRESSED;
        return;
    }

    /* hit == 0：真的抬起了（本次读失败也按抬起处理 —— 不把 I2C 抖动变成假点击）*/
    s_arm = 0; /* ③ 松开后重新上膛 */
    if (s_stuck != 0) {
        s_stuck = 0;
        LOG_I("[TOUCH] 读到真正的抬起 —— 解除触摸压制");
    } else if (s_hold_on != 0) {
        s_hold_on = 0;
        if (s_log_ms == 0U || (uint32_t)(now - s_log_ms) >= TOUCH_LOG_MIN_MS) {
            s_log_ms = now;
            LOG_I("[TOUCH] 抬起（按住 %u ms）", (unsigned)(now - s_hold_ms));
        }
    }
    data->state = LV_INDEV_STATE_RELEASED;
}
#endif /* APP_TOUCH_ENABLE */

/* ------------------------------------------------------ 面板专属初始化序列 ----
 * ★★ 为什么在 LVGL 之后**再发一条**（2026-10-12 现场 A/B 实测：发完它屏的行为立刻变了）：
 *   LVGL 的 `init_cmd_list` 是 LovyanGFX 的**通用** ST7796 值 —— **整条不发 `0xC0`(PWR1)**，
 *   且 `0xC1/C2/C5` 与屏厂值不同 ⇒ 本面板的 AVDD/AVCL/VGHS/VGLS 走芯片默认值，
 *   模拟驱动不足（实测现象：GRAM 写得好好的，屏只扫出一两行）。
 *
 * 数据来源（一手，逐字节对照，不凭记忆）：
 *   `docs/TK035F5687_9629_整理资料_(含SPI及并口版)/02_STM32例程/TK035F9629_ST7796版/
 *    STM32_16位并口_标准库_电容触摸/project/bsp/LCD/LCD.c` 的 `Lcd_Initialize()`
 *   （该文件 GBK 编码；抽取脚本 `_work/lcd_flashscreen_20261012/gen_vendor_init.py`）
 *
 * 只补"屏厂有、LVGL 没有或值不同"的那几条；**收尾顺序照屏厂**
 * （SLPOUT → 电压/伽马 → DISPON → INVON → COLMOD → MADCTL）—— 后面三条由
 * `lv_st7796_set_invert()` / `set_color_format` / `set_rotation` 紧接着发出，顺序天然一致。
 * 格式同 LVGL：`命令, 参数个数, 参数…`；`LV_LCD_CMD_DELAY_MS` 的单位是 **10 ms**。
 * ⚠️ 改这里就是改屏的行为，改完必须上板看（现场记录 `_work/lcd_flashscreen_20261012/`）。 */
static const uint8_t s_panel_init_panel[] = {
    LV_LCD_CMD_EXIT_SLEEP_MODE, 0,   /* 0x11 SLPOUT */
    LV_LCD_CMD_DELAY_MS, 12,         /* 120 ms */
    0xF0, 1, 0xC3,                   /* CSCON：开扩展命令 2（下面几个寄存器在其后） */
    0xF0, 1, 0x96,
    0xB4, 1, 0x01,                   /* INVCTR：1-dot 反转（与 LVGL 同值，照抄） */
    0xB7, 1, 0xC6,                   /* EM/Entry Mode（屏厂值；LVGL 整条不发） */
    0xB9, 1, 0x02,                   /* （屏厂值；LVGL 整条不发） */
    0xC0, 2, 0xF0, 0x65,             /* PWR1：AVDD/AVCL=0xF · VGHS=3/VGLS=2 ★LVGL 整条不发 */
    0xC1, 1, 0x15,                   /* PWR2：VRH（LVGL 发 0x06） */
    0xC2, 1, 0xAF,                   /* PWR3（LVGL 发 0xA7） */
    0xC5, 1, 0x22,                   /* VCOM（LVGL 发 0x18） */
    0xE8, 8, 0x40, 0x8A, 0x00, 0x00, 0x29, 0x19, 0xA5, 0x33, /* DOCA（与 LVGL 同值） */
    0xE0, 14, 0xD0, 0x04, 0x08, 0x09, 0x08, 0x15, 0x2F,
              0x42, 0x46, 0x28, 0x15, 0x16, 0x29, 0x2D,      /* 正伽马（屏厂值） */
    0xE1, 14, 0xD0, 0x04, 0x09, 0x09, 0x08, 0x15, 0x2E,
              0x46, 0x46, 0x28, 0x15, 0x15, 0x29, 0x2D,      /* 负伽马（屏厂值） */
    0xF0, 1, 0x3C,                   /* CSCON：关扩展命令 2 */
    0xF0, 1, 0x69,
    LV_LCD_CMD_DELAY_MS, 5,          /* 50 ms */
    LV_LCD_CMD_SET_DISPLAY_ON, 0,    /* 0x29 DISPON */
    LV_LCD_CMD_DELAY_MS, LV_LCD_CMD_EOF
};


/* ------------------------------------------------------------------ 对外 */
int app_lvgl_init(void) {
    /* ① 屏的硬复位与背光控制在 Device 层（bsp_lcd_init）—— 这里只开背光，
     *    等 LVGL 把屏初始化完再开，否则用户会看到一段花屏。 */
    if (bsp_lcd_init() != 0) {
        LOG_E("[LVGL] bsp_lcd_init 失败");
        return -1;
    }

    /* ② LVGL 内核。⚠️ 必须在创建任何对象之前；日志回调先挂上，
     *    lv_init() 自己的告警才不会漏。 */
    lv_log_register_print_cb(lvgl_log_cb);
    lv_init();

    /* ★ 第二块池（见上面 s_lvgl_pool_ext 与 board_config.h 的 BOARD_LVGL_MEM_EXT_KB）。
     *   位置有讲究：**必须在 `lv_init()` 之后**（之前池子系统还没建），
     *   又**必须在建任何对象之前**（否则那批对象只能落在主池里）。 */
#if LV_USE_STDLIB_MALLOC == LV_STDLIB_BUILTIN
    (void)lv_mem_add_pool(s_lvgl_pool_ext, sizeof(s_lvgl_pool_ext));
#endif

    /* ⚠️ 本函数跑在**调度器启动前**（app_init 里），下面这句是那次踩坑的代价
     *    （2026-10-02 上板实测，缺它就起不来）：
     *    `lv_st7796_create()` 的初始化序列里有 `lv_delay_ms()`，而它默认等的是
     *    `lv_tick_inc()` 推进的内部计数 —— 那要调度器起来后的任务来推 ⇒ 死等。
     *    把 tick 源接到 Device 层的毫秒时基（TIM6），调度器前后都能走。
     *    （`lv_tick_inc` 随之成为空操作，留着无害，见 app_lvgl_task。）
     *    ⚠️ 这里**不再**需要清 BASEPRI：lv_conf.h 已把 LV_USE_OS 定为 LV_OS_NONE，
     *       LVGL 不再建/取 FreeRTOS 互斥量，也就不会把中断钉住。 */
    lv_tick_set_cb(bsp_tick_ms);

    /* ③ 显示：用 LVGL 自带的 ST7796 驱动（初始化序列是权威来源，见文件头）。
     * ⚠️ `LV_LCD_FLAG_*` 决定 MADCTL 的镜像/红蓝序：
     *      上下颠倒 → LV_LCD_FLAG_MIRROR_Y · 左右颠倒 → LV_LCD_FLAG_MIRROR_X
     *    **只改这一处** —— 同时改别处（比如 bsp_lcd）会让问题变成两个来源。
     *
     * ★ 2026-10-14（七轮）：`NONE → **BGR**`。现场实测："模拟器里蓝色（波形/强调色），
     *   屏上显示成**黄色**" —— 强调色 0x4CC9F0 的 RGB565 = 0x4F1E（R9 G50 B30），
     *   **R/B 对调后**变成 R30 G50 B9 = 黄绿色 ⇒ 与"蓝变黄"完全对上 ⇒ 面板当前把
     *   数据当 RGB 解、而实际通路是 BGR（横屏 rotation 之后 MADCTL 这一位没跟着调）。
     *   设 BGR 位 = 告诉面板"进来的是 BGR"，由它内部换回来。
     *   ⚠️ 判据（一次就能分清）：改后蓝色应恢复成蓝；若反而变成**蓝↔红**，说明反了，
     *      把这里改回 `LV_LCD_FLAG_NONE`。 */
    s_disp = lv_st7796_create(BOARD_LCD_WIDTH, BOARD_LCD_HEIGHT, LV_LCD_FLAG_BGR,
                              lcd_send_cmd, lcd_send_color);
    if (s_disp == NULL) {
        LOG_E("[LVGL] lv_st7796_create 失败 —— 内存池太小？看 LV_MEM_SIZE");
        return -1;
    }

    /* ★★ 面板专属初始化（屏厂序列；理由与数据来源见 s_panel_init_panel 的注释）。
     * 放在这里的原因：必须在 LVGL 自己的序列**之后**（覆盖它的 C1/C2/C5），
     * 又必须在 INVON/COLMOD/MADCTL **之前**（复现屏厂的收尾顺序）。 */
    lv_st7796_send_cmd_list(s_disp, s_panel_init_panel);

    /* ★★ 面板反显：必须发一条 INVON(0x21)，否则整屏颜色取反。
     *    how：2026-10-02 用 J-Link 绕过 LVGL 直接往面板 GRAM 写已知值，实测
     *    "写进去的是补色"—— 写 0xFFFF(白)→显黑、写 0xF800(红)→显青、
     *    写 0x07E0(绿)→显洋红、写 0x001F(蓝)→显黄，4/4 全中；
     *    同一时刻把 LVGL 的 draw buffer 抓出来在 PC 上还原，画面是**正常的浅色 UI**
     *    ⇒ 渲染/取数/字节序都对，坏的只有"面板显示极性"这一处。
     *    面板上电默认 INVOFF（`lv_lcd_generic_mipi_create` 不碰 INV），
     *    这块模块要 INVON ⇒ 补这一条。
     *    ⚠️ 只改这里：不要去 bsp_lcd 或 MADCTL 里动颜色（会让问题变成两个来源）。
     *    ⚠️ 若拿到的模块批次相反（补这条后反而取反了），把 true 改成 false。 */
    lv_st7796_set_invert(s_disp, true);

    /* ④ 颜色格式：**原生 RGB565**（2026-10-13 六轮改，原为 RGB565_SWAPPED）。
     *  why 改：`_SWAPPED` 让 LVGL 走 swapped 的混合路径 ⇒ 当时实测"每像素 ~2.2µs
     *  （≈320 周期）"这个荒谬的单位成本（handler 0.63 s/s 而 flush 只 24ms/s）。
     *  改成原生格式后渲染走 LVGL 的**标准快路径**。
     *  ⚠️ 2026-10-14（C9①）更正：这里**没有**什么"字节序交换挪到写屏那一步" ——
     *  `bsp_lcd_write_pixels_cont` 的拼字表达式在算术上等于一次原生半字读，
     *  不是交换（详见该函数注释）。
     *  ⚠️ 但格式与拼字表达式是**绑定**的：`_SWAPPED` 下缓冲是[高,低]，
     *  该表达式会算出**字节对调后**的值 ⇒ 必须同时把它改成 `(p[0]<<8)|p[1]`。
     *  （判据：字节对调后画面不是"红蓝互换"，而是**整片乱色**。）
     *  ⚠️ `_SWAPPED` 到底贵在哪还有疑问：七轮基线里改成原生之后仍 ≈2.0µs/px、
     *  根因当时被判为"面积 × 算法"，且 LVGL 9.5 有格式原生的 swapped 混合实现。
     *  ⇒ 见 `_work/lvgl_perf_20261014/` 与 `docs/03_…` §5.2 的"未定案"。 */
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);

    /* ⑤ draw buffer：PARTIAL 模式（只重绘脏区，320×480 全帧 300KB 装不下）。
     *   单缓冲：`buf1 = s_draw_buf`、`buf2 = NULL`。
     *   ⚠️⚠️ 2026-10-14 清理旧实验时**这一句被误删过一次**（清理脚本删
     *   `#if APP_LVGL_DMA … #endif` 整块，把 `#else` 分支里的本调用一起删了）：
     *   症状 = **屏完全不显示**，而**编译不报错** —— 因为 `s_draw_buf` 之后只被
     *   `sizeof()` 引用（编译期常量，不需要它的存储）⇒ 被 `--gc-sections` 回收，
     *   RAM 凭空少 **53.8 KB**。**账就是这么对上的**（教训：删条件编译块必须认 `#else`）。 */
    lv_display_set_buffers(s_disp, s_draw_buf, NULL, sizeof(s_draw_buf),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    /* ★ 显示方向：**固定横屏 480×320**（2026-10-13 起）。
     *
     *   为什么不再条件编译：2026-10-09 那条"demo=横屏 / 业务=竖屏"的开关，前提是
     *   业务 UI 按竖屏排版。现在业务 UI 已按横屏重排（`app_ui.c`：左侧竖排页签 +
     *   右侧内容，尺寸一律取 `APP_LVGL_SCR_W/H`），demo 也早不用了 ⇒ **两边同向**，
     *   那个 `#if` 已无事可做（留着只会让人以为它还有意义）。
     *
     *   ★★ 2026-10-13 上板定稿 **ROTATION_270**，不是 90：
     *      先按"顺时针 90°"设成 90，上板一看整幅画面是**倒的**（相对人眼期望差 180°）
     *      ⇒ 再补 180° = 270。**270 与 90 都是横屏**（逻辑分辨率仍是 480×320），
     *      两者只差内容方向 180°；面板物理尺寸没变，所以 draw buffer 不用动。
     *   ⚠️ 触摸**不需要跟着改**：`lv_indev_read()` 每一拍都按**当前** rotation 重新换算
     *      （`lv_display_rotate_point()` 里 270 有自己的分支）⇒ 方向一改，触摸自动跟上。
     *      若哪天发现"点左边亮右边"这类**镜像**（而不是整体倒 180°），那是另一个问题：
     *      改 `lv_st7796_create()` 的 `LV_LCD_FLAG_MIRROR_X/Y`，**不要**再动 rotation。
     *   ⚠️ 换方向时**只改这一处**，别去动 MADCTL / bsp_lcd，否则问题变成两个来源。 */
    lv_display_set_rotation(s_disp, LV_DISPLAY_ROTATION_270);

    /* ★ 重绘周期：把 LVGL 的显示刷新定时器从 lv_conf.h 的 16ms 收到
     *   `APP_LVGL_REFR_PERIOD_MS`(10ms) —— 这是**帧率的第一瓶颈**
     *   （sysmon 报的 FPS 上限 = 1000/period，见 project_config.h 同名宏段）。
     *   ⚠️ 只动"重绘"这一个定时器，不去改 lv_conf.h 的 `LV_DEF_REFR_PERIOD`：
     *      那是全局初值，还会连带把 indev 的读定时器（触摸采样率）一起改掉。 */
    {
        lv_timer_t *refr = lv_display_get_refr_timer(s_disp);
        if (refr != NULL) {
            lv_timer_set_period(refr, APP_LVGL_REFR_PERIOD_MS);
        }
    }


    /* ⑥ 触摸：FT6336 走 I2C2。初始化失败也继续 —— 只显示不触摸是可用的降级。 */
#if APP_TOUCH_ENABLE
    /* ★ **不管首次探测成功没有，都建 indev**（2026-10-13 四轮改）：
     *   `bsp_touch_read()` 里有一句"每秒重试一次 bsp_touch_init()" ⇒ 台面上把触摸线
     *   插好 / 换颗模块 / 给模块断电重上电，**不用重启整机**就能恢复。
     *   旧写法是"探测失败就不建 indev"—— 于是那次失败之后**再没有人去读它**，
     *   触摸死到下次重启（板上实测踩到：开机一下没通，后面通了也没人管）。 */
    /* ★ 2026-10-14 S4：**触摸采样率 16ms → `APP_TOUCH_READ_PERIOD_MS`(10ms)**。
     *   why：滑动的"跟手度"上限由触摸采样率决定 —— 手指动了最多要等一个采样周期
     *   才被读到，再等一帧才画出来。`lv_indev` 的读定时器默认跟 `LV_DEF_REFR_PERIOD`
     *   （`indev/lv_indev.c:132`：`lv_timer_create(lv_indev_read_timer_cb,
     *   LV_DEF_REFR_PERIOD, indev)`）⇒ 16ms ⇒ 上限 62.5Hz（板上实测 `[TOUCH] 1s:
     *   采样 50`）。收到 10ms ⇒ 上限 100Hz。
     *   ⚠️ 仍然**只动这一个定时器**，`LV_DEF_REFR_PERIOD` 一个字不改（同上面的理由）。
     *   ⚠️ 代价：I2C 读次数翻倍（每次 ~7 B @100kHz ≈ 0.7ms）⇒ UI 任务多约 3.5% CPU。
     *      **采样窗内不受影响** —— 那时 UI 任务在 `while (app_engine_acquiring())` 里
     *      忙等，根本不会去读触摸（见 `app_ui_task`），所以不会往采样窗里灌 I2C 噪声。
     *   ⚠️ 位置必须在 `lv_indev_create()` **之后** —— 读定时器是那个函数里建的。 */
    const int touch_ok = (bsp_touch_init() == 0);
    s_indev = lv_indev_create();
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_indev, touch_read_cb);
    {
        lv_timer_t *rd = lv_indev_get_read_timer(s_indev);
        if (rd != NULL) {
            lv_timer_set_period(rd, APP_TOUCH_READ_PERIOD_MS);
        }
    }
    if (touch_ok == 0) {
        LOG_W("[LVGL] 触摸首次探测失败 —— 仍建 indev（bsp_touch_read 会每秒重试初始化）");
    }
#else
    /* 诊断开关：完全不去碰触摸引脚（PB10/PB11）—— 用来排除"固件反复驱动这两条线"
     * 对模块/电源轨的影响。见 project_config.h 的 APP_TOUCH_ENABLE。 */
    LOG_I("[LVGL] APP_TOUCH_ENABLE=0：本次启动不碰触摸（诊断态）");
#endif

    /* ★ 2026-10-14 八轮：`APP_LVGL_USE_DEMO` 的 demo 分支（`lv_demo_widgets()` +
     *   demo 后的堆余量诊断）已删 —— 屏/触摸的上板验证早完成，三处（本文件调用、
     *   CMake 源条目、`lv_conf.h` 的 `LV_USE_DEMO_WIDGETS`）一起下线。
     *   LVGL 堆的真实余量：开机一次由 `app_ui.c` 的 `ui_heap_report()` 打，之后
     *   每秒 `[UI] 1s`（5s 粒度）与每 5s `[LVGL] diag` 各打一份。 */
    /* 诊断浮层（fps + 触点红点）2026-10-07 已删除：业务 UI 顶栏有 fps、
     * 第 5 页 TOUCH 有坐标与准星，demo 模式靠控件自身的触摸反馈 ——
     * 同一套诊断信息原本要养三份（历史记录见 hardware/04 §6.17）。 */

    bsp_lcd_backlight(1); /* 到这一步屏已经初始化完，可以开背光了 */
    /* ⚠️ 打的是 **LVGL 实际生效的分辨率**，不是 board_config 的常量 ——
     *    两者在"设了旋转"之后会不一样（旋转会把逻辑分辨率换成 480×320）。
     *    2026-10-02 之前这里打的是常量，于是"旋转到底生效没有"从日志上看不出来。 */
    LOG_I("[LVGL] 就绪：%ldx%ld · draw buffer %u 行（%u B）· 池 %u KB @CCM + %u KB @SRAM1",
          (long)lv_display_get_horizontal_resolution(s_disp),
          (long)lv_display_get_vertical_resolution(s_disp),
          (unsigned)APP_LVGL_DRAW_BUF_LINES, (unsigned)sizeof(s_draw_buf),
          (unsigned)BOARD_LVGL_MEM_SIZE_KB, (unsigned)BOARD_LVGL_MEM_EXT_KB);

    return 0;
}

void app_lvgl_poll(uint32_t now_ms) {
    if (s_disp == NULL) {
        return;
    }

    /* LVGL 的时基靠"两次调用的差值"推进。
     * ⚠️ 第一次调用只记时间、不推 —— 否则会把"从上电到第一次调用"的
     *    那一大段（几秒）当成一次 tick 灌进去，动画会瞬移。 */
    if (!s_tick_started) {
        s_tick_started = 1;
        s_last_ms = now_ms;
        return;
    }
    {
        uint32_t dt = now_ms - s_last_ms; /* 无符号减法，天然处理回绕 */
        s_last_ms = now_ms;
        if (dt > 0U) {
            lv_tick_inc(dt);
        }
    }

    /* （2026-10-02 的"每 8 秒换一个方向"诊断已撤：根因是**面板反显**，不是方向。
     *   留档：轮换期间四个角度现象一致，恰是"颜色整体取反"的表现 —— 见上面的 INVON 说明。） */

    /* 跑一次 LVGL 的定时器：该重绘的重绘、该读触摸的读触摸。
     * 返回"距离下次该调用还有多少毫秒"，本层忽略它 —— 节拍由 Task 层定。
     * ★ 计时（2026-10-13 六轮）：`[UI] 1s` 的分段探针报"LVGL 段 ~190ms/s"，而
     *   flush 只占 21ms/s ⇒ 必须知道这 190ms 到底在 LVGL 里面还是外面（比如被抢占）。*/
    const uint32_t lvth0 = bsp_tick_cyc_now();
    (void)lv_timer_handler();
    s_lvth_cyc += (bsp_tick_cyc_now() - lvth0);

    /* ★ 诊断（2026-10-13，临时）：每 5 秒一行运行状态。
     * why：现场报"跑半分钟后**屏卡死**"。心跳日志证明固件一直活着、且日志里
     *   没有任何 LVGL 断言（`LV_ASSERT_MALLOC` 失败会打 ERROR）⇒ 需要把
     *   "**LVGL 不再重绘**"和"**在重绘、但屏不显示**"这两件事分开：
     *     · `flush` 累计像素**在涨** ⇒ 数据一直在往屏里写 ⇒ 问题在屏侧（FSMC 时序 / 模块）；
     *     · **停涨** ⇒ LVGL 自己停了 ⇒ 往 LVGL 侧查（脏区、flush_ready、内存）。
     * `heap free/maxblk` 同时盯内存池碎片 —— 32KB 池被碎掉的表现也是"跑一会儿就出事"。
     * 定位完删掉本段（它每 5 秒占一次 RTT，且读 lv_mem_monitor 要遍历池）。 */
    {
        static uint32_t s_diag_ms = 0U; /* 上一次打印的时刻 */
        static uint32_t s_diag_px = 0U; /* 上一次打印时的累加器（差分用）*/
        if (s_diag_ms == 0U) {
            /* 首拍只锚定、不打印：否则分母是"开机到现在的几秒"，读数会偏小到没意义 */
            s_diag_ms = now_ms;
            s_diag_px = s_flush_px;
        } else if ((uint32_t)(now_ms - s_diag_ms) >= 5000U) {
            /* ★ 2026-10-14 八轮（P0.4）：**打实际窗口 ms**（不再硬编码 "5s"）。
             *   旧写法窗口是"≥5000ms 且可能被 UI 任务拖长"，读者却按 5s 去算 ⇒ 一旦
             *   RTT 上行满 1024B 静默丢行，相减就会得出"超过 FSMC 物理上限"的假吞吐
             *   （本会话就是这样误判过一次）。现在窗口、像素、px/s 一起打，自成闭环。 */
            const uint32_t dt_ms = (uint32_t)(now_ms - s_diag_ms);
            const uint32_t win_px = s_flush_px - s_diag_px; /* 本窗口真实像素 */
            s_diag_ms = now_ms;
            s_diag_px = s_flush_px;

            lv_mem_monitor_t mon;
            lv_mem_monitor(&mon);
            /* flush 自身耗时（µs/窗口）。判读：拿它和 `[UI] 1s` 的分段 LVGL 比 ——
             * 若 flush 远小于分段值 ⇒ 时间花在 **LVGL 软件渲染**（不是写屏）。 */
            const uint32_t flush_us = bsp_tick_cyc_to_us(s_flush_cyc);
            LOG_I("[LVGL] diag: 窗口 %lu ms · flush %lu px（%lu px/s）/ %lu 次（最大 %lu px/次）",
                  (unsigned long)dt_ms, (unsigned long)win_px,
                  (unsigned long)((uint64_t)win_px * 1000U / (uint64_t)dt_ms),
                  (unsigned long)s_flush_calls, (unsigned long)s_flush_max_px);
            /* ★★ 2026-10-14：**拆成两行**。原来这一行 > 160 B，尾部被 `Common/Src/log.c` 的
             *   `LOG_LINE_MAX` **静默截断** ⇒ `heap free=18856` 被切成了 **`=1`**，
             *   被当成"滑动时 LVGL 堆只剩 1 B"，照着它去调池、烧了一版 A/B —— 全是白工
             *   （真值一直是 18,856，见 `docs/03_LVGL刷屏优化评估-20261014.md` §5.7）。
             * ⚠️ **纪律**：带数字的日志行**不许超过 `LOG_LINE_MAX`(160 B)**；装不下就分行。
             *   `log.c` 现在会给被截断的行尾补一个 `~` 记号 —— 见到 `~` 就去拆行，别解释数字。 */
            LOG_I("[LVGL] diag: flush %lu µs · handler %lu µs · heap free=%u maxblk=%u frag=%u%%",
                  (unsigned long)flush_us, (unsigned long)bsp_tick_cyc_to_us(s_lvth_cyc),
                  (unsigned)mon.free_size, (unsigned)mon.free_biggest_size,
                  (unsigned)mon.frag_pct);
            s_flush_cyc = 0U;
            s_lvth_cyc = 0U;
            s_flush_calls = 0U;
            s_flush_max_px = 0U;
        }
    }
}
