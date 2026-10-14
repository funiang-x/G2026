/* ============================================================================
 *  app_engine.c —— 测量编排 · **独立 FreeRTOS 任务** · 见 app_engine.h
 *
 *  ---- 一次测量的数据流 ----
 *      xTaskNotifyWait(周期超时 | UI 请求)   ← 任务通知 = 轻量二值信号量
 *        → bsp_adc_start(一帧 4096 点)
 *        → bsp_adc_wait_frame(50ms)         ← **阻塞**在 ADC 帧信号量上（ISR 给）
 *        → bsp_adc_stop()
 *        → op_dsp_run → op_peak_run → op_meas_build
 *        → xQueueOverwrite(结果 mailbox)     ← 跨任务"双缓冲"：写不覆盖读
 *
 *  ---- 内存落点 ----
 *  s_adc_buf 在 SRAM1（DMA 可达）；**只有 s_fft(32KB)** 在 CCM 低半区
 *  （section .ccmram；`s_xw` 与 `s_adc_buf` 一样在 SRAM）；CCM 不能被 DMA 摸 —— 它们不碰 DMA。
 *  ⚠️ 2026-10-02 核实：**链接脚本里目前没有越界 ASSERT**（原注释说"有"，是错话），
 *     而 `_eccmram` 恰好停在 0x10008000 = LVGL 内存池基址（`BOARD_LVGL_MEM_ADDR`）
 *     ⇒ 往 `.ccmram` 里再加变量会**静默压进 LVGL 堆**。补 ASSERT 与 `(NOLOAD)`
 *     （现 `>CCMRAM AT> FLASH` 会让这 32KB 白占 FLASH 的 LMA）见 hardware/25 §6-S3。
 *
 *  ---- 标定流程（docs/01 §11）----
 *  UI 逐步驱动：cal_begin → 显示频点提示 → 人调信号源 → cal_step（同步测一次）
 *  → …17 次… → cal_save（擦+写 Flash）。表在 RAM 里即时生效。
 *  ⚠️ cal_step 由 LVGL 回调（UI 任务上下文）调，本函数**阻塞**到测完 ——
 *     标定是"人点一下测一次"的交互，UI 短暂停顿可接受；彻底不卡 UI 需改成
 *     "engine 收请求 / UI 轮询进度"的异步版（见 app_engine.h 的遗留说明）。
 * ==========================================================================*/
#include "app_engine.h"

#include <math.h> /* sin / lround —— 自检的合成帧 */

#include "board_config.h" /* 增益/采样率的装配来源（Task 层允许读硬件事实） */
#include "bsp.h"
#include "log.h"
#include "project_config.h"
#include "op_dsp.h"
#include "op_meas.h"
#include "op_peak.h"

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

/* DMA 缓冲：SRAM1（.bss），8KB。**单缓冲** —— 采集是门控的（采满即停触发源），
 * 没有"边采边处理"的流水；且生产者(2.048ms/帧)比消费者(DSP ~13ms/帧)快 6 倍，
 * 双缓冲只会让新帧盖掉还没处理完的旧帧（见 app_engine.h 的说明）。 */
static uint16_t s_adc_buf[OP_DSP_N];

/* 加窗时域（N floats，**SRAM1**）：op_peak 的单点 DFT 点积要用它。
 * ⚠️ 2026-09-28 从 CCM 挪到 SRAM1 —— FFT 工作块要 2N（见下），CCM 那 32KB 装不下两块。
 *    它只被 CPU 读（不经 DMA），放 SRAM1 无副作用，只是访问略慢。 */
static float s_xw[OP_DSP_N];

/* FFT 工作块：**2N floats = 32KB**（CCM 低 32KB）。
 * ⚠️ 越界**没有**编译期兜底：2026-10-02 核实链接脚本里原本没有 ASSERT，现已补
 *   （`.ccmram` 加 `(NOLOAD)` + 两条越界 ASSERT，见 hardware/25 §6-S3 与 .ld 注释）
 * ⚠️⚠️ 必须是 **2N**！`arm_cfft_f32` 的 p1 是"2*fftLen 个 float 的交织复数缓冲"
 *   （官方 `arm_cfft_f32.c` 文档 + `arm_radix8_butterfly_f32` 索引到
 *   `pSrc[2*(fftLen-1)+1]` = 8191）。**2026-09-28 修正**：原先只给 N=4096，
 *   FFT 会写穿 16KB —— 而当时 ELF 里紧邻的正是 s_xw，等于每次测量都把 op_peak
 *   要用的加窗数据写坏。PC 测不到（host 走朴素 DFT 替身），见 docs/01 §5.1。
 * ⚠️ 不能放 CCM 的是 DMA 目标（s_adc_buf 在 SRAM1 ✓）—— 这块是 CPU 独占。 */
static float s_fft[2 * OP_DSP_N] __attribute__((section(".ccmram")));

