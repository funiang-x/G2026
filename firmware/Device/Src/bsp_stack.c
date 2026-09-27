/* bsp_stack 实现 —— __get_MSP 的薄封装。
 * 为什么值得单独一个文件：App 层的内存审计（app_mem.c）需要 MSP，
 * 但分层契约禁止 App 碰 core_cm4.h。隔离点必须落在 BSP，
 * 顺手把"为什么隔离"写在头文件里，免得将来有人图省事把 intrinsic 搬进 App。 */
#include "bsp_stack.h"

#include "stm32f4xx.h" /* __get_MSP（经 core_cm4.h 的 CMSIS intrinsic） */

uint32_t bsp_stack_msp_get(void) {
    return __get_MSP();
}
