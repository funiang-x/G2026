/* ============================================================================
 *  app_tasks.c —— 任务创建（全静态）· 见 app_tasks.h
 *
 *  本文件**只做一件事**：把 Task_Engine / Task_Ui 建起来。任务体在
 *  app_engine.c / app_ui.c（各自与 Device/Operation 打交道的地方）。
 *
 *  ---- 为什么全部用 `xTaskCreateStatic` ----
 *  动态创建会从 FreeRTOS 堆里出"栈 + TCB"，两块任务栈就要 **10 KiB**
 *  （Task_Engine 1024 words = 4 KiB + Task_Ui 1536 words = 6 KiB，再加两个 TCB），
 *  而堆只有 **4096 B**（`configTOTAL_HEAP_SIZE`，真相源 FreeRTOSConfig.h；
 *  ⚠️ 2026-10-14 订正：旧注释写 15KB，那是更早一版的口径）。
 *  ⇒ 动态建**必然失败**，这不是"省一点"而是"根本放不下"。
 *  ⇒ 栈与 TCB 一律是**本文件的 static 数组**，堆只留给内核对象。
 *  代价是"栈深写在编译期"，换来的是**不会有运行期分配失败**。
 *
 *  ---- 优先级关系（硬约束，见 project_config.h）----
 *      Task_Ui(26) > Task_Engine(25) > defaultTask(24)
 *  两条硬约束：
 *   ① **engine 必须在 UI 之下**（2026-10-13 六轮定）：它的 DSP 一拍 40ms 纯计算，
 *      高于 UI 时每 200ms 就硬插一刀 —— 现场症状是"波形逐带刷 + 一卡一卡"。
 *      现在引擎用 UI 的睡眠缝跑，40ms 摊到 ~55ms 墙钟。
 *   ② **defaultTask 垫底**（2026-10-14）：它只做心跳/闪灯/RTC，全工程最不紧急。
 *      ⚠️ 这**不是**"把 defaultTask 降下来" —— 它的值在生成物里改不动
 *      （`const osThreadAttr_t defaultTask_attributes`，不在任何 USER CODE 区内；
 *      理由见 project_config.h 同名小节），而是把业务任务**抬上去**得到的等价结果。
 * ==========================================================================*/
#include "app_tasks.h"

#include "app_engine.h"
#include "app_ui.h"
#include "log.h"
#include "project_config.h"

#include "FreeRTOS.h"
#include "task.h"

/* ------------------------------------------------------------ 静态任务存储 */
static StaticTask_t s_engine_tcb;
static StackType_t  s_engine_stack[APP_TASK_ENGINE_STACK_WORDS];
static StaticTask_t s_ui_tcb;
static StackType_t  s_ui_stack[APP_TASK_UI_STACK_WORDS];

static TaskHandle_t s_h_engine = NULL;
static TaskHandle_t s_h_ui = NULL;

static int s_ok = 0;

/* ------------------------------------------------------------ 对外 ---- */
int app_tasks_init(void) {
    /* ⚠️ 优先级**不在这里写数字** —— 一律取自 project_config.h。
     * ⚠️ 栈深实参是 word 数，xTaskCreateStatic 的形参类型是
     *    configSTACK_DEPTH_TYPE —— 显式转一次，别靠隐式。 */
    s_h_engine = xTaskCreateStatic(app_engine_task, "Task_Engine",
                                   (configSTACK_DEPTH_TYPE)APP_TASK_ENGINE_STACK_WORDS,
                                   NULL, APP_TASK_ENGINE_PRIORITY, s_engine_stack,
                                   &s_engine_tcb);
    s_h_ui = xTaskCreateStatic(app_ui_task, "Task_Ui",
                               (configSTACK_DEPTH_TYPE)APP_TASK_UI_STACK_WORDS, NULL,
                               APP_TASK_UI_PRIORITY, s_ui_stack, &s_ui_tcb);

    if (s_h_engine == NULL || s_h_ui == NULL) {
        LOG_E("[TASK] 静态任务创建失败（栈数组/TCB 没配够？）—— 组合根将退回单循环");
        s_ok = 0;
        return -1;
    }

    s_ok = 1;
    LOG_I("[TASK] 已建：Task_Engine(prio=%u, %u words) + Task_Ui(prio=%u, %u words)"
          "（+ CubeMX 的 defaultTask prio=24）",
          (unsigned)APP_TASK_ENGINE_PRIORITY, (unsigned)APP_TASK_ENGINE_STACK_WORDS,
          (unsigned)APP_TASK_UI_PRIORITY, (unsigned)APP_TASK_UI_STACK_WORDS);
    return 0;
}

int app_tasks_ok(void) {
    return s_ok;
}

void app_tasks_report_stack(void) {
    struct {
        const char *name;
        TaskHandle_t h;
    } tab[2] = {
        {"Task_Engine", s_h_engine},
        {"Task_Ui", s_h_ui},
    };

    for (unsigned i = 0U; i < (sizeof(tab) / sizeof(tab[0])); i++) {
        if (tab[i].h != NULL) {
            /* 单位是 **word**（4 字节）—— 换算成字节一起打出来，免得看错量级。 */
            const unsigned words = (unsigned)uxTaskGetStackHighWaterMark(tab[i].h);
            LOG_I("[STACK] %s 栈高水位 = %u words（%u B 剩余）", tab[i].name, words, words * 4U);
        }
    }
}