/* 编译期钉住尺寸契约：**改 OP_DSP_N 而忘了同步缓冲**会当场报错，
 * 而不是等到上板写穿内存（2026-09-28 那两处缺陷就是这么来的）。 */
_Static_assert(sizeof(s_fft) / sizeof(s_fft[0]) == 2U * OP_DSP_N,
               "FFT 工作块必须是 2*OP_DSP_N 个 float（arm_cfft_f32 的官方约定）");

/* ---- 结果 mailbox：1 深队列 + xQueueOverwrite = 跨任务双缓冲 ----
 * engine 只写、UI 只读；队列内部自带临界区 ⇒ UI 永远读不到"写了一半"的结构。
 * ⚠️ 存储数组用**元素类型**声明（不是 uint8_t[]）—— 对齐天然正确（历史坑）。 */
static meas_result_t s_result_q_store[1];
static StaticQueue_t s_result_q_cb;
static QueueHandle_t s_result_q = NULL;

static meas_result_t s_result_snap; /* UI 侧 xQueuePeek 的落点（只 UI 任务读写） */

static TaskHandle_t     s_task = NULL;   /* 本任务句柄（remeasure 的通知目标） */
static volatile uint8_t s_acquiring = 0; /* 1 = 采样窗进行中（UI 据此静默，见 O1） */

/* 1 = 共享 DSP 缓冲正被"非 engine 任务"占用（标定在 UI 任务里跑同步测量）。
 * ⚠️ 这是本文件唯一的一处**跨任务共享资源**保护：s_adc_buf/s_xw/s_fft 是三个
 *    static 数组，标定（UI 任务）与周期测量（engine 任务）都用它们。
 *    ⚠️ 2026-10-14 订正（原文写反了）：旧注释说"engine 优先级更高 ⇒ 标定跑到一半时
 *    engine 会抢占并写坏缓冲" —— 方向**反了**：UI(26) > engine(25)（见 project_config.h）
 *    ⇒ 是**标定抢占 engine**（标定回调跑在 UI 任务里，见 `app_engine_cal_step`）。
 *    所以 engine 每轮进来先看这个标志，被占用就跳过。
 *    用 volatile 单字节（Cortex-M 上读写原子）足够 —— 不需要互斥量。
 *    ⚠️ 想换成互斥量（可拿优先级继承）的完整评估见
 *    `_work/rtos_review_20261014/FreeRTOS-合理使用指南与改造清单.md` §4.1。 */
static volatile uint8_t s_buf_busy = 0;
/* ★ 2026-10-14 八轮（P0.2）：engine 正握着共享缓冲（`s_adc_buf`/`s_xw`/`s_fft`）的标志。
 *   为什么 `s_buf_busy` 一个不够：那个检查只在 `engine_measure_once` 的**入口**读
 *   （`app_engine.c:245`），而标定是在**UI 任务**里跑的、优先级比 engine 高
 *   （UI 26 > engine 25）⇒ engine 越过入口检查、还没跑完 DSP 时会被抢占，标定回调
 *   随即用同一批缓冲起新采集 ⇒ 两边同时读写同一块内存。本标志从"入口"一直立到
 *   "发布完成"，标定侧先等它落 0 再置 `s_buf_busy`（见 `app_engine_cal_step`）。 */
static volatile uint8_t s_measuring = 0;

/* 链长分段（2026-10-14 八轮 P3.1）：单位 µs；每 5s 打一行 `[ENG]`（见 measure_body） */
static uint32_t s_eng_acq_us = 0U; /* 采集窗（抢闸 + 等帧 2.048ms + 停）*/
static uint32_t s_eng_dsp_us = 0U; /* DSP 链（FFT + 找峰/精化 + 口径四条）*/
static uint32_t s_eng_pub_us = 0U; /* 发布（队列覆盖）*/
static uint32_t s_eng_log_ms = 0U;

static uint32_t s_seq = 0U;              /* 结果序列号（发布一次 +1） */

/* ---- 原始帧口径（2026-10-13 六轮，见 app_engine.h）---- */
static float             s_lsb_in_mv = 0.0f; /* 码 → 输入端 mV（init 里装配）*/
static app_engine_raw_t  s_raw;             /* 最近一帧的原始统计 */
static uint8_t  s_scene = MEAS_SCENE_UA;

/* 降级路径（多任务没建起来时 app_poll 内联调用）用的周期计时 */
static uint32_t s_last_start_ms = 0U;

/* 标定 */
static op_cal_table_t s_cal;           /* 工作表（载入的或正在建的） */
static int            s_cal_valid = 0; /* 1 = 表可用于修正 */
static int            s_cal_calibrating = 0;
static int            s_cal_idx = 0;
static int            s_cal_loaded = 0;

