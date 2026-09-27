#ifndef APP_LVGL_H
#define APP_LVGL_H
/* ============================================================================
 *  app_lvgl —— LVGL 9.5 与硬件的**适配层**（显示 + 触摸）
 *
 *  ---- 它在哪一层、为什么在这 ----
 *  本文件属于 `Task/`（`task_root` target）。它必须**同时**看到
 *      · `lvgl.h`                     （GUI 库）
 *      · `bsp_lcd.h` / `bsp_touch.h`  （Device 层）
 *  而 `Operation/` 不许碰硬件（规矩 2）、`Device/` 不该反过来依赖 GUI 库
 *  （依赖方向会倒过来）⇒ 只有 Task 层容得下这个组合。
 *  它是**适配**，不是业务，所以不放进 Operation。
 *
 *  ---- 它到底做了什么 ----
 *  显示：`lv_display_create()` 建一个 480×320 的显示，颜色格式
 *        **`LV_COLOR_FORMAT_RGB565`（不是 SWAPPED）**，单缓冲 PARTIAL；
 *        flush 回调只干两件事 —— `bsp_lcd_set_window()` + `bsp_lcd_write_pixels()`，
 *        最后**必须** `lv_display_flush_ready()`（漏了界面永久卡死）。
 *        ⇒ **不用 LVGL 内置的 ST7796 驱动**（文档 §6.6）：那个驱动是给 SPI/通用
 *        MIPI 写的，颜色数据走回调逐块送；我们要的是 8080 下 16 位紧循环直写。
 *        屏的初始化序列在 `Device/Src/bsp_lcd.c`（来自屏厂例程）。
 *  触摸：`lv_indev` 的 read 回调 → `bsp_touch_read()`（FT6336，I2C1 轮询）。
 *  时基：`lv_tick_set_cb(HAL_GetTick)` —— **只用这一种**（文档 §5.8：
 *        与手动 `lv_tick_inc` 二选一，都用会让动画速度翻倍）。
 *
 *  ---- 与配置文档的对应 ----
 *  文档 §5.4 把这一层拆成 `lv_port_disp.c` / `lv_port_indev.c` / `ui.c` 三个文件；
 *  本工程沿用原有的 `app_lvgl.c` 单文件（文件命名是工程约定，不是配置口径）。
 *  文档 §5.5 / §5.6 / §5.7 的**顺序与代码形态**在本文件里逐条落实。
 *
 *  ---- ⚠️ 未上板验证 ----
 *  屏幕能亮、颜色对不对、触摸坐标准不准，都要上板才知道。
 *  分步验证顺序见配置文档 §12（先纯色 → 再 Hello → 再真 UI），别一上来就搬 UI。
 * ==========================================================================*/

/* 初始化整条显示链路。返回 0 = 成功，-1 = 屏或 LVGL 起不来。
 *
 * 顺序**不可换**（文档 §5.5）：
 *     lv_init → lv_tick_set_cb → bsp_lcd_init / bsp_touch_init
 *             → 显示 → 输入 → UI
 *
 * ⚠️ 由组合根 `app_init()` 在**调度器启动之前**调用（见 app.h 的时序说明）。
 *    `lv_init()` 会建一个 FreeRTOS 递归互斥量（吃 FreeRTOS 堆），
 *    调度器启动前建互斥量是允许的（heap_4 只是分配内存）。
 *    （文档 §5.5 把它放在 LVGL 任务里；本工程提前到 app_init，是为了让
 *      `app_self_test()` 有机会验证屏与触摸 —— 顺序表本身逐条照做。）
 * ⚠️ 触摸初始化失败**不返回错误** —— 只显示不触摸是可用的降级。 */
int app_lvgl_init(void);

/* UI 主循环：**不返回**。任务体 `Task_Ui` 调它。
 *
 * ⚠️ `lv_timer_handler()` **只能在这一个任务里调用**（文档 §13-9）：
 *    并发会破坏 LVGL 内部状态；在 ISR 里调更不行。 */
void app_lvgl_run(void);

#endif /* APP_LVGL_H */
