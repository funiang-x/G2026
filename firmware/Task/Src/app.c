/* ============================================================================
 *  app.c —— **组合根**（Composition Root）
 *
 *  四层结构里唯一允许跨层的地方（docs/01）。它同时看 Operation 与 Device，
 *  职责只有三件事：**创建实例 → 注入参数 → 启动**。
 *
 *  ⚠️ 它是**装配**，不是业务：
 *     · 业务逻辑一律在 `Operation/`（纯函数，PC 上可单测）
 *     · 硬件操作一律在 `Device/`
 *     · 本文件只做"把两者接起来"这件事，不做任何计算
 *
 *  ⚠️ 本文件是全工程**唯一**允许同时 include `Device/Inc` 与 `Operation/Inc`
 *     的 Task 文件。其余 Task 文件只 include `Operation/Inc` 与 `Common/Inc`。
 * ==========================================================================*/
#include "app.h"

#include "app_blink.h"
#include "app_engine.h"     /* 测量编排（task_ops 层 —— 碰 bsp_adc/bsp_spi） */
#include "app_lvgl.h"       /* LVGL 适配层（显示 + 触摸）—— 与 Device 一起用 */
#include "app_tasks.h"      /* 任务创建（Task_Engine / Task_Ui）*/
#include "app_ui.h"         /* 测量 UI（五页面，task_ops 层） */
#include "board_config.h"   /* 硬件事实 */
#include "bsp.h"            /* Device 层总入口 */
#include "log.h"            /* Common 层：LOG_* 宏的唯一来源 */
#include "op_flash.h"       /* Operation 层：页写拆分的纯逻辑 */
#include "project_config.h" /* 应用参数 */

#include <stddef.h> /* NULL —— 不要依赖别的头间接带入（app_init_failed 用到） */

/* 心跳灯（PB2，见 board_config.h）。句柄是组合根的私有状态 ——
 * 别的文件不该知道"灯接在哪个脚上"。 */
static const bsp_gpio_t s_led = {(bsp_gpio_port_t)BOARD_LED_PORT, BOARD_LED_PIN};

/* ⚠️ 板上 SW2（原 PA0）**已让给 ADC1_IN0** —— 见 board_config.h 的说明。
 *    所以这里不再有按键句柄与按键自检。要按键就得先把模拟输入挪到别的脚。 */

static int      s_led_on = 0;        /* 当前已写入的电平，用于"变了才写" */
static int      s_boot_reported = 0; /* [AI_READY] 只打一次 */
static int      s_stack_reported = 0; /* 任务栈余量只报一次 */
static uint32_t s_last_hb_ms = 0;

/* ---- 自检结果表（UI 第 3 页 STATE 读，见 app.h）------------------------ */
static app_st_item_t s_st[APP_ST_MAX];
static int            s_st_n = 0;

/* 记一项。满了就丢 —— 自检是"探一探"，不该因为它把组合根搞崩。 */
static void st_add(const char *name, int ok) {
    if (s_st_n < APP_ST_MAX) {
        s_st[s_st_n].name = name;
        s_st[s_st_n].ok = (ok != 0) ? 1 : 0;
        s_st_n++;
    }
}

int app_selftest_snapshot(app_st_item_t *out, int cap) {
    if (out == NULL || cap < APP_ST_MAX) {
        return -1;
    }
    for (int i = 0; i < s_st_n; i++) {
        out[i] = s_st[i];
    }
    return s_st_n;
}

/* ---- 自检状态（上板验证用）-------------------------------------------- */
static bsp_rtc_time_t s_rtc_t0; /* 启动时读到的 RTC 时间 */
static int            s_rtc_t0_valid = 0;
static int            s_rtc_checked = 0; /* 走时对比只做一次 */

/* 定义在文件下方，这里先声明（C 要求先声明后使用）。
 * 返回 0 = 读写往返一致；-1 = 失败**或跳过**（跳过 ≠ 验证通过，见函数内注释）。 */