/* ---------------- 内部：跑一次完整测量并发布 ---------------- */
/* ⚠️ 会阻塞（等 ADC 帧 ~2.048ms + DSP 链 ~13ms）。只能从 engine 任务、
 *    UI 任务的标定回调、或降级路径的 app_poll 里调 —— 都是任务上下文。 */
/* 同步采一帧（两处调用方共用：engine_measure_once / cal_measure_current）：
 * 抢 LCD 总线闸 → 启动 ADC → 阻塞等帧 → 停。
 * 返回 0 = 帧已在 s_adc_buf；-1 = start 失败（静默）或帧超时（日志已打）。
 * ★ 收成一个函数的原因：take/acquiring/start/wait/stop/give 的**配对顺序**
 *   错一处就是总线闸泄漏或采集态卡死 —— 两份拷贝早晚改岔。 */
static int acquire_frame(void) {
    /* ---- 采样窗：**先抢 LCD 总线闸，再开采集**（2026-10-02 新增）----
     * 闸在开采集之前就拿到 ⇒ 写屏已经在块边界停住（拿闸时若 UI 正在写，
     * 优先级继承让它先把当前块写完）⇒ 整个 2.048ms 采样窗内 FSMC 静态。
     * 抢不到（闸没建起来/超时）就退回旧的整帧级避让 —— 不比改前差。
     * 闸本体在 Device 层（bsp_lcd，它是总线的主人），两边都链得到它。 */
    /* ★ 2026-10-13 四轮修：**抢闸的结果必须记下来**（原来 `(void)` 掉、末尾无条件
     *   `give`）。若这次 take 超时（UI 正持着闸在写块），那记 give 就是"放一把
     *   没拿到的锁"：互斥量的持有者计数被打乱，之后任一方 take 都可能**永久阻塞**
     *   —— 现象正是"屏再也不刷新，而任务都还在跑"（这类故障最难查，因为日志里
     *   一切正常）。配对规则：take 成功才 give。 */
    const int got_bus = bsp_lcd_bus_take(APP_LCD_BUS_WAIT_MS);
    s_acquiring = 1;

    if (bsp_adc_start(s_adc_buf, OP_DSP_N) != 0) {
        s_acquiring = 0;
        if (got_bus != 0) {
            bsp_lcd_bus_give();
        }
        return -1; /* start 失败（上一帧没收走 / 参数错）—— 静默，下一拍再试 */
    }

    /* 阻塞等帧就绪（ISR 给信号量）。等待期间**让出 CPU**、UI 任务会跑，但它在
     * 块边界拿不到闸 ⇒ 不写 FSMC（否则 UI 会把整个 ≈1.6ms 的 flush 写完，
     * 直接横跨这个 2.048ms 采样窗 —— 那正是 O1 名存实亡的原因）。 */
    const int got = bsp_adc_wait_frame(APP_ADC_FRAME_TIMEOUT_MS);
    s_acquiring = 0; /* 采样窗结束 */
    bsp_adc_stop();
    if (got_bus != 0) { /* 只放自己真拿到的锁（见上面的说明）*/
        bsp_lcd_bus_give();
    }

    if (got != 0) {
        LOG_W("[ENG] 采集帧超时（>%u ms）—— 触发链或 DMA 未工作，本帧跳过",
              (unsigned)APP_ADC_FRAME_TIMEOUT_MS);
        return -1;
    }
    return 0;
}

/* 原始帧统计 + 抽取轨迹（跑在测量任务里；4096 次比较/乘加 ≈ 几十 µs —— 与它同框的
 * FFT 是 13ms 级，占比可忽略）。见 app_engine.h 的口径说明。 */
