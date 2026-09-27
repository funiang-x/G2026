#ifndef APP_TASKS_H
#define APP_TASKS_H
/* ============================================================================
 *  app_tasks.h —— 任务划分与 **DSP → UI 数据交接**（配置文档 §9 / §10）
 *
 *  ---- 任务表（优先级铁律见 project_config.h）----
 *      Task_Meas  5  ·  Task_Dsp  4  ·  Task_Link  3  ·  Task_Ui  2  ·  Task_Sys  1
 *
 *  ---- 交接机制（**没有 UART 组帧了**）----
 *  单芯片方案把"UART 921600 组帧发 ESP32"整条删掉，改成：
 *
 *      Task_Meas ──[q_raw：帧引用，几十字节]──▶ Task_Dsp
 *      Task_Dsp  ──[q_result：dsp_result_t 拷贝，≤128 B]──▶ Task_Ui
 *      Task_Dsp  ──[g_wave 双缓冲 ping-pong + 有效索引]────▶ Task_Ui
 *
 *  三条约束（文档 §10.2）：
 *    1. `dsp_result_t` **必须 ≤ 128 B**，由 `_Static_assert` 把关 ——
 *       **绝不许把频谱/波形数组塞进去**（队列是按值拷贝的，代价随大小线性涨）；
 *    2. `g_wave` 用**乒乓双缓冲**（`g_wave[2][256]`，1 KB）：DSP 写 A 时 UI 读 B，
 *       结果结构体里带"当前有效缓冲号"。比加锁简单，也不会撕裂；
 *    3. **UI 只读，不回写** —— UI 不做任何测量判定。
 *
 *  ⚠️ 结构体里**不放指针指向会被覆写的缓冲**，只放值 + 索引（文档 §10.3）。
 * ==========================================================================*/
#include <stdint.h>

#include "project_config.h"

/* ---------------------------------------------------------------- 报警位 ----
 * 依据配置文档 §15.4（来自上游总设计 §5.5）：
 *     DSP_FLAG_ALARM = 0x0ED7   全部报警位之和
 *     信息位          = 0x1128   ALIGNED | DEGRADED | OUTBAND | AUDIT_OK
 *     并集            = 0x1FFF   13 位全定义
 * ⚠️ **判异常一律写 `(flags & DSP_FLAG_ALARM) != 0`，禁止写 `flags == 0`**
 *    —— 信息位也在 flags 里，后者会把"有信息位"误判成异常。 */
#define DSP_FLAG_ALARM 0x0ED7U
#define DSP_FLAG_INFO 0x1128U

/* 结果结构体的硬上限（文档 §13-10）。 */
#define DSP_RESULT_MAX_BYTES 128U

/* DSP → UI 的一帧结果。**只放值**，不放指针。 */
typedef struct {
    uint32_t seq;      /* 帧序号：UI 用它判断"这是不是新结果" */
    float    freq_hz;  /* 主频（Hz）*/
    float    vpp_mv;   /* 峰峰值（mV）*/
    float    vrms_mv;  /* 有效值（mV）*/
    float    duty;     /* 占空比 0~1（测不出时填 0）*/
    uint16_t wave_idx; /* g_wave 的**有效缓冲号**（乒乓：0 或 1）*/
    uint16_t flags;    /* DSP_FLAG_* 位域 */
} dsp_result_t;

/* ★ 文档 §13-10 的机器强制：超了就**编译失败**，不会等到跑起来才发现队列拷贝变慢。 */
_Static_assert(sizeof(dsp_result_t) <= DSP_RESULT_MAX_BYTES, "dsp_result_t 超 128 B");

/* 原始帧的**引用**（不是数据本身）：4096 点的 uint16 是 8KB，
 * 走队列拷贝不现实 ⇒ 只传"是哪一块缓冲 + 多少点"，数据本体在 app_tasks.c 里。 */
typedef struct {
    uint8_t buf_idx; /* 采集缓冲号（乒乓：0 或 1）*/
    uint8_t _pad[3];
    uint32_t count; /* 有效点数 */
} dsp_frame_t;

/* 命令（UI / 调试口 → Task_Meas）。具体命令集等命令协议定下来再补。 */
typedef struct {
    uint16_t id;
    uint16_t arg;
} app_cmd_t;

/* 波形乒乓缓冲：`g_wave[有效号][0..APP_WAVE_POINTS-1]`。
 * ⚠️ **UI 只读**；写它的一律是 Task_Dsp。 */
extern uint16_t g_wave[2][APP_WAVE_POINTS];

/* 创建 4 个**静态**任务（Meas / Dsp / Link / Ui）与全部队列。
 * 返回 0 = 成功；-1 = 有对象没建起来（内存/配置问题，日志里会指出是哪个）。
 *
 * ⚠️ 必须在**调度器启动之前**调用（组合根 `app_init()` 里）——
 *    `xTaskCreateStatic` 在 vTaskStartScheduler 之前调用是允许的。
 * ⚠️ `Task_Sys` **不在本函数里建**：它是 CubeMX 强制保留的那个 `defaultTask`
 *    （`.ioc` 的 FREERTOS.Tasks01，优先级 1 / 256 words），任务体是
 *    `Task/Src/app_rtos.c` 的 `app_rtos_run()`。原因见 project_config.h。 */
int app_tasks_init(void);

/* 打印各任务的**栈高水位**（剩余栈的最小值，单位 word）。
 * 由 Task_Sys 周期性调用 —— 栈溢出是本工程最难查的一类故障，
 * 高水位是唯一能"提前看到"它的手段。 */
void app_tasks_report_stack(void);

#endif /* APP_TASKS_H */