static int app_flash_rw_test(void);

/* 上电自检：把板载外设挨个探一遍，结果打进日志。
 *
 * ⚠️ **任何一项失败都不停机** —— 这只是"探一探"，不是启动门槛。
 *    教训来自 2026-09-23：SDIO 初始化失败曾经让整块板子起不来
 *    （见 app_init_failed() 的说明）。自检同理：探不通就报一行，继续跑。
 *
 * ⚠️ 必须在 bsp_log_init() 之后调 —— 它全程依赖日志。 */
static void app_self_test(void) {
    /* ① 板载 SPI Flash（W25Q128，16MB）*/
    if (bsp_spi_flash_init() == 0) {
        LOG_I("[SELFTEST] W25Q128 ok, JEDEC ID=0x%04X（期望 0xEF18）",
              (unsigned)bsp_spi_flash_read_id());
        st_add("FLASH", 1);
    } else {
        LOG_W("[SELFTEST] W25Q128 未就绪 —— 看上面那行 ID 实际读到了什么");
        st_add("FLASH", 0);
    }

    /* ② TF 卡 —— **2026-10-06 起不检了**：`SDIO` 已从 `.ioc` 移除（板子不留 TF
     *    卡座），CubeMX 不再生成 `sdio.c/h` 与 HAL SD 驱动 ⇒ `bsp_sd.c` 一并
     *    从构建里下线（见 `CMakeLists.txt` 的 device 目标）。 */

    /* ③ 系统时钟频率（走 Device 层的封装，不直接调 HAL —— 那是分层规矩）
     * ⚠️ 期望值是 **144MHz 不是 168MHz**：2.0MSPS 采样要求 ADCCLK=36MHz，
     *    而那要求 PCLK2=72MHz ⇒ SYSCLK=144MHz。推导见 board_config.h 时钟一节。 */
    LOG_I("[SELFTEST] SYSCLK = %lu Hz（期望 144000000）",
          (unsigned long)bsp_tick_sysclk_hz());
    st_add("CLK", bsp_tick_sysclk_hz() == 144000000UL);

    /* ④ RTC。没设过时间时读出来是 2000-01-01，那是"无意义值"不是错误。
     *    ⇒ 第一次上电主动设一次，之后靠 VBAT 保持。
     *    真正的「走时验证」在 app_poll_at() 里做：5 秒后读回来对比。 */
    if (!bsp_rtc_is_configured()) {
        const bsp_rtc_time_t init = {2026U, 9U, 23U, 3U, 20U, 30U, 0U};
        if (bsp_rtc_set(&init) == 0) {
            LOG_I("[SELFTEST] RTC 首次配置为 2026-09-23 20:30:00（星期三）");
        } else {
            LOG_E("[SELFTEST] RTC 设置失败");
        }
    }
    if (bsp_rtc_get(&s_rtc_t0) == 0) {
        s_rtc_t0_valid = 1;
        st_add("RTC", 1);
        LOG_I("[SELFTEST] RTC 初值 %02u:%02u:%02u（5 秒后复查是否在走）", s_rtc_t0.hour,
              s_rtc_t0.minute, s_rtc_t0.second);
    } else {
        st_add("RTC", 0);
        LOG_E("[SELFTEST] RTC 读取失败");
    }

    /* ⑤ USB —— **2026-10-02 起不检了**：`USB_OTG_FS` 已从固件里拔除（省 ~15KB FLASH，
     *    见 `board_config.h` 的 USB 段）。Type-C 座仍然能用（ROM bootloader 的 DFU），
     *    但那与本固件无关 ⇒ 这里原来那句"USB_OTG_FS 已使能"**已经是假话**，
     *    留着会污染自检结论（同 `docs/07` 的"自检即硬件清单"口径）。 */

    /* ⑥ SPI Flash 读写往返（会擦最后一个扇区；内部有"非空则跳过"的保护）*/
    st_add("FLASH RW", app_flash_rw_test());

    /* ⑦ 采样链自检 —— **不在这里**，挪到 `app_adc_selftest()`（调度器启动后）。
     *    理由：它要真启动一次采集，路上有 `xSemaphoreTake` 与 DMA 中断里的
     *    `xSemaphoreGiveFromISR` 两处 FreeRTOS 原语；调度器启动前跑会被
     *    FreeRTOS 的 ISR 入栈校验断言拦成死循环（2026-10-02 实测）。
     *    见 app.h 里 `app_adc_selftest()` 的说明。 */

    /* ⑧ 液晶屏：读 0x04 RDDID。
     * ⚠️ 判据同样是"**读得到**"而不是"ID 等于某个值"：能读到 4 个字节就说明
     *    RS 地址译码 + 16 位数据总线 + 读写时序全部正确。
     *    ⚠️ 此时屏**已经**由 LVGL 初始化过（app_init 里 app_lvgl_init 先跑）。 */
    st_add("LCD", bsp_lcd_self_check() == 0);

    /* ⑨ 触摸：FT6336 应答 0x38（TD_STATUS 读得通即过，见 bsp_touch.c）。 */
#if APP_TOUCH_ENABLE
    st_add("TOUCH", bsp_touch_self_check() == 0);
#else
    st_add("TOUCH", 0); /* 诊断态跳过 ⇒ 记"未通过"，别在屏上显示一个假的 ok */
    LOG_I("[SELFTEST] APP_TOUCH_ENABLE=0：跳过触摸自检（诊断态，不碰 PB10/PB11）");
#endif

    /* ⑩ DSP 链：合成一帧已知单音 → 全链 → 校验谱峰位置与幅值。
     * ★ 这是**唯一覆盖 MCU 侧 arm_cfft_f32 路径**的检查 —— PC 单测走的是朴素
     *   DFT 替身（OP_DSP_HOST_FFT），看不见 FFT 实例/表是否装配正确
     *   （2026-09-28 加，理由见 app_engine.h）。 */
    st_add("DSP", app_engine_selftest() == 0);

    /* ⚠️ 板上按键 SW2（PA0）**无代码消费**（ADC 于 2026-09-28 挪到 PA1，
     *    PA0 空闲但 SW2 电路还在；要用按键先配 .ioc 再建 bsp_key）——
     *    所以这里没有按键自检。LED 仍靠 app_poll_at() 的心跳闪烁体现。 */

#if APP_SELFTEST_FAULT
    /* ⑧ 故意触发 HardFault（仅当 project_config.h 里 APP_SELFTEST_FAULT=1）。
     *    写保留地址 ⇒ BusFault ⇒ 升级成 HardFault。
     *    预期：RTT 出现 [FAULT] + 栈帧转储，然后**停机**。 */
    LOG_W("[SELFTEST] APP_SELFTEST_FAULT=1 —— 即将故意触发 HardFault");
    LOG_W("[SELFTEST] 预期：出现 [FAULT] + 栈帧转储，然后停机");
    /* ⚠️ 用"未定义指令"，**不是**"写保留地址"：
     *    实测写 0xFFFFFFF0 在 F407 上**不产生 fault**（被直接忽略），
     *    固件照常往下跑，看着像"取证没生效"，其实是**根本没崩**。
     *    `udf` ⇒ UsageFault ⇒（未单独使能）升级成 HardFault，必然触发。 */
    __builtin_trap();
#endif
}