static void raw_publish(void) {
    uint32_t lo = 0xFFFFU;
    uint32_t hi = 0U;
    uint64_t sum = 0U;

    for (uint32_t i = 0U; i < (uint32_t)OP_DSP_N; i++) {
        const uint32_t v = s_adc_buf[i];
        if (v < lo) {
            lo = v;
        }
        if (v > hi) {
            hi = v;
        }
        sum += v;
    }
    const float mean = (float)sum / (float)OP_DSP_N; /* 八轮：f32（见下方方差段的说明）*/

    /* ★ 2026-10-14 八轮（P3.3）：**double → float + Kahan 补偿**。
     *   原来整个平方和走 double 软浮点（4096 次 dsub/dmul/dadd ≈ 5.7ms/帧），而本工程
     *   只有单精度 FPU。朴素 float 求和 4096 项的**上界**误差 ~N·eps·Σ|x| ≈ 2.5e-4
     *   相对（Vrms 判据 5mV/100mV = 5% ⇒ 本来也够），但这里直接沿用 op_peak 的房规
     *   ——Kahan 补偿（4 flops/项，硬件 FPU，误差 ~eps·|Σ|）—— 把精度问题一次问死。 */
    float sq = 0.0f;
    float sq_c = 0.0f;
    for (uint32_t i = 0U; i < (uint32_t)OP_DSP_N; i++) {
        const float d = (float)s_adc_buf[i] - (float)mean;
        const float prod = d * d;
        const float t = prod - sq_c;
        const float ns = sq + t;
        sq_c = (ns - sq) - t;
        sq = ns;
    }

    const float ac_rms = sqrtf(sq / (float)OP_DSP_N); /* 码（AC 有效值）*/

    s_raw.seq = s_seq;
    s_raw.vpp_mv = (float)(hi - lo) * s_lsb_in_mv;
    s_raw.vrms_mv = (float)ac_rms * s_lsb_in_mv;
    /* 八轮：字面量 2048 → `BOARD_VBIAS_SENSE_CODE_MID`（同一事实只许有一个来源；
     * 自检那条判据（bsp_adc_self_check）本来就用它，这里当时写死了。） */
    s_raw.vmean_mv = (mean - (float)BOARD_VBIAS_SENSE_CODE_MID) * s_lsb_in_mv;

    /* ★ 2026-10-14：**结论存档** —— 这里原来还有一段"归一化 8 位抽取轨迹"（160 点
     *   盒式平均，128 = 均值、±127 = ±4σAC），供 UI 在无基波时画噪声带。
     *   **随"无信号画直线"（`app_ui.c` 的 `wave_idle`）一起删了**：没有读者了。
     *   省 160 B RAM + 每帧 160 次盒式平均（≈0.1% CPU）。
     *   ⚠️ 当年那两条坑别忘（要恢复轨迹时先看这里）：
     *     ① **不能送 ADC 裸码**（>>4）—— 悬空输入的噪声只有 ±2 码，量化后是常数，
     *        屏上仍是一条直线（2026-10-13 六轮实测栽过）⇒ 必须按 4σ 归一化；
     *     ② **必须盒式平均、不能单点抽取** —— 显示点的时间格 = 2.048ms/160 = 12.8µs，
     *        2MSPS 下相邻采样几乎不相关，单点抽取会把格内全部噪声混叠进来，
     *        画出屏幕网格根本表达不了的"毛球"（既失真、又把光栅化成本推高 5 倍）。 */
}

int app_engine_raw(app_engine_raw_t *st) {
    if (st == NULL || s_raw.seq == 0U) {
        return -1; /* 还没采过 */
    }
    *st = s_raw;
    return 0;
}

/* 真正干活的半段（所有早退都在这里面）。抽出来的目的只有一个：
 * 让 `engine_measure_once` 能用"入口置 `s_measuring`、出口清"包住**每一条返回路径**
 * —— 标定（UI 任务）靠这个标志判断"engine 是不是正握着共享缓冲"（见其声明处）。 */
static void engine_measure_body(void) {
    /* ---- 分段计时（2026-10-14 八轮 P3.1）：把"一拍到底花在哪"量成实测 ----
     * why：节拍已改成"睡到绝对时刻"（见 app_engine_task）⇒ 链长直接决定数据率上限，
     *   而各段占比的几次估算互相差一倍（谁占 20~40ms）。
     * 口径：采集窗（抢闸 + 等帧 + 停）/ DSP 链（FFT+找峰+口径）/ 发布。 */
    const uint32_t c0 = bsp_tick_cyc_now();

    /* ---- 采样：见 acquire_frame() 的说明（抢闸 → 采集 → 放闸）---- */
    if (acquire_frame() != 0) {
        return; /* start 失败或帧超时 —— 下一拍再试 */
    }
    const uint32_t c1 = bsp_tick_cyc_now();
    uint32_t       c2 = 0U; /* DSP 段结束时刻（在下面的内层块里赋值）*/

    if (op_dsp_run(s_adc_buf, OP_DSP_N, s_xw, s_fft, s_fft) != 0) {
        LOG_E("[ENG] op_dsp_run 失败（参数/缓冲错）");
        return;
    }
    raw_publish(); /* 原始帧口径（无基波时 UI 的兜底；见 app_engine.h）*/

    {
        meas_peak_t pk[MEAS_MAX_PEAKS];
        const int n_pk = op_peak_run(s_xw, s_fft, OP_DSP_N, op_dsp_fs_hz(),
                                     s_cal_valid ? &s_cal : NULL, pk, MEAS_MAX_PEAKS);
        if (n_pk < 0) {
            LOG_E("[ENG] op_peak_run 失败");
            return;
        }
        c2 = bsp_tick_cyc_now(); /* DSP 段到此为止 */

        meas_result_t r;
        op_meas_build(pk, n_pk, s_seq + 1U, s_scene, &r);
        s_seq = r.seq;

        if (s_result_q != NULL) {
            (void)xQueueOverwrite(s_result_q, &r); /* 覆盖式发布：不阻塞、不失败 */
        } else {
            s_result_snap = r; /* 队列没建起来（极端）—— 直接落快照 */
        }
    }

    /* 每 5s 一行链长（只在这里打，不刷屏）。判读：总链长 + 周期设定 ⇒ 占空比。 */
    const uint32_t c3 = bsp_tick_cyc_now();
    s_eng_acq_us = bsp_tick_cyc_to_us(c1 - c0);
    s_eng_dsp_us = bsp_tick_cyc_to_us(c2 - c1);
    s_eng_pub_us = bsp_tick_cyc_to_us(c3 - c2);
    const uint32_t now = bsp_tick_ms();
    if (s_eng_log_ms == 0U || (uint32_t)(now - s_eng_log_ms) >= 5000U) {
        s_eng_log_ms = now;
        LOG_I("[ENG] 一拍 %lu ms（采集 %lu + DSP %lu + 发布 %lu）· 节拍设 %u ms",
              (unsigned long)((s_eng_acq_us + s_eng_dsp_us + s_eng_pub_us) / 1000U),
              (unsigned long)s_eng_acq_us, (unsigned long)s_eng_dsp_us,
              (unsigned long)s_eng_pub_us, (unsigned)APP_MEAS_PERIOD_MS);
    }
}

