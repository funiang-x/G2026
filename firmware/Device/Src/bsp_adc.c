/* ============================================================================
 *  bsp_adc.c —— 采样链：TIM8 触发 ADC1 → DMA2_Stream0 · 见 bsp_adc.h
 *
 *  ---- ⚠️ 为什么 ADC 部分是**直接写寄存器**，没走 HAL ----
 *  两条硬约束叠在一起，只有这条路：
 *    ① CubeMX 会**静默丢掉整个 ADC1 IP** —— 不管参数怎么写（照官方 F4 板级
 *       参考的键集合写也一样）：`Mcu.IP` 里没有 ADC1、不生成 adc.c、
 *       `HAL_ADC_MODULE_ENABLED` 不开、DMA 报 `Id (ADC1) not set`。
 *    ② `HAL_ADC_MODULE_ENABLED` 是 **CubeMX 生成物**（stm32f4xx_hal_conf.h）。
 *       手工把它打开、再手工拷一份 HAL ADC 驱动进 Drivers/，在**下一次
 *       CubeMX 生成时会被静默抹掉** —— 那是最坏的一类失败（改了、看着对、
 *       某天重新生成后 ADC 代码突然编不过，或者更糟：编过但行为不对）。
 *  ⇒ ADC 的寄存器配置放在本层。DMA 仍用 HAL（`HAL_DMA_MODULE_ENABLED` 是
 *    开的，因为 DMA 是正经 IP）。
 *
 *  位定义**不是凭记忆写的**，来源逐个标注在下面：
 *    · 位宏名与位置：`Drivers/CMSIS/.../stm32f407xx.h`
 *    · EXTSEL 的编码：官方 HAL 头 `stm32f4xx_hal_adc.h` 里
 *      `ADC_EXTERNALTRIGCONV_T8_TRGO = EXTSEL_3|EXTSEL_2|EXTSEL_1`
 *    · 采样时间 3 周期 = SMP=000：同头文件 `ADC_SAMPLETIME_3CYCLES = 0`
 *
 *  ---- 本层不做的两件事（诚实标注）----
 *    · **不做 ADC 自校准**：STM32F4 的 HAL 里**没有**校准 API
 *      （`HAL_ADCEx_Calibration_Start` 在 F4 HAL 中不存在），CubeMX 生成的
 *      F4 ADC 代码同样不校准。片内失调是**确定性**误差，由总设计 §5.7 的
 *      CAL 标定吸收。
 *    · **不校验缓冲地址**：`buf` 落在 CCM 会导致采回来恒为 0 且不报错，
 *      本层无法检测（见 bsp_adc.h 的铁律）。
 * ==========================================================================*/
#include "bsp_adc.h"

#include "board_config.h"
#include "bsp_gpio.h"
#include "log.h"

#include "stm32f4xx_hal.h"
#include "tim.h" /* htim8（CubeMX 生成）—— 本层只负责启动/停止它 */

/* ---- 位定义：全部来自 stm32f407xx.h，不自己编 ---- */
#define ADC_EXTSEL_T8_TRGO (ADC_CR2_EXTSEL_3 | ADC_CR2_EXTSEL_2 | ADC_CR2_EXTSEL_1)

static ADC_TypeDef *const s_adc = ADC1; /* PA0 属于 ADC123 共用的通道 0，取 ADC1 */

static DMA_HandleTypeDef s_hdma;
static volatile int      s_done = 0; /* ISR 里置 1，主上下文读 */
static volatile int      s_busy = 0;
static int               s_inited = 0;

/* 自检用的小缓冲。**必须在 SRAM1/SRAM2**（不能加 __attribute__ 到 .ccmram）：
 * DMA 访问不了 CCM。放这里就是普通 .bss，落在 SRAM 段。 */
static uint16_t s_probe_buf[64];

/* ------------------------------------------------------------------ 内部 */

/* DMA 传输完成 —— **中断上下文**，只允许：停 TIM8 + 置标志。
 * ⚠️ 不许在这里调 HAL_DMA_Abort / HAL_ADC_Stop_DMA（总设计 §5.6 的中断纪律：
 *    HAL_*_Stop_DMA 会取锁、可能阻塞）。真正收尾由 bsp_adc_stop() 在主上下文做。 */
static void adc_dma_cplt(DMA_HandleTypeDef *hdma) {
    (void)hdma;
    __HAL_TIM_DISABLE(&htim8); /* 立刻停触发源，防止继续转换把缓冲写脏 */
    s_done = 1;
}

