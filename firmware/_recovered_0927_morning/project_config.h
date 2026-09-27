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

/* 任务栈高水位报告周期（Task_Sys）。栈溢出是最难查的故障，
 * 高水位只降不升 —— 定期打印就能在溢出**之前**看到趋势。 */
#define APP_STACK_REPORT_PERIOD_MS 5000U

/* ------------------------------------------------------------ LVGL 参数 ---- */

/* draw buffer 的行数：缓冲 = 屏宽 × 本值 × 2 字节。
 *   横屏 480 × 32 × 2 = **30,720 B = 30.0 KB**（在 SRAM1，见 Task/Src/app_lvgl.c）。
 * ★ 32 行是配置文档 §7.5 的定值：它同时是"每次 flush 搬多少像素"和"吃多少 RAM"
 *   的折中。缩小会让 flush 调用次数暴增（每次开窗有固定开销），
 *   放大则挤占 SRAM1（128KB 里已经排了 adc_buf 16K + g_samples 16K + 池/栈 48K）。
 * ⚠️ 这个缓冲**不能放 CCM**：本工程走 CPU 直写 FSMC 时能跑，
 *    但将来若改成 DMA 搬 FSMC，CCM 会**静默失效**（不报错、花屏/黑屏）。
 *    放 SRAM1 就没有这个隐患，代价只是访问慢一点点。 */
#define APP_LVGL_DRAW_BUF_LINES 32U

/* UI 主循环的节拍：`lv_timer_handler()` 之后睡多久。
 * ★ 配置文档 §5.5 定的是 5ms —— 比 LVGL 自己的重绘周期（LV_DEF_REFR_PERIOD=16ms）
 *   密得多，触摸响应手感才跟得上。改大会掉帧。 */
#define APP_LVGL_HANDLER_PERIOD_MS 5U

/* ------------------------------------------------------------ FreeRTOS 参数 ----
 * ⚠️ 任务**不要用 CubeMX 的图形界面建** —— CubeMX 里只勾 FreeRTOS 内核（docs/01 规矩 4）。
 *
 * ★ 2026-09-27：任务划分按配置文档 §9 从"单任务"改成 **5 个任务**。
 *    优先级铁律（文档 §13-2）：**`Task_Ui` 必须低于所有测量任务** ——
 *    这样即使 UI 一帧渲染 30ms，DSP 也能随时抢占，测量实时性不受 UI 影响。
 *
 *    优先级数值越大越优先；`configMAX_PRIORITIES` = 56。
 *    ⚠️ 栈深单位是 **word（4 字节）**，不是字节。2048 words = 8 KiB。
 *
 *    任务           优先级  栈(words)  职责
 *    Task_Meas        5      512      命令解析 → 采集 → 投原始数据 → 收结果 → 决策
 *    Task_Dsp         4      512      取原始数据 → 跑分析 → 释放缓冲 → 投结果
 *    Task_Link        3      384      调试输出（可删）
 *    Task_Ui          2     2048      lv_timer_handler() + UI 刷新（栈不能小！）
 *    Task_Sys         1      256      心跳 / 栈高水位 / 开机自检
 *
 *    ⚠️ `Task_Ui` 的 2048 words = 8KB 是**硬要求**（文档 §13-8）：
 *       LVGL 渲染要递归遍历对象树，2KB 会 HardFault。
 *    ⚠️ CubeMX 会强制保留一个 `defaultTask`（`.ioc` 里删掉它下次生成也会补回来，
 *       实测过）。本工程把它**当作 `Task_Sys`**：`.ioc` 的 FREERTOS.Tasks01 里
 *       优先级设 1、栈 256 words。它是 CMSIS 动态创建的（CubeMX 对"静态任务"的
 *       代码生成有 bug，会生成 `&NULL` 编译不过），其余 4 个任务都是
 *       `xTaskCreateStatic` 静态创建。**堆只被它一个占**，其余全静态。 */
#define APP_TASK_MEAS_STACK_WORDS 512U
#define APP_TASK_MEAS_PRIORITY 5U
#define APP_TASK_DSP_STACK_WORDS 512U
#define APP_TASK_DSP_PRIORITY 4U
#define APP_TASK_LINK_STACK_WORDS 384U
#define APP_TASK_LINK_PRIORITY 3U
#define APP_TASK_UI_STACK_WORDS 2048U /* ★ 8KB，不能小 */
#define APP_TASK_UI_PRIORITY 2U
#define APP_TASK_SYS_STACK_WORDS 256U
#define APP_TASK_SYS_PRIORITY 1U

/* --------------------------------------------------- DSP → UI 数据交接参数 ----
 * 依据配置文档 §10：单芯片后**没有 UART 组帧**了，改内存队列 + 乒乓缓冲。
 * ⚠️ `dsp_result_t` 必须 ≤ 128 B（文档 §13-10），由 `_Static_assert` 把关 ——
 *    **绝不许把频谱/波形数组塞进去**（队列是按值拷贝的，代价随大小线性涨）。 */
#define APP_Q_RESULT_LEN 4U   /* 结果队列深度：Ui 慢一拍也不会丢结果 */
#define APP_Q_RAW_LEN 2U      /* 原始帧队列深度（与 adc 双缓冲对齐）*/
#define APP_Q_DBG_LEN 4U      /* 调试队列（Task_Link）*/
#define APP_Q_CMD_LEN 4U      /* 命令队列（→ Task_Meas）*/
#define APP_WAVE_POINTS 256U  /* 波形点数：g_wave[2][256] = 1KB 乒乓 */

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
