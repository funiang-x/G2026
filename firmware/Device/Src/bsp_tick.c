/* bsp_tick 实现（ST HAL 后端）。
 *
 * ★ 本工程的 HAL 时基**不是 SysTick，是 TIM6** —— FreeRTOS 必须独占 SysTick。
 *   证据链（三处必须一致，任一处被改动都会立刻表现为"延时时长不对"）：
 *     · `.ioc` 的 `NVIC.TimeBaseIP=TIM6`
 *     · `Core/Src/stm32f4xx_hal_timebase_tim.c` 里的 `HAL_InitTick()` 实现
 *     · `Core/Src/main.c` 的 `HAL_TIM_PeriodElapsedCallback()` 里
 *       对 `htim->Instance == TIM6` 调 `HAL_IncTick()`
 *
 *   TIM6 的中断优先级是 `TICK_INT_PRIORITY`（stm32f4xx_hal_conf.h，= 15，最低），
 *   数值上**低于** FreeRTOS 的 `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY`(5)
 *   ⇒ 它**不被 FreeRTOS 临界区屏蔽**，所以临界区里 `HAL_GetTick()` 照样推进。
 *   （反过来才会出事：若 HAL 时基优先级高于那个阈值，进临界区后 tick 会停。）
 *
 * ⚠️⚠️ `HAL_Delay()` 在 FreeRTOS 下是**忙等**，不让出 CPU。详见 bsp_tick.h。 */
#include "bsp_tick.h"

#include "stm32f4xx_hal.h"

/* 1ms 时基由 HAL_Init() 按 TICK_INT_PRIORITY 配好（TIM6，不是 SysTick）——
 * 本层不再显式重调一次：那是纯转发，反而让"时基在哪配的"有两个答案。 */
uint32_t bsp_tick_ms(void) {
    return HAL_GetTick();
}

uint32_t bsp_tick_sysclk_hz(void) {
    return HAL_RCC_GetHCLKFreq();
}

/* ---- DWT 周期计数器（2026-10-14 八轮从 bsp_lcd **搬过来**）----
 * why 搬：它是"一段代码花了多久"的通用时基，跟 LCD 没有任何关系；而 Task 层不许引
 *   HAL/DWT（规矩），所以必须落在 Device 层 —— 时基层（本文件）才是它的家。
 *   原来的名字 `bsp_lcd_cyc_*` 会让"引擎计时"看起来像在量屏。
 * ⚠️ 首次调用自动打开 TRCENA/CYCCNT（调试单元，对正常运行无副作用）；
 *   32 位自由运行（144MHz ⇒ 29.8s 回绕）⇒ **只做差值**用，别当绝对时间。 */
uint32_t bsp_tick_cyc_now(void) {
    static int on = 0;
    if (on == 0) {
        on = 1;
        CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
        DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
        DWT->CYCCNT = 0U;
    }
    return DWT->CYCCNT;
}

uint32_t bsp_tick_cyc_to_us(uint32_t cyc) {
    return (uint32_t)((uint64_t)cyc * 1000000ULL / (uint64_t)SystemCoreClock);
}
