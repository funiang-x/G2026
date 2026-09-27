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
#include "app_lvgl.h"       /* LVGL 适配层（显示 + 触摸）—— 与 Device 一起用 */
#include "app_tasks.h"      /* 任务划分与 DSP→UI 数据交接（配置文档 §9/§10）*/
#include "board_config.h"   /* 硬件事实 */
#include "bsp.h"            /* Device 层总入口 */
#include "log.h"            /* Common 层：LOG_* 宏的唯一来源 */
#include "op_flash.h"       /* Operation 层：页写拆分的纯逻辑 */
#include "project_config.h" /* 应用参数 */

#include <stddef.h> /* NULL —— 不要依赖别的头间接带入（app_panic 用到） */

/* 心跳灯（PB2，见 board_config.h）。句柄是组合根的私有状态 ——
 * 别的文件不该知道"灯接在哪个脚上"。 */
static const bsp_gpio_t s_led = {(bsp_gpio_port_t)BOARD_LED_PORT, BOARD_LED_PIN};

/* ⚠️ 板上 SW2（原 PA0）**已让给 ADC1_IN0** —— 见 board_config.h 的说明。
 *    所以这里不再有按键句柄与按键自检。要按键就得先把模拟输入挪到别的脚。 */

static int      s_led_on = 0;        /* 当前已写入的电平，用于"变了才写" */
static int      s_boot_reported = 0; /* [AI_READY] 只打一次 */
static uint32_t s_last_hb_ms = 0;
static uint32_t s_last_stack_ms = 0; /* 上一次打印栈高水位 */

/* ---- 自检状态（上板验证用）-------------------------------------------- */
static bsp_rtc_time_t s_rtc_t0; /* 启动时读到的 RTC 时间 */
static int            s_rtc_t0_valid = 0;
static int            s_rtc_checked = 0; /* 走时对比只做一次 */