/* ------------------------------------------------------------------ 对外 */
int bsp_adc_init(void) {
    /* ---- ① 时钟 ----
     * ⚠️ ADCCLK 上限 36MHz。PCLK2 = 72MHz（SYSCLK 144 / APB2 ÷2）
     *    ⇒ ADCPRE = 00（PCLK2/2）= 36MHz。改成 /4 会让转换超时（见头文件推导）。 */
    __HAL_RCC_ADC1_CLK_ENABLE();
    __HAL_RCC_DMA2_CLK_ENABLE();
    /* ⚠️ ADCPRE 不在 ADC1 的寄存器里 —— 它在**ADC 公共**寄存器块（ADC123_COMMON）
     *    的 CCR 里，对所有 ADC 生效。`stm32f407xx.h` 里 `ADC` 这个宏就指向它
     *    （`#define ADC ADC123_COMMON`），所以这里写 `ADC->CCR`。 */
    ADC->CCR &= ~ADC_CCR_ADCPRE; /* ADCPRE[1:0] = 00 → PCLK2/2 = 36MHz */

    /* ---- ② PA0 配成模拟输入 ----
     * 模拟脚**必须**是 ANALOG 模式，否则数字输入缓冲的漏电会让读数飘。
     * ⚠️ 本层做这件事是因为 ADC 不在 .ioc 里（CubeMX 的 gpio.c 只管数字脚）。 */
    {
        const bsp_gpio_t pin = {(bsp_gpio_port_t)BOARD_ADC_IN_PORT, BOARD_ADC_IN_PIN};
        if (bsp_gpio_config(pin, BSP_GPIO_ANALOG, 0U) != 0) {
            LOG_E("[ADC] PA%d 配置为模拟输入失败", BOARD_ADC_IN_PIN);
            return -1;
        }
    }

    /* ---- ③ DMA2_Stream0 ----
     * ⚠️ ADC1 在 DMA2 上的映射是**固定**的（Stream0 / Channel 0），不能换。
     *    循环模式：满了不停，等主上下文收尾（门控采集靠停 TIM8 而不是停 DMA）。 */
    s_hdma.Instance = DMA2_Stream0;
    s_hdma.Init.Channel = DMA_CHANNEL_0;
    s_hdma.Init.Direction = DMA_PERIPH_TO_MEMORY;
    s_hdma.Init.PeriphInc = DMA_PINC_DISABLE;   /* 源固定是 ADC1->DR */
    s_hdma.Init.MemInc = DMA_MINC_ENABLE;       /* 目的递增 */
    s_hdma.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
    s_hdma.Init.MemDataAlignment = DMA_MDATAALIGN_HALFWORD; /* 12 位结果占一个半字 */
    s_hdma.Init.Mode = DMA_CIRCULAR;
    s_hdma.Init.Priority = DMA_PRIORITY_HIGH;
    s_hdma.Init.FIFOMode = DMA_FIFOMODE_DISABLE;
    s_hdma.XferCpltCallback = adc_dma_cplt;
    if (HAL_DMA_Init(&s_hdma) != HAL_OK) {
        LOG_E("[ADC] HAL_DMA_Init 失败");
        return -1;
    }

    /* 优先级 5 = configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY（总设计 §5.6） */
    HAL_NVIC_SetPriority(DMA2_Stream0_IRQn, BOARD_ADC_DMA_IRQ_PRIORITY, 0);
    HAL_NVIC_EnableIRQ(DMA2_Stream0_IRQn);

    /* ---- ④ ADC1 ----
     * CR1：扫描关（只有 1 个通道）、不开中断（用 DMA 完成中断就够）
     * CR2：DMA 使能 + DDS（转换完继续发 DMA 请求，不自动关）+ 上升沿外部触发
     *      + EXTSEL = TIM8_TRGO + ADON 上电
     * SQR1：L=0 ⇒ 规则组 1 个转换
     * SQR3：SQ1 = 0 ⇒ 通道 0（PA0）
     * SMPR2：SMP0 = 000 ⇒ 3 个 ADCCLK 采样 ⇒ 转换 15 ADCCLK = 416.67ns */
    s_adc->CR1 = 0U;
    s_adc->CR2 = 0U;
    s_adc->SQR1 = 0U;
    s_adc->SQR3 = BOARD_ADC_CHANNEL & 0x1FU;
    s_adc->SMPR2 = 0U; /* SMP0 = 000 = 3 周期（ADC_SAMPLETIME_3CYCLES = 0） */
    s_adc->CR2 = ADC_CR2_DMA | ADC_CR2_DDS | ADC_CR2_EXTEN_0 | ADC_EXTSEL_T8_TRGO |
                 ADC_CR2_ADON;

    /* 上电后要等 tSTAB（典型 3µs）才开始转换 —— 这里用忙等，**不能** HAL_Delay：
     * 本函数可能在任何上下文被调，而 HAL_Delay 依赖 SysTick 中断。 */
    for (volatile uint32_t i = 0; i < 2000U; i++) {
        __NOP();
    }

    s_inited = 1;
    LOG_I("[ADC] 就绪：ADCCLK=%luMHz（PCLK2/2）· 采样 %lu 周期 · 触发=TIM8_TRGO 上升沿",
          (unsigned long)(BOARD_ADCCLK_HZ / 1000000U), (unsigned long)BOARD_ADC_SAMPLING_CYCLES);
    return 0;
}