static void engine_measure_once(void) {
    /* 共享缓冲被标定占用 ⇒ 本轮跳过（见 s_buf_busy 的说明）。
     * ⚠️ 必须在**碰任何缓冲之前**判断 —— 否则抢占就已经把标定的数据写坏了。 */
    if (s_buf_busy) {
        return;
    }

    s_measuring = 1; /* 从这一刻起，标定侧要等我们跑完（见声明处）*/
    engine_measure_body();
    s_measuring = 0;
}

/* ---------------- 对外 ---------------- */
void app_engine_init(void) {
    /* 码 → 输入端 mV 的折算系数：VREF/4096/净增益（verify_dsp 的 LSB_IN 同式）。
     * fs 由 TIM8 推导：144MHz /((PSC+1)(ARR+1)) = 2.0MHz —— 两头都从
     * board_config 来，改采样率只动 board_config，这里自动跟着。 */
    const float lsb_in_mv = (float)BOARD_ADC_VREF_MV / 4096.0f / (float)BOARD_NET_GAIN;
    s_lsb_in_mv = lsb_in_mv; /* 原始帧口径也要用（见 raw_publish）*/
    const float fs_hz =
        (float)BOARD_TARGET_SYSCLK_HZ /
        (float)((BOARD_TIM8_PSC + 1U) * (BOARD_TIM8_ARR + 1U));
    op_dsp_init(fs_hz, lsb_in_mv);
    {
        /* 找峰参数来自 project_config.h（APP_PEAK_*），不再写字面量。
         * 扫带与门限的推导见那里的注释。 */
        const op_peak_params_t p = {APP_PEAK_F_LO_HZ, APP_PEAK_F_HI_HZ, APP_PEAK_GATE_MV};
        op_peak_init(&p);

        /* ★ 扫带自检：改 fs / OP_DSP_N 会让上面那组扫带**静默失效**
         *   （漏掉 10 kHz 或 500 kHz 的峰，且不报任何错）。
         *   判据来自 op_peak.c 的下标算术：搜索从 k_lo+1 起、止于 k_hi-4。 */
        {
            const float df = fs_hz / (float)OP_DSP_N;
            const int k_lo = (int)(APP_PEAK_F_LO_HZ / df);
            const int k_hi = (int)(APP_PEAK_F_HI_HZ / df);
            const int k_need_lo = (int)(10000.0f / df);  /* 题面下界 10 kHz 的栅格主峰 */
            const int k_need_hi = (int)(500000.0f / df); /* 题面上界 500 kHz 的 bin */
            if (k_lo >= k_need_lo || k_hi - 4 < k_need_hi) {
                LOG_E("[ENG] 找峰扫带不覆盖题面 10k/500k：k_lo=%d（须 < %d）· "
                      "k_hi=%d（须 ≥ %d）—— 改过 fs 或 OP_DSP_N？见 project_config.h "
                      "的 APP_PEAK_* 段",
                      k_lo, k_need_lo, k_hi, k_need_hi + 4);
            }
        }
    }

    /* 结果 mailbox：1 深 + 静态存储（不动 FreeRTOS 堆）。启动前创建可以，但
     * 建完必须把中断放回来 —— 同 bsp_adc_init()：否则 BASEPRI 被钉住、
     * HAL 时基失效、后面任何 HAL_Delay() 都会死循环。 */
    if (s_result_q == NULL) {
        s_result_q = xQueueCreateStatic(1U, sizeof(meas_result_t), (uint8_t *)s_result_q_store,
                                        &s_result_q_cb);
        portENABLE_INTERRUPTS();
    }
    if (s_result_q == NULL) {
        LOG_E("[ENG] 结果 mailbox 创建失败 —— UI 将只看到初始空结果");
    }

    /* 载入标定表：坏表/无表不影响测量（G=1/P=0，UI 提示未标定） */
    uint8_t blob[OP_CAL_BLOB_SIZE];
    s_cal_loaded = 0;
    if (bsp_spi_flash_read(APP_CAL_FLASH_ADDR, blob, sizeof(blob)) == 0 &&
        op_cal_unpack(blob, sizeof(blob), &s_cal) == 0) {
        s_cal_valid = 1;
        s_cal_loaded = 1;
        LOG_I("[ENG] 标定表已载入（%d 点，首点 %.1fkHz）", OP_CAL_POINTS,
              (double)(s_cal.f[0] / 1000.0f));
    } else {
        LOG_W("[ENG] 无有效标定表 —— 读数未做 LTI 修正（第 3 页 CAL 页点 CALIBRATE 建表）");
    }

    s_seq = 0U;
    s_result_snap.seq = 0U;
    s_last_start_ms = 0U;
}

