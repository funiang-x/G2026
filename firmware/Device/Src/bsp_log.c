/* ============================================================================
 *  bsp_log.c —— 日志**后端**实现（RTT 或 UART），以及 Common 层 log_put() 的 MCU 侧实现
 *
 *  ★ 依赖注入的落地处：Common/Src/log.c 只做格式化，然后把结果交给 log_put()；
 *    log_put() 的**实现**在这里。PC 构建时由 Test/stubs/log_host.c 提供另一份。
 *    同一个符号名，链接时二选一 —— Operation 因此完全不认识硬件。
 *
 *  ★ 并发保护：本文件用的是静态行缓冲，天然不可重入。
 *    多任务并发打印不加锁会输出串行错乱（被截断/交错的半行）。
 *    这里用一把递归互斥锁把"加前缀 + 写出"整段包住 —— 递归是必须的：
 *    打印内部若再触发日志（例如断言）才不会自死锁。
 *
 *  ★ MSP 回调放在"用它的那个文件"里（下面的 HAL_UART_MspInit 就是例子）——
 *    这样删掉某个外设文件时，它的 MSP 不会变成孤儿代码。
 * ==========================================================================*/
#include "bsp_log.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

#include "board_config.h"
#include "rtt_channels.h"

#if defined(LOG_BACKEND_RTT)
#include "SEGGER_RTT.h"
#elif defined(LOG_BACKEND_UART)
#include "stm32f4xx_hal.h"
#else
#error "LOG_BACKEND 未定义：只能是 RTT 或 UART（见 CMakePresets.json 的 base.cacheVariables）"
#endif

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h" /* xTaskGetSchedulerState / taskENTER_CRITICAL / taskSCHEDULER_NOT_STARTED */

/* ============================================================================
 * 日志串口后端（LOG_BACKEND=UART 时启用）
 *
 * ★ Device 就是唯一直接使用 HAL 的层，所以这里**直接写 HAL**，不再需要
 *   "通用串口抽象"那一层。
 * ★ 只保留"日志出口"这一条路径：初始化与写出都在本文件内静态化，对外一个
 *   API 都不留 —— 多留的对外接口就是"宣称支持但没人用"的负债。
 *
 * ★★ 引脚与时钟由 **CubeMX 生成的 `Core/Src/usart.c`** 负责：它的
 *    `HAL_UART_MspInit` 里就是"开 USART1 时钟 + PA9/PA10 + AF7"。
 *
 *    本文件**刻意不再自己定义 `HAL_UART_MspInit`** —— 实测踩过：
 *    两边都定义会 `multiple definition of 'HAL_UART_MspInit'`，**链接失败**
 *    （`Core/Src/usart.c` 与 `Device/Src/bsp_log.c`）。
 *    ⚠️ 参考工程 `template` 里这段 MSP 能编，是因为那边 CubeMX 一个 USART
 *       都没配、`usart.c` 根本不存在。本工程配了 USART1，那条路走不通。
 *    ⚠️ 也别为了"避免冲突"去删 `Core/Src/usart.c` 里的 MSP —— 那是生成物
 *       （docs/01 规矩 4），下次 CubeMX 生成就回来了。**MSP 归生成物**。
 *
 * ⚠️ 由此产生一条依赖：**UART 后端要求 .ioc 里至少配了一个 USART**。
 *    没配 ⇒ 没有 MSP ⇒ `HAL_UART_Init` 拿不到时钟与引脚，表现是
 *    "编得过、但串口一声不响" —— 正是本工程最讨厌的静默失效。
 *    所以下面把它变成**编译期错误**（HAL_UART_MODULE_ENABLED 由 CubeMX 在
 *    配了任意 UART/USART 时写入 stm32f4xx_hal_conf.h）。
 * ==========================================================================*/
#if defined(LOG_BACKEND_UART)
#if !defined(HAL_UART_MODULE_ENABLED)
#error "LOG_BACKEND=UART 要求 CubeMX 里至少配置一个 USART（否则没有 HAL_UART_MspInit，串口不会工作）。见 docs/11 §3。"
#endif