/* DMA 完成中断的转发入口 —— 由 `Core/Src/stm32f4xx_it.c` 的 USER CODE 段调用
 * （见 bsp_adc.h 的说明：CubeMX 总会生成一个空的 DMA2_Stream0_IRQHandler，
 *  所以本层**不能**再定义同名函数，否则 multiple definition）。
 *
 * ⚠️ 句柄保持 `static`（本层私有）—— 这正是不能把它写进头文件、而要导出一个
 *    转发函数的原因（docs/01：HAL 句柄不写进对外头文件）。 */
void bsp_adc_irq_handler(void) {
    HAL_DMA_IRQHandler(&s_hdma);
}

int bsp_adc_start(uint16_t *buf, uint32_t n) {
    if (!s_inited || buf == NULL || n == 0U || n > BOARD_ADC_BUF_LEN) {
        return -1;
    }
    if (s_busy) {
        return -1; /* 上一帧还没收走 —— 调用方应先 poll_done + stop */
    }

    s_done = 0;
    s_busy = 1;

    /* 先挂 DMA，再放 TIM8 —— 反过来的话前几次触发会丢（DMA 还没就绪）。 */
    if (HAL_DMA_Start_IT(&s_hdma, (uint32_t)&s_adc->DR, (uint32_t)buf, n) != HAL_OK) {
        s_busy = 0;
        return -1;
    }
    if (HAL_TIM_Base_Start(&htim8) != HAL_OK) {
        (void)HAL_DMA_Abort(&s_hdma);
        s_busy = 0;
        return -1;
    }
    return 0;
}

int bsp_adc_poll_done(void) {
    return s_done;
}

void bsp_adc_stop(void) {
    /* ⚠️ 顺序：先停触发源（TIM8），再停搬运（DMA）。
     *    反过来会留下"还在转换但没人搬"的窗口 —— 数据丢在 DR 里，
     *    下一帧的第一个点会是上一帧的残留。 */
    (void)HAL_TIM_Base_Stop(&htim8);
    (void)HAL_DMA_Abort(&s_hdma);
    s_busy = 0;
}

int bsp_adc_self_check(void) {
    uint32_t t0;
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
     * 自检可能在任何上下文跑，不想依赖 SysTick。 */
    t0 = HAL_GetTick();
    while (!bsp_adc_poll_done()) {
        if ((HAL_GetTick() - t0) > 20U) {
            bsp_adc_stop();
            LOG_W("[SELFTEST] ADC 采集超时 —— TIM8 没触发？DMA 没搬？"
                  "（检查 TIM8 是否真的在跑、ADC1->CR2 的 EXTEN/EXTSEL）");
            return -1;
        }
    }
    bsp_adc_stop();

    for (uint32_t i = 0; i < 64U; i++) {
        if (s_probe_buf[i] < lo) {
            lo = s_probe_buf[i];
        }
        if (s_probe_buf[i] > hi) {
            hi = s_probe_buf[i];
        }
    }
    /* 判据是"搬回了 64 个点"，**不是**"数值等于某个期望值"：
     * PA0 此刻可能悬空，读数是多少都正常。 */
    LOG_I("[SELFTEST] ADC ok, 采到 64 点，码值范围 %u ~ %u（12 位满量程 4095）", lo, hi);
    return 0;
}
