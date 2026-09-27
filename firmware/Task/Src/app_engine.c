/* ============================================================================
 *  app_engine.c —— 测量编排态机 · 见 app_engine.h
 * ==========================================================================*/
#include "app_engine.h"

#include "board_config.h" /* 增益/采样率的装配来源（Task 层允许读硬件事实） */
#include "bsp.h"
#include "log.h"
#include "project_config.h"
#include "op_dsp.h"
#include "op_meas.h"
#include "op_peak.h"

/* DMA 缓冲：SRAM1（.bss），8KB。单缓冲 —— engine 是"采满→处理→再采"的
 * 串行节奏，双缓冲是给连续流用的（docs/01 §4：测量非连续流）。 */
static uint16_t s_adc_buf[OP_DSP_N];

/* DSP 缓冲：CCM 低 32KB（两块正好顶格，链接脚本 ASSERT 兜底越界）。
 * ⚠️ 不能放 CCM 的是 DMA 目标（s_adc_buf 在 SRAM1 ✓）—— 这两块 CPU 独占。 */
static float s_xw[OP_DSP_N] __attribute__((section(".ccmram")));
static float s_fft[OP_DSP_N] __attribute__((section(".ccmram")));

/* 采集态机 */
typedef enum {
    ENG_IDLE = 0, /* 等 trigger（周期到 / remeasure） */
    ENG_ACQ,      /* 采集中（DMA 搬运，2.048ms） */
    ENG_PROC,     /* 处理（一拍内 ~40ms 完成，见 app_engine.h 文件头） */
} eng_state_t;

static eng_state_t s_state = ENG_IDLE;
static uint32_t    s_last_start_ms = 0;

static meas_result_t s_result; /* 发布单例：seq 判断新旧，无锁（同任务） */
static uint8_t       s_scene = MEAS_SCENE_UA;

/* 标定 */
static op_cal_table_t s_cal;           /* 工作表（载入的或正在建的） */
static int            s_cal_valid = 0; /* 1 = 表可用于修正 */
static int            s_cal_calibrating = 0;
static int            s_cal_idx = 0;
static int            s_cal_loaded = 0;

/* ---------------- 内部：跑一遍 DSP 链（一帧 ~40ms，见文件头） ---------------- */
static int process_frame(void) {
    if (op_dsp_run(s_adc_buf, OP_DSP_N, s_xw, s_fft, s_fft) != 0) {
        LOG_E("[ENG] op_dsp_run 失败（参数/缓冲错）");
        return -1;
    }
    meas_peak_t pk[MEAS_MAX_PEAKS];
    const int n_pk = op_peak_run(s_xw, s_fft, OP_DSP_N, op_dsp_fs_hz(),
                                 s_cal_valid ? &s_cal : NULL, pk, MEAS_MAX_PEAKS);
    if (n_pk < 0) {
        LOG_E("[ENG] op_peak_run 失败");
        return -1;
    }
    op_meas_build(pk, n_pk, s_result.seq + 1U, s_scene, &s_result);
    return 0;
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

    s_result.seq = 0U;
    s_state = ENG_IDLE;
    s_last_start_ms = 0U;
}

/* 状态机：now_ms 由组合根传入（app_poll 的时基，与 LVGL 同源）。
 * ACQ 只等"采满"标志（bsp_adc 的 DMA 中断置位），典型 2ms 内完成 ——
 * 10ms 节拍下中间最多空转一拍，不值得为它上信号量（见 app_engine.h 文件头）。 */
void app_engine_poll(uint32_t now_ms) {
    switch (s_state) {
    case ENG_IDLE:
        if ((uint32_t)(now_ms - s_last_start_ms) >= APP_MEAS_PERIOD_MS) {
            if (bsp_adc_start(s_adc_buf, OP_DSP_N) == 0) {
                s_last_start_ms = now_ms;
                s_state = ENG_ACQ;
            } /* start 失败（上一帧没取走）下一拍再试 */
        }
        break;

    case ENG_ACQ:
        if (bsp_adc_poll_done() != 0) {
            bsp_adc_stop(); /* 帧已到手：先停触发，处理期间不再采 */
            s_state = ENG_PROC;
            (void)process_frame(); /* ~40ms，本拍内完成（超过 10ms 节拍：见文件头） */
            s_state = ENG_IDLE;
            s_last_start_ms = now_ms; /* 处理本身耗掉的时间算进下一周期 */
        }
        break;

    case ENG_PROC:
    default:
        s_state = ENG_IDLE; /* 不可达：防御走不到的态 */
        break;
    }
}

void app_engine_remeasure(void) {
    if (s_state == ENG_IDLE) {
        s_last_start_ms = 0U; /* 令周期判断立即成立 —— 不抢占进行中的帧 */
    }
}

const meas_result_t *app_engine_result(void) {
    return &s_result;
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

int app_engine_cal_step(void) {
    if (!s_cal_calibrating || s_cal_idx >= OP_CAL_POINTS) {
        return -1;
    }

    /* 同步测一帧：启动 → 忙等 DMA（≤2.048ms）→ DSP 链。
     * ⚠️ 阻塞 ~40ms，只能从 LVGL 按钮回调里调（不碰任务节拍语义）。 */
    if (bsp_adc_start(s_adc_buf, OP_DSP_N) != 0) {
        return -1;
    }
    while (bsp_adc_poll_done() == 0) {
        ; /* 2ms 忙等：标定是"人点一下测一次"的流程，可接受 */
    }
    bsp_adc_stop();
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