static UART_HandleTypeDef s_log_uart;
static int s_log_uart_ready;

/* board_config.h 里的数字端口号 → HAL 外设指针。
 * ★ 这里**不**用基址算术（USART1 基址 + 0x400 × 端口号）：F4 上确实等间距，
 *   但算术**不过界检查** —— 端口号写错一个数字就得到一个野指针。 */
static USART_TypeDef *log_uart_instance(void) {
    switch ((unsigned)BOARD_LOG_UART_PORT) {
        case 1U: return USART1;
        case 2U: return USART2;
        case 3U: return USART3;
        case 4U: return UART4;
        case 5U: return UART5;
        case 6U: return USART6;
        default: return NULL;
    }
}

static int log_uart_init(void) {
    USART_TypeDef *inst = log_uart_instance();
    if (inst == NULL) {
        return -1;
    }
    s_log_uart.Instance = inst;
    s_log_uart.Init.BaudRate = BOARD_LOG_UART_BAUD;
    s_log_uart.Init.WordLength = UART_WORDLENGTH_8B;
    s_log_uart.Init.StopBits = UART_STOPBITS_1;
    s_log_uart.Init.Parity = UART_PARITY_NONE;
    s_log_uart.Init.Mode = UART_MODE_TX_RX;
    s_log_uart.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    s_log_uart.Init.OverSampling = UART_OVERSAMPLING_16;

    /* ★ 引脚与时钟不在这里配 —— HAL_UART_Init 内部会调 HAL_UART_MspInit，
     *   而那份实现由 CubeMX 生成在 Core/Src/usart.c（见上方说明）。 */
    if (HAL_UART_Init(&s_log_uart) != HAL_OK) {
        return -1;
    }
    s_log_uart_ready = 1;
    return 0;
}

static void log_uart_write(const char *buf, uint32_t len) {
    /* ★ HAL_UART_Transmit 的 Size 是 uint16_t：超限必须显式拒绝，
     *   强转 = 静默只发前 64KB。本工程日志单行 ≤160 B，永远碰不到；
     *   这是给将来接协议/透传时兜底。 */
    if (s_log_uart_ready == 0 || buf == NULL || len == 0U || len > 0xFFFFU) {
        return;
    }
    (void)HAL_UART_Transmit(&s_log_uart, (uint8_t *)(uintptr_t)buf, (uint16_t)len, 1000U);
}
#endif /* LOG_BACKEND_UART */

/* FreeRTOS 的两个故障钩子由 bsp_fault.c 提供；这里只声明 newlib 的重定向点。 */
int __io_putchar(int ch);

/* ============================================================================
 * 后端无关部分：级别过滤 / 前缀 / 换行 / 锁
 * ==========================================================================*/

/* 单行上限。与 Common/Src/log.c 的 LOG_LINE_MAX 独立 —— 那边是格式化缓冲，
 * 这边是"加了前缀与换行之后"的写出缓冲，两者不共用。 */
#define BSP_LOG_LINE_MAX 192U

static bsp_log_level_t s_level = BSP_LOG_INFO;
static SemaphoreHandle_t s_log_mutex;

