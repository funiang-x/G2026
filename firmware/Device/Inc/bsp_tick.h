#ifndef BSP_TICK_H
#define BSP_TICK_H
/* bsp_tick —— 毫秒时基。
 * 上层只调 bsp_tick_ms() / bsp_delay_ms()，不直接依赖 HAL_GetTick，
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

/* 初始化 1ms 时基（HAL_Init 已配好，这里是显式重申依赖，幂等可重入）。 */
void bsp_tick_init(void);

/* 自启动以来的毫秒数，32 位会在大约 49.7 天后回绕。
 * 比较时间差请用无符号减法： (uint32_t)(now - last) >= period ，天然抗回绕。 */
uint32_t bsp_tick_ms(void);

/* 忙等指定毫秒（基于时基中断，不是空转延时）。
 *
 * ⚠️⚠️ **在 FreeRTOS 下它是忙等，不让出 CPU** —— 在高优先级任务里调
 *    `bsp_delay_ms(1000)` 会**卡住整个系统 1 秒**（低优先级任务、idle 全停）。
 *    任务上下文请用 `osDelay()`；本函数只留给**调度器启动前**与
 *    **已经在临界区里**的场合（那两处不能调 osDelay）。 */
void bsp_delay_ms(uint32_t ms);

/* 当前 SYSCLK 频率（Hz）。用于自检时把"实际值"和"期望值"对一下 ——
 * 时钟配错的话这里会立刻显形（比如 HSE 起振失败退化成 HSI 16MHz）。
 * ⚠️ 它返回的是 HAL 记录的值，不是示波器量出来的；能证明"软件认为自己在跑多快"。 */
uint32_t bsp_tick_sysclk_hz(void);

#ifdef __cplusplus
}
#endif
#endif /* BSP_TICK_H */
