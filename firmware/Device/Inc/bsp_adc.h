#ifndef BSP_ADC_H
#define BSP_ADC_H
/* ============================================================================
 *  bsp_adc —— 采样链：**TIM8 触发 ADC1 → DMA2_Stream0 搬进内存**
 *
 *  依据：总设计 §2.1 / §5.6。参数全部来自 `board_config.h`，本层不写魔数。
 *
 *      TIM8 (PSC=0, ARR=71, TRGO=Update)  →  2.000 MHz
 *          ↓ 每个更新事件触发一次转换
 *      ADC1 (PA0/IN0, 采样 3 周期, ADCCLK=36MHz)  →  转换 416.67ns
 *          ↓
 *      DMA2_Stream0（循环模式，半字）  →  N=4096 个点进内存
 *
 *  ---- ⚠️ 为什么 ADC 和 DMA 配在本层，而不是 CubeMX ----
 *  CubeMX 会**静默丢掉整个 ADC1 IP**（写任何参数都一样：Mcu.IP 里没有 ADC1、
 *  不生成 adc.c、DMA 报 `Id (ADC1) not set`），而 FSMC 参数会让它卡死。
 *  这两处只能在本层配 —— 好处是采样参数与总设计 §2.1 的常量放在一起，
 *  改采样率时不会漏改。
 *  ⚠️ 代价：**ADC/DMA 的引脚与时钟不在 `.ioc` 里**，`fw.py lint` 对它们
 *     只会报"仅 board_config.h 有值"（提示，不是失败）。
 *
 *  ---- ⚠️ 内存铁律 ----
 *  **`buf` 必须指向 SRAM1/SRAM2，绝不能是 CCM。**
 *  CCM 不可被 DMA 访问，放错**不报错、不告警**，采回来恒为 0。
 *  本层不校验这件事（校验不了）—— 调用方负责。
 *
 *  ---- ⚠️ 未上板验证 ----
 *  "TIM8 真的能触发 ADC1、DMA 真的搬对地方"没在实物上跑过。
 *  首次上板先看 `bsp_adc_self_check()` 报的 min/max。
 * ==========================================================================*/
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化：ADC 时钟（36MHz）+ PA0 模拟模式 + ADC1 + DMA2_Stream0 + NVIC。
 * 返回 0 = 成功。
 *
 * ⚠️ 必须在 `MX_TIM8_Init()` 之后调（TIM8 由 CubeMX 生成，本层只负责启动它）。
 * ⚠️ 本函数**不启动采集** —— 采集是门控的，由 `bsp_adc_start()` 显式发起。 */
int bsp_adc_init(void);

/* 启动一帧采集（**非阻塞**）：DMA 把 `n` 个点搬进 `buf`，满了置完成标志。
 * 返回 0 = 已启动，-1 = 参数非法 / 上一帧还没取走。
 *
 * ⚠️ `n` 不能超过 DMA 配置时的缓冲长度（BOARD_ADC_BUF_LEN）。 */
int bsp_adc_start(uint16_t *buf, uint32_t n);

/* 查询本帧是否已采满（非阻塞）。
 * 返回 1 = 采满（调用方应尽快取走数据），0 = 还在采。 */
int bsp_adc_poll_done(void);

/* 停采集：停 TIM8 + 停 DMA。可重复调用。 */
void bsp_adc_stop(void);

/* 自检：跑一小段采集（64 点），报出点数与 min/max。
 * 返回 0 = DMA 确实搬回了数据（说明"触发→转换→DMA"整条链是通的）；
 * -1 = 超时（链没通）。
 * ⚠️ 判据是"搬回了数据"，**不是**"数值等于某个期望值" ——
 *    PA0 此刻可能悬空，读数是什么都正常。 */
int bsp_adc_self_check(void);

/* DMA 中断的转发入口（**胶水函数**，给 Core 的 USER CODE 段用）。
 *
 * ⚠️ 为什么要它：CubeMX **总会**生成一个空的 `DMA2_Stream0_IRQHandler()`
 *    （哪怕 .ioc 里没给 DMA 配请求 —— 2026-09-26 实测），而它在 USER CODE 段
 *    之外、按 R4 不能删。本层的 DMA 句柄按规矩是 `static`，也不能写进头文件。
 *    ⇒ 由本层导出这一个函数，`Core/Src/stm32f4xx_it.c` 的
 *      `USER CODE BEGIN DMA2_Stream0_IRQn 0` 段里写一行调用（与 bsp_fault.h
 *      在同一个文件里的用法一致）。
 *    它内部只做 `HAL_DMA_IRQHandler()` —— 业务在 XferCpltCallback 里，
 *    且那里也只允许"停 TIM8 + 置标志"（总设计 §5.6 的中断纪律）。 */
void bsp_adc_irq_handler(void);

#ifdef __cplusplus
}
#endif
#endif /* BSP_ADC_H */