/* 采样链自检（在调度器启动后跑，见 app.h 里的完整说明）。
 *
 * 判据是"**DMA 确实搬回了点**"，不是"数值等于某个期望值" —— PA1 此刻可能没接
 * 信号源，读数是多少都正常。搬回来就证明触发 / 转换 / 搬运三跳全通
 * （这是"编译通过 ≠ 能跑"最容易骗人的一处）。 */
void app_adc_selftest(void) {
    st_add("ADC", bsp_adc_self_check() == 0);
}

/* SPI Flash 读写往返测试：擦一个扇区 → 跨页写 → 读回比对。
 *
 * ⚠️ **它会擦掉一个 4KB 扇区**，所以有两道保护：
 *    ① 选**最后一个扇区**（16MB - 4KB）—— 离固件和数据区最远；
 *    ② 动手前**先把整个扇区读一遍**，只有确认全是 0xFF（从未写过）
 *       才继续；否则只报一行"跳过"。⇒ 万一里面真有数据，也只会
 *       得到"跳过"，不会丢东西。
 *
 * ★ 为什么用 `app_flash_write()` 而不是 `bsp_spi_flash_write_page()`：
 *   前者走 `op_flash_split_pages()` 拆分，**故意从页中间起写、跨越页边界**
 *   —— 这才是那套拆分逻辑存在的意义，不测它等于没测。
 *
 * ⚠️ 缓冲区用 `static`（BSS）不吃栈 —— `app_init()` 跑在 MSP 上，
 *    而 MSP 只有 1~2KB，放不下两个 256B 数组加中间变量。 */
