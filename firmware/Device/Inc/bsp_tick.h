#ifndef BSP_TICK_H
#define BSP_TICK_H
/* bsp_tick —— 毫秒时基。
 * 上层只调 bsp_tick_ms()（需要阻塞延时就用 osDelay），不直接依赖 HAL_GetTick，
 * 将来换 RTOS 或换芯片时只改这一层。
 *
 * ⚠️ **两个独立时基**（本工程实测确认）：`bsp_tick_ms()` 走 **TIM6**（HAL 时基），
 *    `osDelay()` 走 **SysTick**（FreeRTOS 独占）。两者都是 1ms 但各自计数，
 *    长期有微小漂移 —— 别把一边的值当另一边用。
 *    为什么 HAL 时基要挪到 TIM6：FreeRTOS 必须独占 SysTick，证据链见 bsp_tick.c。 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 自启动以来的毫秒数，32 位会在大约 49.7 天后回绕。
 * 比较时间差请用无符号减法： (uint32_t)(now - last) >= period ，天然抗回绕。 */
uint32_t bsp_tick_ms(void);

/* 当前 SYSCLK 频率（Hz）。用于自检时把"实际值"和"期望值"对一下 ——
 * 时钟配错的话这里会立刻显形（比如 HSE 起振失败退化成 HSI 16MHz）。
 * ⚠️ 它返回的是 HAL 记录的值，不是示波器量出来的；能证明"软件认为自己在跑多快"。 */
uint32_t bsp_tick_sysclk_hz(void);

/* ---- DWT 周期计数器（"这段代码花了多久"的时基，2026-10-14 八轮从 bsp_lcd 搬来）----
 * `bsp_tick_cyc_now()` 首次调用自动打开 TRCENA/CYCCNT（调试单元，对正常运行无副作用），
 * 返回 CPU 周期计数（32 位自由运行 ⇒ 只做**差值**，不要当绝对时间）；
 * `bsp_tick_cyc_to_us()` 把周期差换算成微秒。 */
uint32_t bsp_tick_cyc_now(void);
uint32_t bsp_tick_cyc_to_us(uint32_t cyc);

#ifdef __cplusplus
}
#endif
#endif /* BSP_TICK_H */