/* ★★ 调度器启动前，绝对不碰任何 FreeRTOS 原语。
 *
 * 机理（已在硬件上实测定位到源码）——引用一律用**符号名**，不写行号：
 *   Middlewares/Third_Party/FreeRTOS 的 port.c 里的 uxCriticalNesting：
 *     · 它的**初值不是 0**（是个 0xaaaa… 的哨兵值，用来暴露"没初始化就用了"）
 *     · 只有 xPortStartScheduler() 会把它归零
 *     · portENTER_CRITICAL / portEXIT_CRITICAL 各自对它 ++ / --
 *     · portEXIT_CRITICAL 末尾按 "uxCriticalNesting == 0" 决定是否
 *       portENABLE_INTERRUPTS()
 *
 *   于是调度器启动前进入一次临界区：哨兵值 → +1 → −1，**永远回不到 0**，
 *   portENABLE_INTERRUPTS() 永不执行，BASEPRI 被永久钉在
 *   configMAX_SYSCALL_INTERRUPT_PRIORITY。
 *   后果：所有抢占优先级低于该阈值的中断全部失效，其中包括 HAL 时基定时器。
 *   现象链：uwTick 冻结 → HAL_GetTick() 恒为定值 → HAL_Delay() 死循环。
 *
 *   ⚠️ 上面为什么不写行号：升级 FreeRTOS 后行号会**静默过期**（注释不会报错，
 *      只有人踩坑时才发现），而符号名永远可以 grep。见 docs/05。
 *
 *   xSemaphoreCreateRecursiveMutex / xSemaphoreTakeRecursive 内部都会走
 *   xQueueGenericSend → taskENTER_CRITICAL()，所以"建锁"和"取锁"都算。
 *   调度器启动前是单线程，本来就不需要锁。 */
static int log_rtos_ready(void) {
    /* xTaskGetSchedulerState() 只读一个静态变量，自身不使用临界区，可安全调用 */
    return (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) ? 1 : 0;
}

/* 惰性建锁：首次在"调度器已运行"的任务上下文里创建。
 * xSemaphoreCreate 依赖 heap_4，在调度器前后都可用，但**不能在调度器前调用**（见上）。
 * 两个任务首次并发打印时可能同时建锁，用临界区保护赋值，败者回收，避免泄漏堆。 */
static void log_lock(void) {
    if (log_rtos_ready() == 0) {
        return;
    }
    if (s_log_mutex == NULL) {
        SemaphoreHandle_t m = xSemaphoreCreateRecursiveMutex();
        if (m != NULL) {
            taskENTER_CRITICAL(); /* 此处调度器已启动，临界区合法 */
            if (s_log_mutex == NULL) {
                s_log_mutex = m;
                m = NULL;
            }
            taskEXIT_CRITICAL();
            if (m != NULL) {
                vSemaphoreDelete(m);
            }
        }
    }
    if (s_log_mutex != NULL) {
        (void)xSemaphoreTakeRecursive(s_log_mutex, portMAX_DELAY);
    }
}

static void log_unlock(void) {
    /* 与 log_lock 用同一个判据：调度器状态单调（未启动→已启动），
     * 因此"取过锁"就一定能"还锁"。 */
    if (log_rtos_ready() == 0) {
        return;
    }
    if (s_log_mutex != NULL) {
        (void)xSemaphoreGiveRecursive(s_log_mutex);
    }
}

static const char *level_tag(bsp_log_level_t level) {
    switch (level) {
        case LOG_LVL_ERROR:
            return "E";
        case LOG_LVL_WARN:
            return "W";
        case LOG_LVL_INFO:
            return "I";
        default:
            return "D";
    }
}

/* 后端无关的写出：加前缀 + 补换行 + 交给具体后端。
 * ⚠️ 每个早退分支都必须与加锁配对，否则会漏还锁。 */
static void log_emit(bsp_log_level_t level, const char *buf, unsigned len, int use_lock) {
    char out[BSP_LOG_LINE_MAX];
    unsigned used = 0U;

    if (level > s_level) {
        return; /* 级别过滤在这里做 —— 阈值由 Task 层通过 bsp_log_set_level 注入 */
    }
    if (use_lock) {
        log_lock();
    }

    /* ★ 加锁范围：整段"加前缀 + 写出"。
     *   若只锁写出，两个任务的格式化结果会争用同一个 out 缓冲，仍然错乱。 */
    out[used++] = '[';
    const char *tag = level_tag(level);
    out[used++] = tag[0];
    out[used++] = ']';
    out[used++] = ' ';

    if (len > sizeof(out) - used - 2U) {
        len = sizeof(out) - used - 2U;
    }
    for (unsigned i = 0U; i < len; i++) {
        out[used++] = buf[i];
    }

    /* 补 \r\n：无论串口终端还是 RTT Viewer，缺了 \r 都会出现"阶梯状"错行 */
    out[used++] = '\r';
    out[used++] = '\n';

    bsp_log_write_raw(out, used);

    if (use_lock) {
        log_unlock();
    }
}