/* ★ 2026-10-14：**发布时刻戳**（本轮结果是什么时候发出去的）。
 *   为什么需要它：用户的要求是"接入信号要**及时**刷出来"，那就得能**量**端到端延迟，
 *   而不是靠推算。UI 侧拿到新 `seq` 时算 `now_ms − app_engine_pub_ms()` 就是
 *   **"发布 → UI 决定重绘"** 这一段（见 `app_ui.c` 的 `[UI] 1s`）。
 *   ⚠️ 不加进 `meas_result_t`：那个结构有 84 B 红线（`docs/01 §8`），
 *      而这个值只是诊断用、且**只在新 seq 出现时读一次** ⇒ 放文件级变量足够。
 *   ⚠️ 32 位对齐读写在 Cortex-M 上是原子的 ⇒ 不需要额外同步；`seq` 已经是配对判据。 */
static volatile uint32_t s_pub_ms = 0U;
uint32_t app_engine_pub_ms(void) {
    return s_pub_ms;
}

/* 测量任务：等"周期到"或"UI 请求重测" → 跑一帧。
 *
 * ★ 为什么用 xTaskNotifyWait 而不是"定时 + 轮询标志"：
 *   它把"周期超时"与"外部唤醒"合成一次阻塞调用 —— 没有通知就睡满一个测量
 *   周期（500ms），有通知（切页 / 换 1T-3T）立刻醒。既不轮询、也不多一个队列。
 * ⚠️ 退出时清空通知计数（0xFFFFFFFF），避免多次 remeasure 攒成"连续狂测"。 */
void app_engine_task(void *arg) {
    (void)arg;
    s_task = xTaskGetCurrentTaskHandle();
    LOG_I("[ENG] 测量任务启动（prio=%u, stack=%u words）", (unsigned)APP_TASK_ENGINE_PRIORITY,
          (unsigned)APP_TASK_ENGINE_STACK_WORDS);

    /* ★ 2026-10-14 八轮（P3.2）：**睡到下一个绝对时刻**，不再"测完再睡一个周期"。
     *   旧写法把链长直接叠加进周期（100ms 设定 + ~40ms 链长 ≈ 140ms ⇒ 实测只有 7Hz），
     *   而"波形流畅度"只看**内容更新率** ⇒ 链长必须藏在周期里（绝对时刻天然做到）。
     *   被 UI 通知唤醒（切页/换档要立刻重测）时：立即测，但**不改相位** ⇒ 下一次
     *   仍然落在原节拍上，不会因为"多测了一拍"把节拍整体推后。 */
    uint32_t next_ms = bsp_tick_ms();
    for (;;) {
        next_ms += APP_MEAS_PERIOD_MS;
        {
            const uint32_t now = bsp_tick_ms();
            int32_t        wait = (int32_t)(next_ms - now); /* 无符号差分后再转有符号 */
            if (wait < 1) {
                wait = 1; /* 已经到点/过期 ⇒ 立刻测（也别传 0，pdMS_TO_TICKS(0) 是"不等待"）*/
            }
            (void)xTaskNotifyWait(0U, 0xFFFFFFFFUL, NULL, pdMS_TO_TICKS((uint32_t)wait));
        }
        engine_measure_once();
        s_pub_ms = bsp_tick_ms(); /* 发布时刻（本轮结果刚发出去，见上面的说明）*/

        /* 落后超过 3 个周期（被标定 / 长事务拖住）⇒ 重锚，避免"追赶风暴"把 CPU 打满 */
        if ((int32_t)(bsp_tick_ms() - next_ms) > (int32_t)(3U * APP_MEAS_PERIOD_MS)) {
            next_ms = bsp_tick_ms();
        }
    }
}

/* 降级路径：多任务没建起来时由 app_poll 每 10ms 调一次（旧形态，保底可用）。 */
void app_engine_poll(uint32_t now_ms) {
    if ((uint32_t)(now_ms - s_last_start_ms) >= APP_MEAS_PERIOD_MS) {
        s_last_start_ms = now_ms;
        engine_measure_once();
    }
}

