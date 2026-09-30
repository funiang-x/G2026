/* ============================================================================
 *  app_tasks.c —— 任务创建（全静态）· 见 app_tasks.h
 *
 *  本文件**只做一件事**：把 Task_Engine / Task_Ui 建起来。任务体在
 *  app_engine.c / app_ui.c（各自与 Device/Operation 打交道的地方）。
 *
 *  ---- 为什么全部用 `xTaskCreateStatic` ----
 *  动态创建会从 FreeRTOS 堆里出"栈 + TCB"，两块任务栈就要 10KB，
 *  而堆只有 15KB（configTOTAL_HEAP_SIZE），LVGL 还要用同一个堆建互斥量。
 *  ⇒ 栈与 TCB 一律是**本文件的 static 数组**，堆只留给内核对象。
 *  代价是"栈深写在编译期"，换来的是**不会有运行期分配失败**。
 *
 *  ---- 优先级关系（硬约束，见 project_config.h）----
 *      Task_Engine(26) > defaultTask(24) > Task_Ui(23)
 *  测量永远优先于界面；defaultTask 只做心跳/闪灯，夹在中间不碍事。
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
static TaskHandle_t create_static(TaskFunction_t fn, const char *name, UBaseType_t prio,
                                  StaticTask_t *tcb, StackType_t *stack, uint32_t words) {
    return xTaskCreateStatic(fn, name, (configSTACK_DEPTH_TYPE)words, NULL, prio, stack, tcb);
}

int app_tasks_init(void) {
    /* ⚠️ 优先级**不在这里写数字** —— 一律取自 project_config.h。 */
    s_h_engine = create_static(app_engine_task, "Task_Engine", APP_TASK_ENGINE_PRIORITY,
                               &s_engine_tcb, s_engine_stack, APP_TASK_ENGINE_STACK_WORDS);
    s_h_ui = create_static(app_ui_task, "Task_Ui", APP_TASK_UI_PRIORITY, &s_ui_tcb, s_ui_stack,
                           APP_TASK_UI_STACK_WORDS);

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
