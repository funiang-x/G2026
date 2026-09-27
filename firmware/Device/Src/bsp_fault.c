/* bsp_fault 实现：崩溃现场取证。
 *
 * ★ 本文件**全程用 bsp_log_printf_nolock()**，一处都不用 LOG_E。
 *   理由：这些函数全部在**异常上下文**被调用（HardFault_Handler / panic / reset），
 *   而取互斥锁要进 FreeRTOS 临界区 —— 在异常上下文里那是非法操作，
 *   会把 BASEPRI 钉住或直接死锁。恰好在最需要现场的时候失效。
 *   代价：与正常日志并发时会交错输出。这个代价可以接受（系统马上就停了）。
 *
 * 注意：故障处理路径上"越简单越可靠"。这里用了 vsnprintf（可能依赖堆/栈），
 * 在栈已被踩坏或中断异常嵌套的极端情况下它自己也可能再崩。
 * 换来的是可读的现场，对绝大多数 HardFault（空指针、越界、除零）都够用。
 * 若连这行都打不出来，改用 J-Link 直接 halt + 看 PC 寄存器。 */
#include "bsp_fault.h"

#include <stddef.h> /* NULL —— 不要依赖其它头文件间接带入 */

#include "bsp_log.h"
#include "stm32f4xx.h"

/* FreeRTOS 的两个故障钩子（见文件末尾）。钩子由内核调用，原型不在 task.h 里
 * （V10.3.1 只在自己的 .c 里 extern），所以这里自己声明 —— 否则
 * -Wmissing-prototypes 会报。 */
#include "FreeRTOS.h"
#include "task.h"

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName);
void vApplicationMallocFailedHook(void);

/* 横幅直写。★ 长度交给编译器数（sizeof-1），**不要手写数字**：
 *   历史缺陷 —— HardFault 那条横幅实际是 43 字节，代码却传了 44，
 *   于是多写出 1 个 '\0'（仍在字面量存储之内、不越界，但输出夹带 NUL）。
 *   相邻那条 33 恰好写对了，属于"碰巧对"。手数长度的模式本身就是缺陷源。 */
#define FAULT_BANNER(lit) bsp_log_write_raw((lit), (unsigned)(sizeof(lit) - 1U))

/* 打印异常压栈帧。ARMv7-M 异常进入时硬件自动压栈顺序：
 *   sf[0..3] = R0,R1,R2,R3
 *   sf[4]    = R12
 *   sf[5]    = LR   （出错时的返回地址，能看出"从哪个函数调过来"）
 *   sf[6]    = PC   （出错的那条指令，用 addr2line 就能定位到源码行）
 *   sf[7]    = xPSR
 * 若启用了 FPU 且异常前用过浮点寄存器，帧尾还会多 16 个字 + 1 个状态字；
 * 定位 PC/LR 只需要前 8 个，不受影响。 */
static void dump_stack_frame(const uint32_t *sf) {
    if (sf == NULL) {
        bsp_log_printf_nolock(BSP_LOG_ERROR, "[FAULT] 栈帧指针为空，无法取寄存器");
        return;
    }
    bsp_log_printf_nolock(BSP_LOG_ERROR, "[FAULT] R0 =0x%08lx  R1 =0x%08lx  R2 =0x%08lx  R3 =0x%08lx",
                   (unsigned long)sf[0], (unsigned long)sf[1], (unsigned long)sf[2],
                   (unsigned long)sf[3]);
    bsp_log_printf_nolock(BSP_LOG_ERROR, "[FAULT] R12=0x%08lx  LR =0x%08lx  PC =0x%08lx  xPSR=0x%08lx",
                   (unsigned long)sf[4], (unsigned long)sf[5], (unsigned long)sf[6],
                   (unsigned long)sf[7]);
    bsp_log_printf_nolock(BSP_LOG_ERROR,
                   "[FAULT] 定位源码: arm-none-eabi-addr2line -f -e <你的.elf> 0x%08lx",
                   (unsigned long)(sf[6] & ~1UL));
}

