#ifndef BSP_ADC_H
#define BSP_ADC_H
/* ============================================================================
 *  bsp_adc —— 采样链：**TIM8 触发 ADC1 → DMA2_Stream0 搬进内存**
 *
 *  依据：总设计 §2.1 / §5.6。参数来自 `board_config.h` + **CubeMX 的 .ioc
 *  （2026-09-28 起外设配置一律 GUI 化）**，本层不写魔数。
 *
 *      TIM8 (PSC=0, ARR=71, TRGO=Update)  →  2.000 MHz
 *          ↓ 每个更新事件触发一次转换
 *      ADC1 (PA1/IN1, 采样 3 周期, ADCCLK=36MHz)  →  转换 416.67ns
 *          ↓
 *      DMA2_Stream0（循环模式，半字）  →  N=4096 个点进内存
 *
 *  ---- 配置的归属（2026-09-28 起）----
 *  外设本体（通道/触发源/采样时间/DMA 流/NVIC）**全部由 CubeMX 生成**：
 *  `MX_ADC1_Init` / `MX_DMA_Init`（main 里已调）配置 `hadc1`/`hdma_adc1`。
 *  本层**只做运行期动作**：启动一帧（HAL_ADC_Start_DMA + 放行 TIM8）、
 *  查完成标志、收尾（先停 TIM8 再 Stop_DMA）—— 不写任何寄存器。
 *  想改采样率/触发/采样时间：去 CubeMX 改 .ioc 重新生成（铁律 7），
 *  然后核对 board_config.h 的 BOARD_ADC_* 宏（fw.py lint 对账）。
 *
 *  ---- ⚠️ 内存铁律 ----
 *  **`buf` 必须指向 SRAM1/SRAM2，绝不能是 CCM。**
 *  CCM 不可被 DMA 访问，放错**不报错、不告警**，采集结果恒为 0。
 *  ⇒ `bsp_adc_start()` 里有 CCM 落区守卫（按 board_config 的基址/容量拒绝启动），
 *    但"SRAM 内被别的数据踩"仍靠调用方保证 —— engine 的缓冲是静态区，安全。
 *
 *  ---- ⚠️ 未上板验证 ----
 *  "TIM8 真的能触发 ADC1、DMA 真的搬对地方"没在实物上跑过。
 *  首次上板先看 `bsp_adc_self_check()` 报的 min/max。
 * ==========================================================================*/
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化：记录就绪状态（外设本体已由 CubeMX 生成的 MX_DMA_Init / MX_ADC1_Init
 * 在 main() 里配好）。
 * 返回 0 = 成功。
 *
 * ⚠️ 必须在 `MX_DMA_Init()` / `MX_ADC1_Init()` / `MX_TIM8_Init()`（main 里）
 *    之后调。
 * ⚠️ 本函数**不启动采集** —— 采集是门控的，由 `bsp_adc_start()` 显式发起。 */
int bsp_adc_init(void);

/* 启动一帧采集（**非阻塞**）：HAL_ADC_Start_DMA 把 `n` 个点搬进 `buf`，满了置标志。
 * 返回 0 = 已启动，-1 = 参数非法 / 上一帧还没取走。
 *
 * ⚠️ `n` 不能超过 DMA 配置时的缓冲长度（BOARD_ADC_BUF_LEN）。
 * ⚠️ DMA 是**循环模式**：搬满后 HAL 不停 —— 完成回调里已停 TIM8（断触发源），
 *    调用方拿到 wait_frame 后应尽快 bsp_adc_stop() 收尾再开下一帧。 */
int bsp_adc_start(uint16_t *buf, uint32_t n);

/* 阻塞等本帧采满（**任务上下文专用**）。
 * 返回 0 = 采满；-1 = 超时或信号量未就绪。
 *
 * 机制：DMA 完成中断（`HAL_ADC_ConvCpltCallback`）里 `xSemaphoreGiveFromISR`，
 * 本函数 `xSemaphoreTake` —— 采集期间任务**让出 CPU**，不再轮询。
 * ⚠️ 调度器启动**前**不许调（FreeRTOS 阻塞 API 会失败）。数值自检（bsp_adc_self_check）在内部忙等。
 * ⚠️ timeout_ms 是"等一帧"的上限；正常一帧 2.048ms（4096 点 @2MSPS）。 */
int bsp_adc_wait_frame(uint32_t timeout_ms);

/* 停采集：先停 TIM8（断触发源）再 HAL_ADC_Stop_DMA。可重复调用。 */
void bsp_adc_stop(void);

/* 自检：跑一小段采集（64 点），报出点数与 min/max。
 * 返回 0 = DMA 确实搬回了数据（说明"触发→转换→DMA"整条链是通的）；
 * -1 = 超时（链没通）。
 * ⚠️ 判据是"搬回了数据"，**不是**"数值等于某个期望值" ——
 *    PA1 此刻可能没接信号源，读数是什么都正常。 */
int bsp_adc_self_check(void);

/* （2026-09-28 退役）旧的 `bsp_adc_irq_handler()` 转发入口已删除：
 * 用户在 .ioc 勾选 DMA2_Stream0 中断后，CubeMX 生成的
 * `DMA2_Stream0_IRQHandler → HAL_DMA_IRQHandler(&hdma_adc1)` 直接接管，
 * 不再需要本层转发；完成事件经 `HAL_ADC_ConvCpltCallback`（bsp_adc.c）进来。 */

#ifdef __cplusplus
}
#endif
#endif /* BSP_ADC_H */