static uint8_t s_flash_expect[BSP_SPI_FLASH_PAGE_SIZE];
static uint8_t s_flash_actual[BSP_SPI_FLASH_PAGE_SIZE];

/* 写在测试数据开头的魔数 —— 让下一次上电的「保护检查」能认出
 * "这片是上次我们自己写的"，而不是误判成"别人的数据"（那会**永久跳过**测试）。 */
static const uint8_t s_flash_magic[8] = {'F', 'L', 'S', 'H', 'T', 'S', 'T', '1'};

static int app_flash_rw_test(void) {
    /* 用**倒数第二个**扇区：上一次跑测试在最后一个扇区留了数据（那是加魔数
     * 之前写的，认不出来），换个扇区让本次能立刻测起来。
     * 有了魔数之后，以后同一个扇区就能反复测了。 */
    const uint32_t base = BSP_SPI_FLASH_SIZE_BYTES - (2U * BSP_SPI_FLASH_SECTOR_SIZE);
    const uint32_t off = 128U; /* 从**页中间**起写，制造跨页 */

    /* ① 保护：扇区必须"是空的"或"是我们上次留下的测试数据"。
     *    判据：读 base+off 处的前 16 字节 ——
     *      全 0xFF ⇒ 空，安全
     *      带魔数  ⇒ 上次就是我们写的，可覆盖
     *      其他    ⇒ 别人的数据，跳过
     *
     *    ⚠️ **必须能认出第二种**，否则会自锁：第一次跑写入数据 →
     *       第二次检测到"非空"就跳过 → 跳过就不执行收尾的擦除 → 永远跳过。
     *       （2026-09-23 实测踩过。） */
    uint8_t probe[16];
    if (bsp_spi_flash_read(base + off, probe, sizeof(probe)) != 0) {
        LOG_W("[FLASHTEST] 读扇区失败，跳过");
        return -1;
    }
    int blank = 1;
    int ours = 1;
    for (unsigned i = 0U; i < sizeof(probe); i++) {
        if (probe[i] != 0xFFU) {
            blank = 0;
        }
        if (i < sizeof(s_flash_magic) && probe[i] != s_flash_magic[i]) {
            ours = 0;
        }
    }
    if (!blank && !ours) {
        LOG_W("[FLASHTEST] 测试扇区有数据且不是我们的，**跳过以免误擦**");
        /* ⚠️ 返回 -1（= 未通过）而不是 0：**跳过 ≠ 验证通过**。
         *   屏上显示 FAIL 是诚实的 —— 这一项这次确实没验。 */
        return -1;
    }

    /* ② 造图案：前 8 字节放魔数（供下次识别），其余是递增异或
     *    （递增异或能避免全 0 / 全 1 这类"看不出错"的值）*/
    for (unsigned i = 0U; i < sizeof(s_flash_expect); i++) {
        s_flash_expect[i] = (uint8_t)(i ^ 0x5AU);
    }
    for (unsigned i = 0U; i < sizeof(s_flash_magic); i++) {
        s_flash_expect[i] = s_flash_magic[i];
    }

    /* ③ 擦 → 跨页写 → 读回 */
    if (bsp_spi_flash_erase_sector(base) != 0) {
        LOG_E("[FLASHTEST] 扇区擦除失败");
        return -1;
    }
    if (app_flash_write(base + off, s_flash_expect, sizeof(s_flash_expect)) != 0) {
        LOG_E("[FLASHTEST] 跨页写失败");
        return -1;
    }
    if (bsp_spi_flash_read(base + off, s_flash_actual, sizeof(s_flash_actual)) != 0) {
        LOG_E("[FLASHTEST] 读回失败");
        return -1;
    }

    /* ④ 比对 */
    unsigned bad = 0U;
    unsigned first_bad = 0U;
    for (unsigned i = 0U; i < sizeof(s_flash_expect); i++) {
        if (s_flash_expect[i] != s_flash_actual[i]) {
            if (bad == 0U) {
                first_bad = i;
            }
            bad++;
        }
    }
    if (bad == 0U) {
        LOG_I("[FLASHTEST] 擦除 + 跨页写 %u B（跨 2 页）+ 读回：**逐字节一致**",
              (unsigned)sizeof(s_flash_expect));
        /* 收尾：把测试扇区擦回空白。
         * ⚠️ 不擦的话，下次上电会因为"扇区非空"而跳过测试（实测踩过）——
         *    保护逻辑把**自己上次写的数据**当成了"别人的数据"。 */
        if (bsp_spi_flash_erase_sector(base) == 0) {
            LOG_I("[FLASHTEST] 测试扇区已擦回空白（下次可重复测）");
        }
        return 0;
    }
    LOG_E("[FLASHTEST] 读回有 %u/%u 字节不符（首个 @+%u：写 0x%02X 读到 0x%02X）",
          bad, (unsigned)sizeof(s_flash_expect), first_bad, s_flash_expect[first_bad],
          s_flash_actual[first_bad]);
    return -1;
}