/* 把 CFSR 的 3 个子域拆出来，才能一眼看出"是总线错误、还是非法指令" */
static void dump_fault_status(void) {
    uint32_t cfsr = SCB->CFSR;
    bsp_log_printf_nolock(BSP_LOG_ERROR, "[FAULT] CFSR=0x%08lx (MMFSR=0x%02lX BFSR=0x%02lX UFSR=0x%04lX)",
                   (unsigned long)cfsr, (unsigned long)(cfsr & 0xFFUL),
                   (unsigned long)((cfsr >> 8) & 0xFFUL), (unsigned long)((cfsr >> 16) & 0xFFFFUL));
    bsp_log_printf_nolock(BSP_LOG_ERROR, "[FAULT] HFSR=0x%08lx", (unsigned long)SCB->HFSR);

    /* MMFAR/BFAR 只在对应 VALID 位置位时才有意义，否则是残留旧值 */
    if ((cfsr & SCB_CFSR_MMARVALID_Msk) != 0U) {
        bsp_log_printf_nolock(BSP_LOG_ERROR, "[FAULT] 非法访问地址 MMFAR=0x%08lx",
                       (unsigned long)SCB->MMFAR);
    }
    if ((cfsr & SCB_CFSR_BFARVALID_Msk) != 0U) {
        bsp_log_printf_nolock(BSP_LOG_ERROR, "[FAULT] 非法访问地址 BFAR =0x%08lx",
                       (unsigned long)SCB->BFAR);
    }
}

/* ★ 从"疑似栈顶"定位**真正**的异常栈帧（2026-09-23 实测加的）。
 *
 * 为什么需要它：HardFault_Handler 是**普通 C 函数**，编译器会给它生成序言
 * （实测 `push {r3, lr}`，MSP 因此少了 8 字节）⇒ 直接把 MSP 当栈帧会**整体错位**
 * （症状：xPSR 里出现 0x0800xxxx 这种 Flash 地址、PC 是 0x44 这种荒谬值）。
 *
 * 为什么不用"固定补偿 +8"：那个偏移依赖编译器的序言，改优化等级就变，
 * 是个会悄悄失效的魔法数。
 *
 * ★ 改用异常栈帧的**固有特征**，三条同时满足才认：
 *   ① sf[6] (PC)   落在 Flash 代码区
 *   ② sf[7] (xPSR) 的 bit24 (Thumb) 必须为 1 —— M4 只跑 Thumb
 *   ③ sf[5] (LR)   落在代码区，或是 0xFFFFFFFx（EXC_RETURN）
 *   ⇒ 从当前栈顶往上扫几个字，第一个全中的就是它。 */
#define FAULT_CODE_BASE 0x08000000UL
#define FAULT_CODE_END 0x08100000UL /* VGT6 = 1MB Flash */
#define FAULT_SCAN_WORDS 16U

static const uint32_t *locate_stack_frame(const uint32_t *sp) {
    if (sp == NULL) {
        return NULL;
    }
    for (unsigned i = 0U; i < FAULT_SCAN_WORDS; i++) {
        const uint32_t *f = sp + i;
        const uint32_t pc = f[6];
        const uint32_t lr = f[5];
        const uint32_t xpsr = f[7];
        if (pc < FAULT_CODE_BASE || pc > FAULT_CODE_END) {
            continue;
        }
        if ((xpsr & (1UL << 24)) == 0UL) {
            continue;
        }
        if (!((lr >= FAULT_CODE_BASE && lr <= FAULT_CODE_END) ||
              ((lr & 0xFFFFFFF0UL) == 0xFFFFFFF0UL))) {
            continue;
        }
        if (i != 0U) {
            bsp_log_printf_nolock(BSP_LOG_ERROR,
                                  "[FAULT] 栈帧已自动校正：跳过 %lu 个字（函数序言）",
                                  (unsigned long)i);
        }
        return f;
    }
    return sp; /* 找不到就用原值 —— 至少能出日志，别因定位失败就什么都不报 */
}

void bsp_fault_report(uint32_t *stack_frame) {
    FAULT_BANNER("\r\n========== [FAULT] HardFault ==========\r\n");
    dump_stack_frame(locate_stack_frame(stack_frame));
    dump_fault_status();

    /* 常见原因自查表（按出现频率排序）：
     *   UFSR.DIVBYZERO=1        → 整数除零（浮点除零不触发，只置标志）
     *   UFSR.UNALIGNED=1        → 结构体指针强转导致的非对齐访问
     *   BFSR.PRECISERR=1 + BFAR → 访问了未使能时钟的外设寄存器 / 野指针
     *   CFSR 全 0 但 HFSR.FORCED=1 → 多半是跳到了非法地址或栈溢出 */
    bsp_log_printf_nolock(BSP_LOG_ERROR, "[FAULT] 固件已停在此处（死循环），复位后重试");

    for (;;) {
    }
}

