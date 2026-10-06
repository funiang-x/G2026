/* ============================================================================
 *  bsp_adc.c —— 采样链：TIM8 触发 ADC1 → DMA2_Stream0 · 见 bsp_adc.h
 *
 *  ---- 2026-09-28 起全面改走 CubeMX 生成物（用户拍板：外设一律 GUI 配置）----
 *  配置事实（.ioc，用户在 GUI 里亲手配的）：
 *    · ADC1：IN1@PA1、3 周期采样、**TIM8_TRGO 上升沿触发**、
 *      **DMA Continuous Requests = ENABLE**（DDS，转换完继续发 DMA 请求）
 *    · DMA：DMA2_Stream0、Circular、外设/内存都是 **Half Word**、High 优先级
 *    · NVIC：DMA2_Stream0 全局中断使能（优先级 5，it.c 里 CubeMX 已生成
 *      `HAL_DMA_IRQHandler(&hdma_adc1)` —— 本层的旧转发入口已随之退役）
 *  ⇒ 本文件**不再写任何寄存器**：MX_ADC1_Init/MX_DMA_Init/MX_TIM8_Init 在
 *    main() 里已把外设配好，本层只剩"启动一帧 / 查完成 / 停"三个动作，
 *    全部走 HAL。参数想改？去 CubeMX 改 .ioc 重新生成（铁律 7）。
 *
 *  ---- 本层不做的两件事（诚实标注，与旧版一致）----
 *    · **不做 ADC 自校准**：STM32F4 的 HAL 里**没有**校准 API
 *      （`HAL_ADCEx_Calibration_Start` 在 F4 HAL 中不存在）。片内失调是
 *      确定性误差，由总设计 §5.7 的 CAL 标定吸收。
 *    · **CCM 落区只在 start 拦一道**（地址范围守卫，见 bsp_adc_start）：
 *      "落在 SRAM 但被别的数据踩"这类错误仍由调用方保证 ——
 *      engine 的采集缓冲是静态区，安全。
 * ==========================================================================*/
#include "bsp_adc.h"

#include "board_config.h"
#include "log.h"

#include "adc.h" /* hadc1（CubeMX 生成，MX_ADC1_Init 已在 main 里跑过） */
#include "tim.h" /* htim8（CubeMX 生成，TRGO=Update，fs=2MHz 的触发源） */
#include "stm32f4xx_hal.h"

/* FreeRTOS：中断只投事件（给信号量），任务侧阻塞等待 —— 见 bsp_adc.h 的 wait_frame。
 * Device 层允许 FreeRTOS（docs/01：中断投递），且 include 路径由 device→stm32cubemx 提供。 */
#include "FreeRTOS.h"
#include "semphr.h"

static volatile int s_done = 0;   /* DMA 搬满标志：ISR 里置 1，主上下文读 */
static volatile int s_busy = 0;
static int          s_inited = 0;

/* 帧就绪二值信号量：ISR 给、任务取（bsp_adc_wait_frame）。
 * ⚠️ 在 bsp_adc_init() 里创建 —— 它在调度器启动**前**跑（app_init），
 *    FreeRTOS 允许在启动前创建内核对象；只是**不许**在启动前阻塞取用。 */
static SemaphoreHandle_t s_frame_sem = NULL;

/* 自检用的小缓冲。**必须在 SRAM1/SRAM2**（不能加 __attribute__ 到 .ccmram）：
 * DMA 访问不了 CCM。放这里就是普通 .bss，落在 SRAM 段。 */
static uint16_t s_probe_buf[64];

/* ------------------------------------------------------------------ 内部 */

/* ADC 的 DMA 传输完成回调 —— HAL_ADC_Start_DMA 挂好链路后，每搬满一帧
 * （HAL_ADC_Start_DMA 的 len 个半字）由 DMA 中断走到这里。
 *
 * ⚠️ **中断上下文**，只允许：停触发源 + 置标志。
 *    不许调 HAL_ADC_Stop_DMA（它取锁、可能阻塞，总设计 §5.6 的中断纪律）——
 *    真正的收尾由 bsp_adc_stop() 在主上下文做。
 * ⚠️ Circular 模式下 DMA 永不停 ⇒ 这里的"完成"是**逻辑帧**边界：调用方
 *    （engine）拿到标志后必须自己 bsp_adc_stop()，否则数据被循环覆盖。 */