void app_init(void) {
    /* ---- ① Device 就位 --------------------------------------------------
     * ⚠️ bsp_log_init() 必须**最先**：后面所有 LOG_* 都依赖它。
     *    它在调度器启动前被调用，所以内部只做初始化、不建锁（见 bsp_log.c）。 */
    bsp_log_init();
    bsp_log_set_level(LOG_LVL_INFO);

    /* ---- ② Operation 参数注入 -------------------------------------------
     * ★ 参数从 project_config.h 来，但**由本文件传进去** ——
     *   Operation 不读配置头（否则它没法用别的参数在 PC 上单测）。 */
    app_blink_init(APP_BLINK_ON_MS, APP_BLINK_OFF_MS);

    /* ---- ③ 启动前的收尾 ------------------------------------------------- */
    (void)bsp_gpio_config_out(s_led);
    bsp_gpio_write(s_led, 0);
    s_led_on = 0;

    /* ---- ④ 新外设的上电（**顺序有依赖，别乱**）---------------------------
     *   ADC：只配寄存器与 DMA，不启动采集（采集是门控的）
     *   屏：bsp_lcd_init（FSMC 已由 main 的 MX_FSMC_Init 配好）→ 复位 →
     *       LVGL 发初始化序列 → 触摸 → 开背光
     *   ⚠️ 必须**先**做这两步，下面的自检才有东西可查。 */
    (void)bsp_adc_init();
    if (app_lvgl_init() != 0) {
        /* 屏起不来不停机：业务（采样/DSP）不依赖它，降级跑。 */
        LOG_E("[APP] LVGL/屏初始化失败 —— 业务继续，但看不到界面");
    }

    /* 测量链：engine 先 init（载标定表），UI 再 init（读得到"已标定"状态）。
     * app_ui_init 失败跟着 LVGL 一起降级 —— 显示坏了不影响测量。 */
    app_engine_init();
    if (app_ui_init() != 0) {
        LOG_E("[APP] UI 构建失败 —— 测量继续，但看不到界面");
    }

    /* ---- ⑤ 板载外设自检（上板验证用；任何一项失败都不停机）--------------- */
    /* ⚠️ 进自检前把中断掩码清掉 —— 本工程的 FreeRTOS 端口在**调度器启动前**
     *    `vTaskExitCritical()` 走空分支（tasks.c：xSchedulerRunning==pdFALSE 时直接
     *    `mtCOVERAGE_TEST_MARKER()`），**根本不恢复中断**。⇒ 前面任何一次
     *    `taskENTER_CRITICAL()`（建内核对象、LVGL 取锁…）都会把 BASEPRI 永久钉在
     *    configMAX_SYSCALL_INTERRUPT_PRIORITY，优先级 ≥5 的中断全失效。
     *    机理详见 bsp_log.c 顶部。
     *    ⚠️ 自检十项里**只有第 ⑦ 项（ADC 忙等）依赖中断**，那一项自己在忙等前清；
     *       ⑧LCD 走 FSMC 轮询、⑨触摸走 I²C 轮询、⑩DSP 纯计算，都不需要。 */
    app_self_test();

    LOG_I("app_init done (tick=%u ms, on/off=%u/%u ms)", (unsigned)APP_TICK_PERIOD_MS,
          (unsigned)APP_BLINK_ON_MS, (unsigned)APP_BLINK_OFF_MS);
}