void app_engine_remeasure(void) {
    if (s_task != NULL) {
        (void)xTaskNotifyGive(s_task); /* 唤醒测量任务立刻测一帧 */
    } else {
        s_last_start_ms = 0U; /* 降级路径：令周期判断立即成立 */
    }
}

const meas_result_t *app_engine_result(void) {
    /* UI 从 mailbox 取一份快照（xQueuePeek 自带临界区 ⇒ 不会读到写一半）。
     * 队列空（还没测过）时保留上一份快照。 */
    if (s_result_q != NULL) {
        (void)xQueuePeek(s_result_q, &s_result_snap, 0U);
    }
    return &s_result_snap;
}

/* ---------------- DSP 链自检（上电一次，见 app_engine.h） ---------------- */
int app_engine_selftest(void) {
    /* 单音落在第 k0 个 bin：**整数周期** ⇒ 无泄漏，谱峰位置与幅值都有解析真值。
     * k0=100 在扫描带 [19,1028] 内，对应 fs·k0/N = 48.83kHz。 */
    const uint32_t k0 = 100U;
    const float    amp_mv = 100.0f; /* 输入端 100mV 峰值（远高于门限 1.5mV） */
    const float    lsb_mv = (float)BOARD_ADC_VREF_MV / 4096.0f / (float)BOARD_NET_GAIN;
    const double   two_pi = 6.28318530717958647692;

    if (op_dsp_fs_hz() <= 0.0f) {
        LOG_E("[DSPTEST] fs 未注入 —— app_engine_init() 没先跑？");
        return -1;
    }
    const float f_expect = op_dsp_fs_hz() * (float)k0 / (float)OP_DSP_N;

    /* 合成：整数码 + 偏置 2048，四舍五入（与 PC 单测的 synth_code 同法） */
    for (uint32_t i = 0; i < OP_DSP_N; i++) {
        const double t = (double)i / (double)OP_DSP_N;
        const double mv = (double)amp_mv * sin(two_pi * (double)k0 * t);
        long c = lround(2048.0 + mv / (double)lsb_mv);
        if (c < 0) {
            c = 0;
        }
        if (c > 4095) {
            c = 4095;
        }
        s_adc_buf[i] = (uint16_t)c;
    }

    if (op_dsp_run(s_adc_buf, OP_DSP_N, s_xw, s_fft, s_fft) != 0) {
        LOG_E("[DSPTEST] op_dsp_run 失败");
        return -1;
    }

    {
        meas_peak_t pk[MEAS_MAX_PEAKS];
        const int n = op_peak_run(s_xw, s_fft, OP_DSP_N, op_dsp_fs_hz(), NULL, pk, MEAS_MAX_PEAKS);
        if (n != 1) {
            LOG_E("[DSPTEST] 期望检出 1 根谱线，实得 %d —— FFT / 找峰链路有问题", n);
            return -1;
        }
        const float df = op_dsp_fs_hz() / (float)OP_DSP_N;
        const float f_err = pk[0].freq_hz - f_expect;
        const float a_err = pk[0].amp_mv - amp_mv;
        if (fabsf(f_err) > df / 4.0f) {
            LOG_E("[DSPTEST] 谱峰频率错：读到 %.1fHz，期望 %.1fHz（Δf=%.1fHz）"
                  " —— FFT 实例/表装配错？",
                  (double)pk[0].freq_hz, (double)f_expect, (double)df);
            return -1;
        }
        if (fabsf(a_err) > 1.0f) {
            LOG_E("[DSPTEST] 谱峰幅值错：读到 %.3fmV，期望 %.1fmV —— 窗归一/折算错？",
                  (double)pk[0].amp_mv, (double)amp_mv);
            return -1;
        }
        LOG_I("[DSPTEST] DSP 链自检通过：合成 %.0fmV@bin%u → 读 %.1fHz / %.3fmV"
              "（Δf=%+.2fHz, ΔA=%+.3fmV）",
              (double)amp_mv, (unsigned)k0, (double)pk[0].freq_hz, (double)pk[0].amp_mv,
              (double)f_err, (double)a_err);
    }
    return 0;
}

int app_engine_acquiring(void) {
    return (int)s_acquiring;
}

void app_engine_set_scene(uint8_t scene) {
    if (scene <= MEAS_SCENE_UBJ) {
        s_scene = scene;
    }
}

uint8_t app_engine_scene(void) {
    return s_scene;
}

/* ---------------- 标定流程 ---------------- */
void app_engine_cal_begin(void) {
    op_cal_default_freqs(&s_cal);
    s_cal_idx = 0;
    s_cal_calibrating = 1;
    s_cal_valid = 0; /* 建表期间测量不带半截表 */
}

int app_engine_cal_points(void) {
    return OP_CAL_POINTS;
}

int app_engine_cal_index(void) {
    return s_cal_idx;
}

