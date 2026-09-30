#ifndef PROJECT_CONFIG_H
#define PROJECT_CONFIG_H
/* ============================================================================
 *  project_config.h —— 应用参数与功能开关（**与硬件无关**）
 *
 *  四层架构里的位置（见 docs/01）：
 *    · Task / Operation 都 include 它（本文件在工程根，两层都能看到）
 *    · **它里面不许出现任何 HAL 内容** —— Operation 层要在 PC 上编译，
 *      引了硬件头就编不过。硬件事实一律去 Core/Inc/board_config.h。
 *
 *  分工：
 *    Core/Inc/board_config.h   引脚、时钟、外设实例（"这块板子上有什么"）
 *    project_config.h（本文件）  周期、阈值、功能开关（"产品要怎么做"）
 *
 *  ⚠️ 写业务时把阈值 / 周期 / 开关放这里，**不要散落到 .c 里**（本工程禁止魔数）。
 *     加的时候照下面这条规矩：
 *
 *     · Operation 层的参数**不从本文件读** —— 由 Task 层在初始化时**注入**给
 *       Operation 实例（它是纯逻辑，配置也要当参数进来，那样才好在 PC 上单测）。
 *     · 本文件里**不许出现任何 HAL 内容**（Operation 要在 PC 上编译）。
 *     · 硬件事实（引脚/时钟）不在这儿，去 Core/Inc/board_config.h。
 * ==========================================================================*/

/* ---------------------------------------------------------------- 基本节拍 ---- */

/* 业务节拍：周期任务每次推进的间隔。
 *
 * ★ 2026-09-26 从 100ms 改成 **10ms** —— 因为 LVGL 的时基靠本循环推进
 *   （`app_lvgl_poll()` 用两次调用的差值调 `lv_tick_inc()`）。
 *   100ms 的节拍会让 LVGL 的时间分辨率变成 100ms：动画一顿一顿，
 *   触摸响应最多迟 100ms（手感"粘"）。
 *   10ms 是折中：比 LVGL 自己的重绘周期（16ms）密，又不至于空转太频繁。
 *   ⚠️ 改成大于 16ms 的值会让 LVGL 的重绘被拖慢（它能跑，但会掉帧）。 */
#define APP_TICK_PERIOD_MS 10U

/* 心跳日志周期（让 RTT 上能看出"系统还活着"）。 */
#define APP_HEARTBEAT_PERIOD_MS 5000U

/* ------------------------------------------------------------ LVGL 参数 ---- */

/* draw buffer 的行数：缓冲 = 屏宽 × 本值 × 2 字节。
 *   320 × 48 × 2 = 30.7 KB（在 SRAM1，见 Task/Src/app_lvgl.c）。
 * ⚠️ 调大省 flush 次数（FSMC 每次开窗有固定开销），但吃 RAM；
 *    2026-09-28 从 40 提到 48：RAM 余量允许（用量 65/128KB），换重绘块数 -20%。
 * ⚠️ 这个缓冲**不能放 CCM**：本工程走 CPU 直写 FSMC 时能跑，
 *    但将来若改成 DMA 搬 FSMC，CCM 会**静默失效**（不报错、花屏/黑屏）。
 *    放 SRAM1 就没有这个隐患，代价只是访问慢一点点。 */
#define APP_LVGL_DRAW_BUF_LINES 48U