void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc) {
    if (hadc->Instance == ADC1) {
        __HAL_TIM_DISABLE(&htim8); /* 先停触发源，防止继续转换把缓冲写脏 */
        s_done = 1;
        /* 投事件：唤醒在 bsp_adc_wait_frame() 上阻塞的测量任务。
         * ⚠️ DMA2_Stream0 的 NVIC 抢占优先级 = 5 = configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY
         *    ⇒ 允许调 FromISR 系列（数值再小就不许了，见 FreeRTOSConfig.h）。 */
        if (s_frame_sem != NULL) {
            BaseType_t woken = pdFALSE;
            (void)xSemaphoreGiveFromISR(s_frame_sem, &woken);
            portYIELD_FROM_ISR(woken);
        }
    }
}

/* ------------------------------------------------------------------ 对外 */
int bsp_adc_init(void) {
    /* MX_DMA_Init / MX_ADC1_Init（含 PA1 模拟脚、DMA 链接、NVIC）已在 main()
     * 里由 CubeMX 生成物完成 —— 本层只记状态。PA1 引脚事实登记在
     * board_config.h（BOARD_ADC_IN_*，与 .ioc 的 ADC1.Channel-1 一致）。 */
    if (s_frame_sem == NULL) {
        /* ⚠️ 建对象内部会进一次 FreeRTOS 临界区（xQueueGenericReset）；调度器启动前
         *    uxCriticalNesting 是哨兵值，退出时**不**恢复中断 ⇒ BASEPRI 被永久钉在
         *    configMAX_SYSCALL_INTERRUPT_PRIORITY → HAL 时基中断失效 → uwTick 冻结
         *    → HAL_Delay() 死循环（2026-10-02 上板实测，卡在 bsp_lcd_init）。
         *    机理全文见 bsp_log.c 顶部；这里建完必须显式把中断放回来。 */
        s_frame_sem = xSemaphoreCreateBinary();
        portENABLE_INTERRUPTS();
    }
    if (s_frame_sem == NULL) {
        LOG_E("[ADC] 帧信号量创建失败 —— 测量任务将无法阻塞等待");
        return -1;
    }
    s_done = 0;
    s_busy = 0;
    s_inited = 1;
    LOG_I("[ADC] 就绪（CubeMX 生成物）：ADCCLK=36MHz（PCLK2/2）· 采样 3 周期 · "
          "触发=TIM8_TRGO 上升沿 · DMA2_Stream0 循环半字 · 帧信号量已建");
    return 0;
}

int bsp_adc_start(uint16_t *buf, uint32_t n) {
    if (!s_inited || buf == NULL || n == 0U || n > BOARD_ADC_BUF_LEN) {
        return -1;
    }
    /* CCM 落区守卫：CCM 不可被 DMA 访问，传错了**不报错、采集恒为 0**
     * （最阴险的静默失效，见 bsp_adc.h 铁律）。全工程只有这里能拦住它 ——
     * 地址范围来自 board_config 登记的 CCM 基址/容量。 */
    if ((uint32_t)(uintptr_t)buf >= BOARD_CCM_BASE_ADDR &&
        (uint32_t)(uintptr_t)buf < BOARD_CCM_BASE_ADDR + BOARD_CCM_SIZE_KB * 1024U) {
        LOG_E("[ADC] buf 落在 CCM（0x%08lx）—— DMA 不可达，采集会恒为 0，已拒绝启动",
              (unsigned long)(uintptr_t)buf);
        return -1;
    }
    if (s_busy) {
        return -1; /* 上一帧还没收走 —— 调用方应先 poll_done + stop */
    }

    s_done = 0;
    s_busy = 1;

    /* 清掉可能残留的"上一帧"信号量（自检的忙等路径不走信号量，但超时路径会留），
     * 否则本次 wait_frame 会立刻返回、拿到还没搬完的数据。 */
    if (s_frame_sem != NULL) {
        (void)xSemaphoreTake(s_frame_sem, 0U);
    }

    /* 先挂 ADC+DMA，再放 TIM8 —— 反过来的话前几次触发会丢（DMA 还没就绪）。
     * HAL_ADC_Start_DMA 内部会启动 ADC（ADON）+ 使能 DMA 请求 + 挂完成回调。 */
    if (HAL_ADC_Start_DMA(&hadc1, (uint32_t *)(uintptr_t)buf, n) != HAL_OK) {
        s_busy = 0;
        return -1;
    }
    if (HAL_TIM_Base_Start(&htim8) != HAL_OK) {
        (void)HAL_ADC_Stop_DMA(&hadc1); /* 主上下文，允许 Stop */
        s_busy = 0;
        return -1;
    }
    return 0;
}

int bsp_adc_poll_done(void) {
    return s_done;
}

