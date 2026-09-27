/* ============================================================================
 *  app_tasks.c —— 任务创建（全静态）与 DSP → UI 数据交接对象 · 见 app_tasks.h
 *
 *  ---- 为什么全部用 `xTaskCreateStatic` ----
 *  动态创建会从 FreeRTOS 堆里出"栈 + TCB"，5 个任务就把 8KB 的堆吃光，
 *  而 LVGL 还要用同一个堆建互斥量（LV_USE_OS = LV_OS_FREERTOS）。
 *  ⇒ 任务的栈与 TCB 一律是**本文件的 static 数组**，堆只留给内核对象。
 *  代价是"栈深写在编译期"，换来的是**不会有运行期分配失败**。
 *
 *  ---- 这些任务现在做什么 ----
 *  这是**骨架**：优先级、栈深、阻塞点、数据通路都按配置文档 §9/§10 建好了，
 *  但**算法与命令协议还没接**（Operation/ 下只有 op_blink / op_flash）。
 *  ⇒ 所以三个测量侧任务都阻塞在自己的队列上，**不产生任何假数据**
 *    （工程规矩：不许伪造数据）。等算法进来时，只需把 TODO 处补上，
 *    任务划分与优先级**不用再动**。
 *
 *  ⚠️ `Task_Sys` 不在这里 —— 它是 CubeMX 强制保留的 `defaultTask`
 *    （优先级 1 / 256 words），任务体在 `Task/Src/app_rtos.c`。
 * ==========================================================================*/
#include "app_tasks.h"

#include "app_lvgl.h"
#include "board_config.h"
#include "log.h"

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

/* ------------------------------------------------------------ 静态任务存储 */
static StaticTask_t s_meas_tcb;
static StackType_t  s_meas_stack[APP_TASK_MEAS_STACK_WORDS];
static StaticTask_t s_dsp_tcb;
static StackType_t  s_dsp_stack[APP_TASK_DSP_STACK_WORDS];
static StaticTask_t s_link_tcb;
static StackType_t  s_link_stack[APP_TASK_LINK_STACK_WORDS];
static StaticTask_t s_ui_tcb;
static StackType_t  s_ui_stack[APP_TASK_UI_STACK_WORDS];

static TaskHandle_t s_h_meas = NULL;
static TaskHandle_t s_h_dsp = NULL;
static TaskHandle_t s_h_link = NULL;
static TaskHandle_t s_h_ui = NULL;

/* -------------------------------------------------------------- 队列存储 */
/* ⚠️ 存储数组用**元素类型**声明（不是 uint8_t[]）：那样对齐天然正确。
 *    FreeRTOS 对静态队列的存储有 `configASSERT(对齐)`，用 uint8_t[] 在
 *    某些编译器/优化档下会踩。 */
static app_cmd_t s_q_cmd_store[APP_Q_CMD_LEN];
static StaticQueue_t s_q_cmd_cb;
static QueueHandle_t s_q_cmd = NULL;

static dsp_frame_t s_q_raw_store[APP_Q_RAW_LEN];
static StaticQueue_t s_q_raw_cb;
static QueueHandle_t s_q_raw = NULL;

static dsp_result_t s_q_result_store[APP_Q_RESULT_LEN];
static StaticQueue_t s_q_result_cb;
static QueueHandle_t s_q_result = NULL;

static uint16_t s_q_dbg_store[APP_Q_DBG_LEN];
static StaticQueue_t s_q_dbg_cb;
static QueueHandle_t s_q_dbg = NULL;

/* 波形乒乓缓冲（文档 §10.2）：DSP 写 A 时 UI 读 B，结果里带有效缓冲号。
 * ⚠️ 1 KB（2 × 256 × 2B），放 SRAM1 普通 .bss。 */
uint16_t g_wave[2][APP_WAVE_POINTS];

/* ------------------------------------------------------------ 任务体 ---- */

/* Task_Meas：命令解析 → 采集 → 投原始数据 → 收结果 → 决策。
 * ⚠️ 它**不直接调 Device** —— 跨层只允许在组合根（app.c，规矩 1）。
 *    真接算法时，把"采一帧"做成 app.c 的显式入口，这里调那个入口。 */
static void task_meas(void *arg) {
    app_cmd_t cmd;

    (void)arg;
    LOG_I("[TASK] Task_Meas 启动（prio=%u, stack=%u words）", (unsigned)APP_TASK_MEAS_PRIORITY,
          (unsigned)APP_TASK_MEAS_STACK_WORDS);

    for (;;) {
        /* 阻塞在命令队列上 —— 不轮询、不吃 CPU（文档 §9.1 的"阻塞在"列）。 */
        if (xQueueReceive(s_q_cmd, &cmd, portMAX_DELAY) == pdTRUE) {
            LOG_I("[TASK] Task_Meas 收到命令 id=%u arg=%u", (unsigned)cmd.id,
                  (unsigned)cmd.arg);
            /* TODO(算法接入)：命令 → 门控采集（bsp_adc，经 app.c 的显式入口）
             *              → 填 dsp_frame_t → xQueueSend(s_q_raw)。
             * TODO：采集用信号量 sem_adc_done / sem_buf_free[2] 做门控，
             *       它们和采集逻辑一起加 —— 现在加只会是没人读的孤儿对象。 */
        }
    }
}