/* ------------------------------------------------------------ FreeRTOS 参数 ----
 * ⚠️ 任务**不要用 CubeMX 的图形界面建** —— CubeMX 里只勾 FreeRTOS 内核（docs/01 规矩 4）。
 *
 * ★ CubeMX 生成的 `defaultTask`（**本工程唯一"生成"的任务**）只跑轻量周期：
 *     栈深 / 优先级   → `.ioc` 的 `FREERTOS.Tasks01`（256 words / 优先级 24）
 *                        ⚠️ 它由 CubeMX 生成，参数**只能在 CubeMX 里给**，
 *                           写在这里的宏不会被读 —— 所以这里不重复定义（避免孤儿宏）。
 *     循环体          → `Task/Src/app_rtos.c` 的 `app_rtos_run()`（心跳/闪灯/RTC + 建下面两个任务）
 *     ⚠️ 栈深单位是 **word（4 字节）**，不是字节。256 words = 1 KiB。
 *     业务（测量 / UI）**不在它里面** —— 见下面的双任务小节。
 *
 * ⇒ **加新任务时**才在下面定义它的参数，并在 `Task/` 层用 `xTaskCreate` 建。
 *   例如：
 *       #define APP_SENSOR_TASK_STACK_WORDS 256U
 *       #define APP_SENSOR_TASK_PRIORITY    4U   // 比 defaultTask(24) 高才抢得到 CPU
 *   ⚠️ 优先级数值越大越优先；`configMAX_PRIORITIES` = 56。 */

/* ---- 测量 / UI 双任务（2026-09-28：从"单循环"改为真多任务）----
 *
 *  ★ 为什么现在拆：单循环里 engine 与 ui 抢同一拍 CPU，靠"DSP 拆两拍"绕开
 *    帧抖动；采集完成靠 10ms 轮询（最多白等一拍）。拆成任务后：
 *      · engine 阻塞在 ADC 帧信号量上 —— **零轮询延迟、采集期让出 CPU**；
 *      · 重测请求走任务通知（轻量二值信号量），切页即刻响应，不等下一拍；
 *      · 结果走 1 深 mailbox 队列（跨任务双缓冲），UI 永不读到写一半的结构。
 *
 *  ⚠️ 优先级都相对 CubeMX 的 defaultTask(24) 定（那个值只能在 .ioc 里改）：
 *      engine 26 > defaultTask 24 > ui 23 —— 测量永远优先于界面。
 *  ⚠️ 栈深单位是 **word（4 字节）**。UI 栈要给 LVGL 重绘 + 标定时的 DSP 链
 *     （标定按钮回调在 UI 任务里跑同步测量）留余量，故比 engine 大。
 *  ⚠️ 两个任务都用 `xTaskCreateStatic`（栈/TCB 是静态数组）—— 不动 FreeRTOS 堆。 */
#define APP_TASK_ENGINE_PRIORITY 26U
#define APP_TASK_ENGINE_STACK_WORDS 1024U
#define APP_TASK_UI_PRIORITY 23U
#define APP_TASK_UI_STACK_WORDS 1536U

/* 等一帧 DMA 的上限。正常一帧 4096 点 @2MSPS = 2.048ms；给 50ms 是"链断了
 * 也不永久挂死"的看门狗口径（超时由 engine 报错并跳过本帧）。 */
#define APP_ADC_FRAME_TIMEOUT_MS 50U

/* 任务栈高水位上报的时刻（上电后 ms，一次）。要晚到"DSP 全链 + LVGL 重绘
 * 都跑过几轮"之后才有意义；标定路径（UI 任务最坏栈）要人手动触发，所以
 * 想验 UI 栈余量，就**在这个时刻之前完成一次标定**（见 app_tasks_report_stack）。 */
#define APP_STACK_REPORT_MS 10000U

/* ------------------------------------------------------- AI 闭环验证标记 ---- */

/* ⚠️ 启动自检**不在上电瞬间跑**，而是延迟这么久、在任务里跑一次。
 *
 * 为什么（实测踩过）：`fw.py verify` 的顺序是"先烧录复位放行、再启动 RTT 主机"。
 * 目标从复位到输出完的那一瞬间，主机**还没挂上**；而 RTT 上行缓冲只有 1024 B
 * 且满则**静默丢** ⇒ 主机挂上时只能读到最早那 1024 B，中间的输出全被丢掉。
 * 实测现象：153 个值只收到 18 个，日志在缓冲容量处直接跳到结束标记 ——
 * 看起来像"算错了"，其实是**采集端来晚了**。
 *
 * 延迟到主机已挂上再跑才是可复现的做法；顺带也不再用几百毫秒拖慢启动。 */