/* 周期主体。now_ms 由 app_poll() 取，做成参数是为了让本函数在将来能被直接
 * 单测（它不读时钟、不碰 RTOS）。 */
static void app_poll_at(uint32_t now_ms) {
    /* ---- ① 闭环标记 -----------------------------------------------------
     * ⚠️ **不在上电瞬间打**：`fw.py verify` 是"先复位放行、再挂 RTT 主机"，
     *    而 RTT 上行缓冲只有 1024 B 且满则静默丢 ⇒ 主机挂上时那条标记已经没了，
     *    表现为"超时"（退出码 2）—— 看着像固件没跑起来，其实是采集端来晚了。
     *    延迟到 APP_TRACE_START_MS 之后再打，才是可复现的做法。 */
    if (s_boot_reported == 0 && now_ms >= APP_TRACE_START_MS) {
        s_boot_reported = 1;
        LOG_I(BOARD_BOOT_OK_MARKER " boot ok, uptime=%lu ms", (unsigned long)now_ms);
    }

    /* ---- ② 心跳（让 RTT 上能看出"系统还活着"）--------------------------- */
    if ((uint32_t)(now_ms - s_last_hb_ms) >= APP_HEARTBEAT_PERIOD_MS) {
        s_last_hb_ms = now_ms;
        LOG_I("heartbeat, uptime=%lu ms", (unsigned long)now_ms);
    }

    /* ---- ③ 任务栈余量（一次性，上板验证用）------------------------------
     * 高水位只降不升 ⇒ 它是"栈快溢出"的提前预警。要晚到 DSP/LVGL 都跑过几轮
     * 才有意义（APP_STACK_REPORT_MS）；想验 UI 任务最坏栈，先手动跑一次标定。 */
    if (s_stack_reported == 0 && now_ms >= APP_STACK_REPORT_MS) {
        s_stack_reported = 1;
        app_tasks_report_stack();
    }

    /* ---- ④ 业务：Operation 出结论，组合根把它落到 Device 上 -------------- */
    int want = app_blink_poll(now_ms);
    if (want != s_led_on) {
        s_led_on = want;
        bsp_gpio_write(s_led, want);
    }

    /* ---- ⑤ 自检：RTC 走时（5 秒后复查一次）------------------------------
     * 判据是"两个时间点的秒数差 ≈ 经过的秒数"。若 RTC 压根没在走，
     * 读回来会和初值**一模一样** —— 那种情况要查 LSE 晶振或时钟源。 */
    if (s_rtc_t0_valid && s_rtc_checked == 0 && now_ms >= 5000U) {
        s_rtc_checked = 1;
        bsp_rtc_time_t t1;
        if (bsp_rtc_get(&t1) == 0) {
            int d = (int)t1.second - (int)s_rtc_t0.second;
            if (d < 0) {
                d += 60; /* 跨分钟 */
            }
            LOG_I("[SELFTEST] RTC 走时：%02u:%02u:%02u → %02u:%02u:%02u（%d 秒）%s",
                  s_rtc_t0.hour, s_rtc_t0.minute, s_rtc_t0.second, t1.hour, t1.minute,
                  t1.second, d, (d >= 4 && d <= 6) ? "✓ 正常" : "✗ 异常（查 LSE）");
        }
    }

    /* ---- ⑥ 测量 / 界面 ---------------------------------------------------
     * 正常路径：两者都是**独立任务**（Task_Engine / Task_Ui，由 app_tasks_init 建），
     * 本循环不再插手 —— 采集阻塞在 ADC 帧信号量上、重测走任务通知、结果走
     * mailbox（见 app_engine.h / app_tasks.h）。
     * 降级路径：任务没建起来时（app_tasks_init 失败）退回旧形态，在本拍内联跑
     * —— 保底可用，绝不静默变砖（engine 先跑，UI 后刷，LVGL 最后）。 */
    if (!app_tasks_ok()) {
        app_engine_poll(now_ms);
        app_ui_poll(now_ms);
        app_lvgl_poll(now_ms);
    }
}