/* ============================================================================
 * 对外接口
 * ==========================================================================*/

void bsp_log_init(void) {
    /* ★ 这里**不**建锁：bsp_log_init() 在 main() 早期被调用，那时调度器还没启动，
     *   而 xSemaphoreCreateRecursiveMutex() 内部会进 FreeRTOS 临界区，
     *   会把 BASEPRI 永久钉住（详见 log_lock() 上方说明）。
     *   互斥锁改由 log_lock() 在调度器启动后惰性创建。 */
#if defined(LOG_BACKEND_RTT)
    /* RTT 控制块本身是静态的，SEGGER_RTT_Init 只是复位读写指针 */
    SEGGER_RTT_Init();
#else
    /* ★ 串口初始化从 main.c 搬到这里：日志出口自己负责把自己的后端拉起来，
     *   main() 不必知道用的是 RTT 还是串口。
     *   ⚠️ 依然**只做初始化、不建锁**（同上）。另：HAL_UART_Init 路径上没有
     *      HAL_Delay，所以调度器前调用是安全的。 */
    (void)log_uart_init();
#endif
}

void bsp_log_set_level(bsp_log_level_t level) {
    s_level = level;
}

bsp_log_level_t bsp_log_get_level(void) {
    return s_level;
}

void bsp_log_write_raw(const char *data, unsigned len) {
    if (data == NULL || len == 0U) {
        return;
    }
#if defined(LOG_BACKEND_RTT)
    (void)SEGGER_RTT_Write(RTT_CH_LOG, data, len);
#else
    log_uart_write(data, (uint32_t)len);
#endif
}

/* ★ Common 层 log_put() 的 **MCU 侧实现**（见 log.h 的文件头）。
 *   PC 侧由 Test/stubs/log_host.c 提供同名函数 —— 链接时二选一。 */
void log_put(log_level_t lvl, const char *buf, unsigned len) {
    log_emit((bsp_log_level_t)lvl, buf, len, 1);
}

/* ============================================================================
 * newlib 输出重定向：让 printf / puts / 第三方库的消息也能进日志
 *
 * ★ 为什么必须有它（实测确认的一个陷阱）：
 *   CubeMX 生成的 `Core/Src/syscalls.c` 里，`_write()` 是**逐字节**调
 *   `__io_putchar()` 的，而 `__io_putchar` 只被**弱声明**、工程里没人实现。
 *   ⇒ 谁在固件里用一次 `printf`，链接期就报
 *     `undefined reference to '__io_putchar'` —— 报错信息里甚至不出现
 *     "printf"，很难一眼看出因果。这里给它一份强定义，坑就填了。
 *
 * ⚠️ 仍然**推荐**用 `LOG_I/W/E/D`（带级别过滤、带前缀、带锁，多任务安全）。
 *    这个重定向是给**你改不了的代码**用的：第三方库的 printf、断言消息、
 *    newlib 内部输出。它走 `bsp_log_write_raw()` —— 不加前缀、不补换行、不加锁。
 * ⚠️ 逐字节写，比 LOG_* 慢；别用它做批量输出。
 * ==========================================================================*/
int __io_putchar(int ch) {
    char c = (char)ch;
    bsp_log_write_raw(&c, 1U);
    return ch;
}

void bsp_log_printf_nolock(bsp_log_level_t level, const char *fmt, ...) {
    char buf[BSP_LOG_LINE_MAX];
    va_list ap;

    if (fmt == NULL) {
        return;
    }
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);

    if (n < 0) {
        return;
    }
    unsigned len = (n >= (int)sizeof buf) ? (unsigned)(sizeof buf - 1U) : (unsigned)n;
    log_emit(level, buf, len, 0);
}