float app_engine_cal_freq(int idx) {
    if (idx < 0 || idx >= OP_CAL_POINTS) {
        return 0.0f;
    }
    return s_cal.f[idx];
}

/* 标定用：测当前频点，把读数写进 s_cal.g/p。返回 0 成功、-1 失败（不前进）。
 * ⚠️ 只碰共享缓冲，**不推进 s_cal_idx** —— 推进留给调用方。 */
static int cal_measure_current(void) {
    /* 同步测一帧（共用 acquire_frame）：由 LVGL 回调（UI 任务）调，会阻塞
     * ~15ms —— 标定是"人点一下测一次"的交互，可接受。 */
    if (acquire_frame() != 0) {
        return -1;
    }
    if (op_dsp_run(s_adc_buf, OP_DSP_N, s_xw, s_fft, s_fft) != 0) {
        return -1;
    }
    {
        meas_peak_t pk[MEAS_MAX_PEAKS];
        const int n_pk = op_peak_run(s_xw, s_fft, OP_DSP_N, op_dsp_fs_hz(), NULL,
                                     pk, MEAS_MAX_PEAKS);
        if (n_pk <= 0) {
            LOG_W("[CAL] %.1f kHz 未检出峰（信号源没接好？）",
                  (double)(s_cal.f[s_cal_idx] / 1000.0f));
            return -1; /* 不前进：人检查后重按"测量" */
        }
        /* 标定源是单音：取最低峰作该频点读数。
         * g = 读数/100mV（标称真值）；p = 读数相位（源端零相位）。
         * ★ 走 `op_cal_set_point`（不是直接赋值）：它负责把相位**展开**——
         *   本板的相移在 306~392 kHz 之间穿过 ±180°，主值序列会在那里跳 360°，
         *   直接存下去会让查表在该区间插出 ~180° 的错（实测 Upp 偏 -19 mV）。 */
        op_cal_set_point(&s_cal, s_cal_idx, pk[0].amp_mv / 100.0f, pk[0].phase_rad);
    }
    return 0;
}

int app_engine_cal_step(void) {
    if (!s_cal_calibrating || s_cal_idx >= OP_CAL_POINTS) {
        return -1;
    }

    /* ★ 独占共享缓冲（2026-10-14 八轮 P0.2 补强）：
     *   ① 先等 engine 这一轮**跑完** —— 它可能已经越过 `s_buf_busy` 的入口检查、
     *      正握着 `s_adc_buf`/`s_xw`/`s_fft`；而本函数跑在 UI 任务（优先级更高），
     *      不主动让路就一定是"抢占 + 同时读写同一块内存"。
     *      标定是"人点一下测一次"的交互 ⇒ 等几十毫秒可接受；超时（100ms）就放弃本轮、
     *      让人重按（不静默）。
     *   ② 再置 `s_buf_busy`：从此 engine 的入口检查会跳过，双方不会再撞。
     *   单入单出 —— 所有返回路径都必须清标志，所以测量逻辑抽成了子函数。 */
    const uint32_t t0 = bsp_tick_ms();
    while (s_measuring != 0U && (uint32_t)(bsp_tick_ms() - t0) < 100U) {
        vTaskDelay(1); /* UI 优先级高于 engine ⇒ 必须主动让出，否则它永远跑不完 */
    }
    if (s_measuring != 0U) {
        LOG_W("[CAL] engine 还在占着共享缓冲（>100ms）—— 本次标定跳过，请重按 MEAS");
        return -1;
    }

    s_buf_busy = 1;
    const int rc = cal_measure_current();
    s_buf_busy = 0;
    if (rc != 0) {
        return -1;
    }

    s_cal_idx++;
    if (s_cal_idx >= OP_CAL_POINTS) {
        s_cal_valid = 1; /* 建表完成立即生效（未 save 前断电即失） */
        s_cal_calibrating = 0;
        return 0;
    }
    return OP_CAL_POINTS - s_cal_idx; /* 剩余点数 */
}

int app_engine_cal_save(void) {
    uint8_t blob[OP_CAL_BLOB_SIZE];
    if (op_cal_pack(&s_cal, blob, sizeof(blob)) != (int)sizeof(blob)) {
        return -1;
    }
    /* 216B 在一页（256B）内，不跨页：擦扇区 → 单页写。 */
    if (bsp_spi_flash_erase_sector(APP_CAL_FLASH_ADDR) != 0 ||
        bsp_spi_flash_write_page(APP_CAL_FLASH_ADDR, blob, sizeof(blob)) != 0) {
        LOG_E("[CAL] 写 Flash 失败 —— 表本次有效，重启后丢失");
        return -1;
    }
    s_cal_loaded = 1;
    LOG_I("[CAL] 标定表已持久化 @0x%08lX", (unsigned long)APP_CAL_FLASH_ADDR);
    return 0;
}

int app_engine_cal_loaded(void) {
    return s_cal_loaded;
}