/* 定义在文件下方，这里先声明（C 要求先声明后使用） */
static void app_flash_rw_test(void);

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
    } else {
        LOG_W("[SELFTEST] W25Q128 未就绪 —— 看上面那行 ID 实际读到了什么");
    }

    /* ② TF 卡。**没插卡是常态，不是错误** —— 只报一行，不打扰。
     *    CubeMX 生成的 MX_SDIO_SD_Init() 已在 main 里跑过一次，
     *    没卡时它必然失败；这里只判断"要不要再试一次"。 */
    if (bsp_sd_is_detected()) {
        LOG_I("[SELFTEST] TF 卡在位，尝试初始化…");
        if (bsp_sd_init() == 0) {
            bsp_sd_info_t info;
            if (bsp_sd_get_info(&info) == 0) {
                LOG_I("[SELFTEST] TF 卡就绪：%lu KB（%lu 块 × %lu B）",
                      (unsigned long)info.capacity_kb, (unsigned long)info.block_count,
                      (unsigned long)info.block_size);
            }
        } else {
            LOG_W("[SELFTEST] TF 卡初始化失败（接触不良 / 非 SD 卡？）");
        }
    } else {
        LOG_I("[SELFTEST] TF 卡未插入（正常，跳过）");
    }

    /* ③ 系统时钟频率（走 Device 层的封装，不直接调 HAL —— 那是分层规矩）
     * ⚠️ 期望值是 **144MHz 不是 168MHz**：2.0MSPS 采样要求 ADCCLK=36MHz，
     *    而那要求 PCLK2=72MHz ⇒ SYSCLK=144MHz。推导见 board_config.h 时钟一节。 */
    LOG_I("[SELFTEST] SYSCLK = %lu Hz（期望 144000000）",
          (unsigned long)bsp_tick_sysclk_hz());

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
        LOG_I("[SELFTEST] RTC 初值 %02u:%02u:%02u（5 秒后复查是否在走）", s_rtc_t0.hour,
              s_rtc_t0.minute, s_rtc_t0.second);
    } else {
        LOG_E("[SELFTEST] RTC 读取失败");
    }

    /* ⑤ USB —— 只能报"外设已使能"。枚举成功与否取决于主机侧，
     *    而本工程**没有实现应用层描述符**（CDC/MSC），所以主机不会认它。 */
    LOG_I("[SELFTEST] USB_OTG_FS 已使能（仅 PCD 层，无描述符 ⇒ 主机不会枚举）");

    /* ⑥ SPI Flash 读写往返（会擦最后一个扇区；内部有"非空则跳过"的保护）*/
    app_flash_rw_test();

    /* ⑦ 采样链：TIM8 触发 ADC1 → DMA2 搬 64 个点。
     * ★ 判据是"**DMA 确实搬回了点**"，不是"数值等于某个期望值" ——
     *   PA0 此刻可能悬空，读数是多少都正常。搬回来就证明
     *   触发 / 转换 / 搬运三跳全通（这是"编译通过 ≠ 能跑"最容易骗人的一处）。 */
    (void)bsp_adc_self_check();

    /* ⑧ 液晶屏：读 0x04 RDDID。
     * ⚠️ 判据同样是"**读得到**"而不是"ID 等于某个值"：能读到 4 个字节就说明
     *    RS 地址译码 + 16 位数据总线 + 读写时序全部正确。
     *    ⚠️ 此时屏**已经**由 LVGL 初始化过（app_init 里 app_lvgl_init 先跑）。 */
    (void)bsp_lcd_self_check();

    /* ⑨ 触摸：GT911 产品 ID 应为 ASCII "911"。 */
    (void)bsp_touch_self_check();

    /* ⚠️ 板上按键**已不存在**（PA0 让给 ADC1_IN0，见 board_config.h）——
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

static void app_flash_rw_test(void) {
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
        return;
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
        return;
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
        return;
    }
    if (app_flash_write(base + off, s_flash_expect, sizeof(s_flash_expect)) != 0) {
        LOG_E("[FLASHTEST] 跨页写失败");
        return;
    }
    if (bsp_spi_flash_read(base + off, s_flash_actual, sizeof(s_flash_actual)) != 0) {
        LOG_E("[FLASHTEST] 读回失败");
        return;
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
    } else {
        LOG_E("[FLASHTEST] 读回有 %u/%u 字节不符（首个 @+%u：写 0x%02X 读到 0x%02X）",
              bad, (unsigned)sizeof(s_flash_expect), first_bad,
              s_flash_expect[first_bad], s_flash_actual[first_bad]);
    }
}

void app_init(void) {
    /* ---- ① Device 就位 --------------------------------------------------
     * ⚠️ bsp_log_init() 必须**最先**：后面所有 LOG_* 都依赖它。
     *    它在调度器启动前被调用，所以内部只做初始化、不建锁（见 bsp_log.c）。 */
    bsp_log_init();
    bsp_log_set_level(BSP_LOG_INFO);
    bsp_tick_init();

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

    /* ---- ⑤ 板载外设自检（上板验证用；任何一项失败都不停机）--------------- */
    app_self_test();

    /* ---- ⑥ 任务与队列（**必须在调度器启动之前**）-------------------------
     * ⚠️ 时机是硬要求：`xTaskCreateStatic` 在 `osKernelStart()` 之前调用是允许的，
     *    而 `osKernelInitialize()` 由 main() 在本函数**之后**调 —— 所以这里
     *    只能建**原生 FreeRTOS** 对象，不能调 `osThreadNew` 那套 CMSIS 包装。
     *    4 个测量/UI 任务在这里建；`Task_Sys` 是 CubeMX 的 defaultTask（见 app_tasks.h）。 */
    if (app_tasks_init() != 0) {
        LOG_E("[APP] 任务创建失败 —— 调度会缺任务，检查栈数组/队列长度");
    }

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

    /* ---- ③ 业务：Operation 出结论，组合根把它落到 Device 上 -------------- */
    int want = app_blink_poll(now_ms);
    if (want != s_led_on) {
        s_led_on = want;
        bsp_gpio_write(s_led, want);
    }

    /* ---- ④ 自检：RTC 走时（5 秒后复查一次）------------------------------
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

    /* ---- ⑤ 任务栈高水位（每 5 秒一次）-----------------------------------
     * 栈溢出是本工程最难查的一类故障（现象是"随机跑飞"）。高水位是唯一能
     * **提前**看到它的手段 —— 它只降不升，贴近 0 就是快溢出了。
     * ⚠️ 本函数（app_poll）跑在 `Task_Sys` 上，也就是 CubeMX 的 defaultTask。 */
    if ((uint32_t)(now_ms - s_last_stack_ms) >= APP_STACK_REPORT_PERIOD_MS) {
        s_last_stack_ms = now_ms;
        app_tasks_report_stack();
    }
}

void app_poll(void) {
    app_poll_at(bsp_tick_ms());
}

void app_panic(const char* reason) {
    /* ⚠️ 只能用 nolock：此刻调度器可能没起、时钟可能不对，取锁/等中断都是死路。
     *    RTT 后端只需要内存（不依赖时钟与中断），所以它能出声 ——
     *    这也正是"默认用 RTT 而不是串口"的一个附带好处。 */
    bsp_log_init(); /* 幂等：RTT 后端只是复位读写指针 */
    bsp_log_printf_nolock(BSP_LOG_ERROR, BOARD_BOOT_FAIL_MARKER " %s",
                          (reason != NULL) ? reason : "unknown");
    bsp_log_printf_nolock(
        BSP_LOG_ERROR,
        "[FAULT] 固件已停在此处。查：HSE 起振 / 时钟树 / HAL 初始化返回值");

    /* 不返回。栈与堆的状态未知，继续跑只会掩盖现场。 */
    for (;;) {
    }
}

void app_init_failed(const char* reason) {
    /* ⚠️ 与 app_panic 的关键差别：**这里要返回**。
     *    调用它的是 `Error_Handler()`，而 Error_Handler 会被"可选外设初始化失败"
     *    调到（典型：没插 TF 卡 ⇒ `HAL_SD_Init` 失败）。那种情况让整块板子停摆
     *    是不合理的 —— 用不上 TF 卡的人不该被它绑架。
     *
     * ⚠️ 也**不能**打 `[AI_FAIL]`：那是 `fw.py verify` 的失败判据，
     *    打了会把"其实跑起来了"误判成失败。 */
    bsp_log_init(); /* 幂等：RTT 后端只需内存，不依赖时钟与中断 */
    bsp_log_printf_nolock(BSP_LOG_ERROR, "[E] %s",
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
