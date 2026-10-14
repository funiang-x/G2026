#ifndef APP_LVGL_H
#define APP_LVGL_H
/* ============================================================================
 *  app_lvgl —— LVGL 与硬件的**适配层**（显示 + 触摸），以及它的节拍入口
 *
 *  ---- 为什么在 Task/ 层 ----
 *  本文件同时依赖 `lvgl.h`（GUI 库）与 `bsp_lcd.h` / `bsp_touch.h`（Device 层）：
 *    · `Operation/` 不许碰硬件（规矩 2）；
 *    · `Device/` 不该反过来依赖 GUI 库（依赖方向会倒过来）；
 *  ⇒ 只有 Task 层能容纳这个组合。它是**适配**，不是业务，所以不放进 Operation。
 *
 *  ---- 它到底做了什么 ----
 *  显示：`lv_st7796_create()`（LVGL 9.5 自带的 ST7796 驱动，初始化序列来自
 *        LovyanGFX，是权威来源）→ 我们只提供两个回调：
 *            发命令/参数、发像素 —— 内部就是 bsp_lcd 的 8080 原语。
 *        颜色格式用 `LV_COLOR_FORMAT_RGB565_SWAPPED`（屏要**大端**），
 *        字节序在绘制时顺手换掉，比在搬运循环里多一遍快。
 *  触摸：`lv_indev` 的 read 回调 → bsp_touch_read()（FT6336，I2C2 轮询）。
 *  节拍：`app_lvgl_poll(now_ms)` 里推 LVGL 的时基并跑 `lv_timer_handler()`。
 *
 *  ---- ⚠️ 未上板验证 ----
 *  屏幕能亮、颜色对不对、触摸坐标准不准，都要上板才知道。
 *  分步验证顺序见 `docs/`（先纯色 → 再 Hello → 再真 UI），别一上来就搬 UI。
 * ==========================================================================*/
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化：lv_init + 创建 display/indev + 开背光。
 * 返回 0 成功，-1 失败（多半是 LVGL 内存池太小：`LV_ASSERT_MALLOC`）。
 *
 * ⚠️ 必须在 `app_init()` 里、**bsp_lcd_init() 之后**调 —— 它依赖 FSMC 已配好
 *    （由 main() 的 `MX_FSMC_Init()` 完成）与屏已复位。 */
int app_lvgl_init(void);

/* 周期入口：推进 LVGL 时基 + 跑一次 `lv_timer_handler()`。
 *
 * ⚠️ `now_ms` 是**单调毫秒**（来自 bsp_tick_ms）。LVGL 的时基靠本函数
 *    按两次调用的**差值**推进（`lv_tick_inc`）—— 所以本函数的调用周期
 *    就是 LVGL 的时间分辨率，**不要低于 10ms**（LVGL 默认重绘周期 16ms）。
 * ⚠️ 本函数**不阻塞**：`lv_timer_handler()` 只跑到该干的事为止。
 *    真正的像素搬运是同步的（FSMC 直写，没有 DMA），所以屏越大它越占 CPU ——
 *    这是 8080 并口方案要接受的代价，换来的是不需要显存也不撕裂。 */
void app_lvgl_poll(uint32_t now_ms);

/* flush 像素速率（**kpx/s**，1 秒窗口）：上 1 秒"交给屏的像素数 / 1s"。
 * ⚠️ 它**不是帧率**（增量绘制下这个数本来就小）；页 2 的 BANDWIDTH 卡每秒读一次。
 * ⚠️ 与 `app_lvgl.c` 里 `[LVGL] diag` 共用同一个累加器，各自差分，谁都不要去清零。
 * （旧 API `app_lvgl_fps_x10()` 2026-10-14 八轮删除 —— 它读的"开机累计值 + 硬编码 5s
 *   分母"曾给出超过 FSMC 物理上限的假吞吐，见 app_lvgl.c 的注释。） */
uint32_t app_lvgl_flush_kpx_s(uint32_t now_ms);

#ifdef __cplusplus
}
#endif
#endif /* APP_LVGL_H */
