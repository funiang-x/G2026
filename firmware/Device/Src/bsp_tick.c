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

void bsp_tick_init(void) {
    /* HAL_Init() 内部已经按 TICK_INT_PRIORITY 配好 1ms 时基（TIM6，不是 SysTick）；
     * 这里再显式调一次是有意的：让"毫秒时基从哪来"这个依赖在代码里可见，
     * 而不是靠某个远处的隐式初始化。幂等，重复调用无害。 */
    (void)HAL_InitTick(TICK_INT_PRIORITY);
}

uint32_t bsp_tick_ms(void) {
    return HAL_GetTick();
}

void bsp_delay_ms(uint32_t ms) {
    HAL_Delay(ms);
}

uint32_t bsp_tick_sysclk_hz(void) {
    return HAL_RCC_GetHCLKFreq();
}
