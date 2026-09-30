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
 *  s_adc_buf 在 SRAM1（DMA 可达）；s_xw/s_fft 两块 f32 在 CCM 低 32KB
 *  （section .ccmram，链接脚本有越界 ASSERT；CCM 不能被 DMA 摸 —— 它们不碰 DMA）。
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

/* FFT 工作块：**2N floats = 32KB**（CCM 低 32KB，链接脚本 ASSERT 兜底越界）。
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
 *    engine 优先级更高 ⇒ 标定跑到一半时 engine 会**抢占**并写坏缓冲
 *    （s_cal.g/p 全变垃圾）。所以 engine 每轮进来先看这个标志，被占用就跳过。
 *    用 volatile 单字节（Cortex-M 上读写原子）足够 —— 不需要互斥量。 */
static volatile uint8_t s_buf_busy = 0;

static uint32_t s_seq = 0U;              /* 结果序列号（发布一次 +1） */
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
static void engine_measure_once(void) {
    /* 共享缓冲被标定占用 ⇒ 本轮跳过（见 s_buf_busy 的说明）。
     * ⚠️ 必须在**碰任何缓冲之前**判断 —— 否则抢占就已经把标定的数据写坏了。 */
    if (s_buf_busy) {
        return;
    }

    /* 采样窗标记：置起后 UI 任务会**推迟** FSMC 写（O1 干扰源管理）。
     * 必须在 start **之前**置位 —— 否则 UI 可能在采集开始后才看到。 */
    s_acquiring = 1;

    if (bsp_adc_start(s_adc_buf, OP_DSP_N) != 0) {
        s_acquiring = 0;
        return; /* start 失败（上一帧没收走 / 参数错）—— 下一拍再试 */
    }

    /* 阻塞等帧就绪（ISR 给信号量）。等待期间**让出 CPU**，UI 任务得以运行
     * —— 但 UI 看到 s_acquiring 会主动避让，不写 FSMC。 */
    const int got = bsp_adc_wait_frame(APP_ADC_FRAME_TIMEOUT_MS);
    s_acquiring = 0; /* 采样窗结束 */
    bsp_adc_stop();

    if (got != 0) {
        LOG_W("[ENG] 采集帧超时（>%u ms）—— 触发链或 DMA 未工作，本帧跳过",
              (unsigned)APP_ADC_FRAME_TIMEOUT_MS);
        return;
    }

    if (op_dsp_run(s_adc_buf, OP_DSP_N, s_xw, s_fft, s_fft) != 0) {
        LOG_E("[ENG] op_dsp_run 失败（参数/缓冲错）");
        return;
    }

    {
        meas_peak_t pk[MEAS_MAX_PEAKS];
        const int n_pk = op_peak_run(s_xw, s_fft, OP_DSP_N, op_dsp_fs_hz(),
                                     s_cal_valid ? &s_cal : NULL, pk, MEAS_MAX_PEAKS);
        if (n_pk < 0) {
            LOG_E("[ENG] op_peak_run 失败");
            return;
        }

        meas_result_t r;
        op_meas_build(pk, n_pk, s_seq + 1U, s_scene, &r);
        s_seq = r.seq;

        if (s_result_q != NULL) {
            (void)xQueueOverwrite(s_result_q, &r); /* 覆盖式发布：不阻塞、不失败 */
        } else {
            s_result_snap = r; /* 队列没建起来（极端）—— 直接落快照 */
        }
    }
}

/* ---------------- 对外 ---------------- */
void app_engine_init(void) {
    /* 码 → 输入端 mV 的折算系数：3.3V/4096/前端增益（verify_dsp 的 LSB_IN 同式）。
     * fs 由 TIM8 推导：144MHz /((PSC+1)(ARR+1)) = 2.0MHz —— 两头都从
     * board_config 来，改采样率只动 board_config，这里自动跟着。 */
    const float lsb_in_mv = 3300.0f / 4096.0f / (float)BOARD_FRONT_GAIN;
    const float fs_hz =
        (float)BOARD_TARGET_SYSCLK_HZ /
        (float)((BOARD_TIM8_PSC + 1U) * (BOARD_TIM8_ARR + 1U));
    op_dsp_init(fs_hz, lsb_in_mv);
    {
        const op_peak_params_t p = {9000.0f, 502000.0f, 1.5f};
        op_peak_init(&p);
    }

    /* 结果 mailbox：1 深 + 静态存储（不动 FreeRTOS 堆）。启动前创建：允许。 */
    if (s_result_q == NULL) {
        s_result_q = xQueueCreateStatic(1U, sizeof(meas_result_t), (uint8_t *)s_result_q_store,
                                        &s_result_q_cb);
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
        LOG_W("[ENG] 无有效标定表 —— 读数未做 LTI 修正（参数页长按 CAL 建表）");
    }

    s_seq = 0U;
    s_result_snap.seq = 0U;
    s_last_start_ms = 0U;
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

    for (;;) {
        (void)xTaskNotifyWait(0U, 0xFFFFFFFFUL, NULL, pdMS_TO_TICKS(APP_MEAS_PERIOD_MS));
        engine_measure_once();
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
    const float    lsb_mv = 3300.0f / 4096.0f / (float)BOARD_FRONT_GAIN;
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
    /* 同步测一帧：启动 → **阻塞等帧信号量** → DSP 链。
     * ⚠️ 由 LVGL 回调（UI 任务）调，会阻塞 ~15ms —— 标定是"人点一下测一次"
     *    的交互，可接受；**不再忙等**（旧版 while 自旋白烧 CPU）。 */
    s_acquiring = 1;
    if (bsp_adc_start(s_adc_buf, OP_DSP_N) != 0) {
        s_acquiring = 0;
        return -1;
    }
    const int got = bsp_adc_wait_frame(APP_ADC_FRAME_TIMEOUT_MS);
    s_acquiring = 0;
    bsp_adc_stop();
    if (got != 0) {
        LOG_W("[CAL] 采集超时 —— 触发链或 DMA 未工作");
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
         * g = 读数/100mV（标称真值）；p = 读数相位（源端零相位）。 */
        s_cal.g[s_cal_idx] = pk[0].amp_mv / 100.0f;
        s_cal.p[s_cal_idx] = pk[0].phase_rad;
    }
    return 0;
}

int app_engine_cal_step(void) {
    if (!s_cal_calibrating || s_cal_idx >= OP_CAL_POINTS) {
        return -1;
    }

    /* ★ 独占共享缓冲：置 s_buf_busy 后 engine 任务本轮跳过（见其声明处）。
     *   单入单出 —— 所有返回路径都必须清标志，所以测量逻辑抽成了子函数。 */
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