#define APP_TRACE_START_MS 3000U

/* RTT 上行缓冲默认只有 1024 B，模式是 NO_BLOCK_SKIP —— 满则**直接丢**。
 * 批量输出必须分块主动让路：每块留在缓冲容量内，块间延时。
 * ⚠️ 节奏必须按**每一行**计（含表头），否则表头会先灌满缓冲，
 *    后面几十行全被丢掉 —— 丢数据是**静默**的。 */
#define APP_TRACE_DRAIN_LINES 8U
#define APP_TRACE_DRAIN_MS 15U

/* ------------------------------------------------------------ 演示：心跳灯 ----
 * 用一个"纯逻辑"的参数演示四层的走法（docs/10 §A）：
 *     Operation/Inc/op_blink.h   ← 纯逻辑：给定时间，该亮还是该灭
 *     Test/test_op_blink.c       ← PC 单测，`fw.py test` 秒级跑完
 *     Task/Src/app_blink.c       ← 取时间 / 传时间 / 按结论驱动 Device
 *
 * ⚠️ `on_ms + off_ms` 必须 < 2^32（用无符号减法算周期，天然处理回绕）。 */
#define APP_BLINK_ON_MS 500U
#define APP_BLINK_OFF_MS 500U

/* ------------------------------------------------------- 演示：写 SPI Flash ----
 * 一次跨页写最多拆成几段 —— 决定组合根里那个段数组开多大。
 * W25Q128 页 = 256B；起始未对齐时首段不足一页，所以段数会多一个。
 * 按"一次最多写 1KB"给 8 段，够用又省栈（`op_flash_page_count()` 可以先问）。 */
#define APP_FLASH_MAX_SEGS 8

/* ------------------------------------------------------------ 测量节拍 ----
 * 自动重测周期。题面"各项 ≤2s 完成"：500ms 周期 + 全链 ~40ms ⇒ 任意时刻的
 * 屏上数据 ≤0.6s 旧 —— 判据的 1/3。切页/换 1T-3T 会立即重测一次（app_ui）。
 * ⚠️ 别改成 <100ms：DSP 一拍 40ms，周期太短会把节拍全部吃满（LVGL 饿死）。 */
#define APP_MEAS_PERIOD_MS 500U

/* 波形页重建曲线的列数。320 = 屏宽 1:1（每像素一列，无插值观感）。
 * op_wave/op_meas 的重建在 UI 刷新时跑：320×3 分量 sinf ≈ 1ms 级，无压力。 */
#define APP_UI_WAVE_POINTS 320 /* 不带 U：大量出现在 int 循环里，-Wsign-compare 会炸 */

/* 标定表在 W25Q128 的落点：扇区 0（4KB）。选它是为了离固件尾部测试扇区
 * （app_flash_rw_test 用倒数两个扇区）足够远；表 216B < 1 页 256B。 */
#define APP_CAL_FLASH_ADDR 0x00000000U

/* --------------------------------------------------------- 功能开关（闸门）----
 * 一个开关如果没有任何代码读它，就是"孤儿宏"（翻成 1 什么都不会发生）。
 * 所以每个开关都配一道编译期闸门 —— 见 docs/07 §功能开关要有闸门。 */

#define APP_ENABLE_IWDG 0
#if APP_ENABLE_IWDG
#error "APP_ENABLE_IWDG=1，但工程里没有 bsp_iwdg 实现：请先补模块再启用"
#endif

/* 置 1 = 上电自检末尾**故意触发 HardFault**，用来验证崩溃取证链路：
 *   HardFault_Handler → bsp_fault_report() → RTT 输出栈帧。
 * ⚠️ 触发后固件**会停机**（bsp_fault_report 是 noreturn，这是预期行为）
 *    ⇒ 默认必须是 0。验完**记得改回 0**，留着会让板子每次上电都死。 */
#define APP_SELFTEST_FAULT 0

#endif /* PROJECT_CONFIG_H */