void app_poll(void) {
    app_poll_at(bsp_tick_ms());
}

void app_init_failed(const char* reason) {
    /* ⚠️ 与"出声 + 停机"的 panic 路径相反：**这里要返回**。
     *    调用它的是 `Error_Handler()`，而 Error_Handler 会被"可选外设初始化失败"
     *    调到。那种情况让整块板子停摆是不合理的 —— 用不上那个外设的人
     *    不该被它绑架。
     *
     * ⚠️ 也**不能**打 `[AI_FAIL]`：那是 `fw.py verify` 的失败判据，
     *    打了会把"其实跑起来了"误判成失败。 */
    bsp_log_init(); /* 幂等：RTT 后端只需内存，不依赖时钟与中断 */
    bsp_log_printf_nolock(LOG_LVL_ERROR, "[E] %s",
                          (reason != NULL) ? reason : "HAL 初始化失败");
}

int app_flash_write(uint32_t addr, const uint8_t* buf, uint32_t len) {
    /* ★ 四层的走法在这里看得最清楚：
     *     ① Operation 出**方案**（纯计算，能在 PC 上单测）
     *     ② Device 按方案**逐段干硬件**
     *     ③ Task（本函数）只负责把两者接起来 —— 不写硬件细节，也不算边界
     *
     *   拆分没塞进 `bsp_spi`，因为 R1 禁止 Device 调 Operation；
     *   也没塞进本文件，因为那会让它变成"没法单测的硬件代码"。 */
    op_flash_seg_t seg[APP_FLASH_MAX_SEGS];

    int n = op_flash_split_pages(addr, len, BSP_SPI_FLASH_PAGE_SIZE, seg,
                                 APP_FLASH_MAX_SEGS);
    if (n < 0) {
        LOG_E("Flash 写失败：拆分不出来（len=%lu，最多 %d 段）", (unsigned long)len,
              APP_FLASH_MAX_SEGS);
        return -1;
    }

    for (int i = 0; i < n; i++) {
        if (bsp_spi_flash_write_page(addr + seg[i].offset, buf + seg[i].offset,
                                     seg[i].len) != 0) {
            LOG_E("Flash 写失败：第 %d/%d 段（偏移 %lu）", i + 1, n,
                  (unsigned long)seg[i].offset);
            return -1;
        }
    }
    return 0;
}