/* Task_Dsp：取原始数据 → 跑分析 → 释放缓冲 → 投递结果。 */
static void task_dsp(void *arg) {
    dsp_frame_t frame;

    (void)arg;
    LOG_I("[TASK] Task_Dsp 启动（prio=%u, stack=%u words）", (unsigned)APP_TASK_DSP_PRIORITY,
          (unsigned)APP_TASK_DSP_STACK_WORDS);

    for (;;) {
        if (xQueueReceive(s_q_raw, &frame, portMAX_DELAY) == pdTRUE) {
            LOG_I("[TASK] Task_Dsp 收到原始帧 buf=%u count=%u", (unsigned)frame.buf_idx,
                  (unsigned)frame.count);
            /* TODO(算法接入)：窗函数 → FFT → 找峰 → 写 g_wave[乒乓] →
             *              填 dsp_result_t（只放值 + 索引）→
             *              xQueueSend(s_q_result, &r, 0)。 */
        }
    }
}

/* Task_Link：调试输出（[OPT] 可删）。把要打印的东西**投递**过来，
 * 让真正干活的线程不因为串口/RTT 阻塞。 */
static void task_link(void *arg) {
    uint16_t code;

    (void)arg;
    LOG_I("[TASK] Task_Link 启动（prio=%u, stack=%u words）", (unsigned)APP_TASK_LINK_PRIORITY,
          (unsigned)APP_TASK_LINK_STACK_WORDS);

    for (;;) {
        if (xQueueReceive(s_q_dbg, &code, portMAX_DELAY) == pdTRUE) {
            LOG_D("[TASK] Task_Link 收到调试码 %u", (unsigned)code);
        }
    }
}

/* Task_Ui：`lv_timer_handler()` + UI 刷新。**优先级最低**（文档 §13-2）——
 * 即使一帧渲染 30ms，Task_Meas(5)/Task_Dsp(4) 也能随时抢占它，
 * 测量实时性完全不受 UI 影响。 */
static void task_ui(void *arg) {
    (void)arg;
    /* app_lvgl_run() 内部是 for(;;) 的 LVGL 主循环，不返回。 */
    app_lvgl_run();
}

/* ------------------------------------------------------------ 对外 ---- */
static TaskHandle_t create_static(TaskFunction_t fn, const char *name, UBaseType_t prio,
                                  StaticTask_t *tcb, StackType_t *stack, uint32_t words) {
    return xTaskCreateStatic(fn, name, (configSTACK_DEPTH_TYPE)words, NULL, prio, stack, tcb);
}

int app_tasks_init(void) {
    /* ---- 队列（静态创建）---- */
    s_q_cmd = xQueueCreateStatic(APP_Q_CMD_LEN, sizeof(app_cmd_t), (uint8_t *)s_q_cmd_store,
                                 &s_q_cmd_cb);
    s_q_raw = xQueueCreateStatic(APP_Q_RAW_LEN, sizeof(dsp_frame_t), (uint8_t *)s_q_raw_store,
                                 &s_q_raw_cb);
    s_q_result = xQueueCreateStatic(APP_Q_RESULT_LEN, sizeof(dsp_result_t),
                                    (uint8_t *)s_q_result_store, &s_q_result_cb);
    s_q_dbg = xQueueCreateStatic(APP_Q_DBG_LEN, sizeof(uint16_t), (uint8_t *)s_q_dbg_store,
                                 &s_q_dbg_cb);
    if (s_q_cmd == NULL || s_q_raw == NULL || s_q_result == NULL || s_q_dbg == NULL) {
        LOG_E("[TASK] 队列创建失败");
        return -1;
    }

    /* ---- 任务（静态创建）----
     * ⚠️ 优先级关系是**硬约束**（文档 §13-2）：Ui(2) < Link(3) < Dsp(4) < Meas(5)。
     *    改优先级请改 project_config.h，不要在这里写数字。 */
    s_h_meas = create_static(task_meas, "Task_Meas", APP_TASK_MEAS_PRIORITY, &s_meas_tcb,
                             s_meas_stack, APP_TASK_MEAS_STACK_WORDS);
    s_h_dsp = create_static(task_dsp, "Task_Dsp", APP_TASK_DSP_PRIORITY, &s_dsp_tcb, s_dsp_stack,
                            APP_TASK_DSP_STACK_WORDS);
    s_h_link = create_static(task_link, "Task_Link", APP_TASK_LINK_PRIORITY, &s_link_tcb,
                             s_link_stack, APP_TASK_LINK_STACK_WORDS);
    s_h_ui = create_static(task_ui, "Task_Ui", APP_TASK_UI_PRIORITY, &s_ui_tcb, s_ui_stack,
                           APP_TASK_UI_STACK_WORDS);

    if (s_h_meas == NULL || s_h_dsp == NULL || s_h_link == NULL || s_h_ui == NULL) {
        LOG_E("[TASK] 静态任务创建失败（栈数组/TCB 没配够？）");
        return -1;
    }

    LOG_I("[TASK] 4 个静态任务已创建（+ CubeMX 的 defaultTask = Task_Sys, prio=%u）",
          (unsigned)APP_TASK_SYS_PRIORITY);
    return 0;
}

void app_tasks_report_stack(void) {
    /* ⚠️ 高水位 = **剩余**栈的最小值（单位 word）。它随时间只降不升，
     *    所以"看到它贴近 0"就是栈快溢出的前兆 —— 这是提前发现
     *    `configCHECK_FOR_STACK_OVERFLOW` 报警的唯一手段。 */
    struct {
        const char *name;
        TaskHandle_t h;
    } tab[4] = {
        {"Task_Meas", s_h_meas},
        {"Task_Dsp", s_h_dsp},
        {"Task_Link", s_h_link},
        {"Task_Ui", s_h_ui},
    };

    for (unsigned i = 0U; i < (sizeof(tab) / sizeof(tab[0])); i++) {
        if (tab[i].h != NULL) {
            LOG_I("[STACK] %s 高水位 = %u words", tab[i].name,
                  (unsigned)uxTaskGetStackHighWaterMark(tab[i].h));
        }
    }
}