int bsp_adc_wait_frame(uint32_t timeout_ms) {
    if (s_frame_sem == NULL) {
        return -1;
    }
    if (xSemaphoreTake(s_frame_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return -1;
    }
    return 0;
}

void bsp_adc_stop(void) {
    /* ⚠️ 顺序：先停触发源（TIM8），再停搬运（ADC+DMA）。
     *    反过来会留下"还在转换但没人搬"的窗口 —— 数据丢在 DR 里，
     *    下一帧的第一个点会是上一帧的残留。 */
    (void)HAL_TIM_Base_Stop(&htim8);
    (void)HAL_ADC_Stop_DMA(&hadc1);
    s_busy = 0;
}

int bsp_adc_self_check(void) {
    uint32_t t0;
    uint32_t sum = 0;
    uint16_t lo = 0xFFFFU;
    uint16_t hi = 0;

    if (!s_inited) {
        LOG_W("[SELFTEST] ADC 未初始化，跳过");
        return -1;
    }

    if (bsp_adc_start(s_probe_buf, 64U) != 0) {
        LOG_W("[SELFTEST] ADC 启动失败");
        return -1;
    }

    /* 64 点 @2MHz = 32µs，给 20ms 余量足够。忙等而不是 HAL_Delay：
     * 自检可能在任何上下文跑，不想依赖 SysTick。
     * ⚠️ **本函数必须在调度器启动后调用**（本工程由 `app_adc_selftest()` 在
     *    `app_rtos_run()` 里调）：它要真启动一次采集，路上有 `bsp_adc_start()`
     *    的 `xSemaphoreTake()` 与 DMA 中断里的 `xSemaphoreGiveFromISR()` 两处
     *    FreeRTOS 原语；调度器启动前跑会被 ISR 入栈校验断言拦成死循环
     *    （2026-10-02 上板实测，PC 停在 vPortValidateInterruptPriority）。 */
    t0 = HAL_GetTick();
    while (!bsp_adc_poll_done()) {
        if ((HAL_GetTick() - t0) > 20U) {
            bsp_adc_stop();
            LOG_W("[SELFTEST] ADC 采集超时 —— TIM8 没触发？DMA 没搬？"
                  "（核对 .ioc：ADC1 触发源=Tim8 TRGO、DMA Continuous Requests、"
                  "DMA2_Stream0 循环半字）");
            return -1;
        }
    }
    bsp_adc_stop();

    for (uint32_t i = 0; i < 64U; i++) {
        sum += s_probe_buf[i];
        if (s_probe_buf[i] < lo) {
            lo = s_probe_buf[i];
        }
        if (s_probe_buf[i] > hi) {
            hi = s_probe_buf[i];
        }
    }
    const uint32_t mean = sum / 64U;

    /* 判据 1（**硬判据**）："搬回了 64 个点" —— 证明 TIM8 真在触发、DMA 真在搬。
     *   不判"数值必须等于某个期望值"：此刻可能接着信号源，瞬时值是多少都正常。 */
    LOG_I("[SELFTEST] ADC ok, 采到 64 点，码值范围 %u ~ %u，均值 %lu 码（≈%lu mV）",
          lo, hi, (unsigned long)mean,
          (unsigned long)(((uint32_t)mean * (uint32_t)BOARD_ADC_VREF_MV) / 4095U));

    /* 判据 2（**提示级**，2026-10-02 加）：帧**直流均值**应落在半量程带内 ——
     *   机理与容差见 `board_config.h` 的 `BOARD_VBIAS_SENSE_CODE_MID` 段：
     *   VBIAS = VREF/2 且 ADC 的 VREF+ 同源 ⇒ 半量程恒对应偏置；输入是 AC 耦合
     *   ⇒ 即便接着信号源，均值也≈偏置。带外只告警**不停机**（本工程口径：
     *   任何一项自检失败都不停机），因为 JP2 被人为断开时本判据会"误报"。 */
    {
        const uint32_t mid = (uint32_t)BOARD_VBIAS_SENSE_CODE_MID;
        const uint32_t band = (mid * (uint32_t)BOARD_VBIAS_SENSE_TOL_PCT) / 100U;
        const uint32_t lo_bound = (mid > band) ? (mid - band) : 0U;
        if (mean < lo_bound || mean > (mid + band)) {
            LOG_W("[SELFTEST] 模拟轨可疑：帧均值 %lu 码不在 %lu±%lu 带内"
                  "（VBIAS=VREF/2，无强直流输入时应在半量程）。依次查："
                  "① `JP2`(0Ω 断点) 是否被断开；② 模拟轨/基准是否有电"
                  "（万用表量 `TP5` 的 VBIAS ≈1.5V(REF3030)/1.65V(3V3)）；"
                  "③ 输入是否被强直流量顶住。"
                  "【台面上没有 AFE 硬件时（AFE 在底板上）本条属预期，可忽略】",
                  (unsigned long)mean, (unsigned long)mid, (unsigned long)band);
        }
    }
    return 0;
}