void bsp_fault_panic(const char *reason) {
    FAULT_BANNER("\r\n========== [FAULT] ==========\r\n");
    bsp_log_printf_nolock(BSP_LOG_ERROR, "[FAULT] 异常: %s", (reason != NULL) ? reason : "unknown");
    dump_fault_status();
    for (;;) {
    }
}

void bsp_fault_reset(const char *reason) {
    /* ★ nolock 而非 LOG_E：调用方（LVGL 断言等）可能处在任何古怪上下文，
     *   取锁可能死锁；与文件末尾两个 FreeRTOS 钩子的取舍一致。 */
    FAULT_BANNER("\r\n========== [RESET] ==========\r\n");
    bsp_log_printf_nolock(BSP_LOG_ERROR, "[RESET] 原因: %s",
                          (reason != NULL) ? reason : "unknown");
    bsp_log_printf_nolock(BSP_LOG_ERROR, "[RESET] 正在复位（NVIC_SystemReset）");
    NVIC_SystemReset();
    for (;;) {
    }
}

/* ============================================================================
 * FreeRTOS 故障钩子
 *
 * ★ 为什么钩子放在 Device 层，而不是 Task 层：
 *     · 它们和 HardFault 取证是**同一件事** —— 系统级故障的现场上报，
 *       所以和 bsp_fault_report 放在一起，而不是和"任务怎么调度"放一起；
 *     · 它们必须**全程 nolock**（堆已耗尽 / 栈已溢出时取锁会再失败），
 *       而本文件已经通篇是 nolock 写法，是现成的正确地基；
 *     · 放在 Device 也避免了"Task 层第二个文件去 include Device 的头" ——
 *       那会稀释 docs/01 里"只有组合根碰 Device"这条可 grep 的规矩。
 *
 * ★ 为什么它们必须存在：
 *     Core/Inc/FreeRTOSConfig.h 的 USER CODE 段开了
 *         configCHECK_FOR_STACK_OVERFLOW = 2
 *         configUSE_MALLOC_FAILED_HOOK   = 1
 *     内核就会调用这两个符号。**不实现 = 链接期 undefined reference**。
 *     （cmsis_os2.c 里有一份 __WEAK 空实现，所以就算忘了也不会报错 ——
 *      那正是"静默失败"：开了开关却什么都不发生。下面两份是强定义，会覆盖它。）
 *
 * ⚠️ 两个钩子都**不许返回**：栈已溢出 / 堆已耗尽，返回等于继续踩坏的内存。
 *    打印完现场就停住，让 RTT 上留下可定位的证据。
 * ==========================================================================*/

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName) {
    /* ⚠️ xTask 是个**指针**，只在栈还没被彻底踩坏时才有意义；
     *    用 %p 打地址而不是解引用它 —— 解引用一个可能已损坏的 TCB 会二次崩溃。 */
    FAULT_BANNER("\r\n========== [FAULT] 任务栈溢出 ==========\r\n");
    bsp_log_printf_nolock(BSP_LOG_ERROR, "[FAULT] 溢出任务: %s",
                          (pcTaskName != NULL) ? pcTaskName : "(名字不可读)");
    bsp_log_printf_nolock(BSP_LOG_ERROR, "[FAULT] TCB 地址: 0x%08lx", (unsigned long)(uintptr_t)xTask);
    bsp_log_printf_nolock(BSP_LOG_ERROR,
                          "[FAULT] 对策: 调大该任务的栈深（单位是 word，不是字节）"
                          "—— 见 project_config.h 的 APP_TASK_STACK_WORDS");
    bsp_log_printf_nolock(BSP_LOG_ERROR, "[FAULT] 固件已停在此处，复位后重试");

    for (;;) {
    }
}

void vApplicationMallocFailedHook(void) {
    /* ⚠️ 这里**只能**用 nolock：堆已经耗尽，任何建锁 / 分配都会再次失败并
     *    递归回到本函数（见 bsp_log.h 对 bsp_log_printf_nolock 的说明）。 */
    FAULT_BANNER("\r\n========== [FAULT] 堆耗尽 ==========\r\n");
    bsp_log_printf_nolock(BSP_LOG_ERROR, "[FAULT] pvPortMalloc 返回了 NULL");
    bsp_log_printf_nolock(BSP_LOG_ERROR,
                          "[FAULT] 对策: 调大 configTOTAL_HEAP_SIZE"
                          "（CubeMX 的 FREERTOS.HEAP_SIZE，当前 15360 B）"
                          "或用 xTaskCreateStatic 静态建任务");
    bsp_log_printf_nolock(BSP_LOG_ERROR, "[FAULT] 固件已停在此处，复位后重试");

    for (;;) {
    }
}
