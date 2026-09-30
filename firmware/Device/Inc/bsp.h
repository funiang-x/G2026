#ifndef BSP_H
#define BSP_H
/* ============================================================================
 *  bsp.h —— 板级支持层（Device 层）总入口 · 能力总表
 *
 *  上层只需 #include "bsp.h"，就拿到全部板级能力，不必逐个记住文件名。
 *
 *  ============================ 这一层是干什么的 ============================
 *
 *  四层结构（见 docs/01）：
 *
 *      Task/        调度：只管"什么时候做"
 *        ↓  只能向下调
 *      Operation/   纯逻辑：吃数字吐数字（禁 HAL / 禁 FreeRTOS）
 *        ↓
 *      Device/      ← **本层**：硬件操作，唯一直接调 HAL 的层
 *        ↓
 *      Core/ + Drivers/   CubeMX 生成 + ST HAL
 *
 *  Device 的规矩：
 *    · 一个外设一个文件（bsp_spi.c / bsp_uart.c …），端到端负责它；
 *    · HAL 句柄一律 `static` 在 .c 内，**不要写进对外头文件**
 *      （头文件里出现 HAL 类型会把上层也拖进 HAL）；
 *    · 引脚 / 时钟等硬件事实取自 Core/Inc/board_config.h，本层不写魔法数字；
 *    · 中断回调（HAL_xxx_Callback）写在本层，**只投事件不跑业务**（docs/01 规矩 3）。
 *
 *  ============================ 加一个新外设 ============================
 *
 *  三步：
 *    1) 建 Device/Src/bsp_xxx.c 与 Device/Inc/bsp_xxx.h
 *    2) .c 里直接 #include "stm32f4xx_hal.h"（HAL 对 Device 层可见，不是越层）
 *    3) 本文件加一行 #include（下面的清单就是"板级能力总表"）
 *
 *  ⚠️ 新外设还要**去 CubeMX 里配一次** —— 否则该外设的 HAL 模块宏不会打开
 *     （Core/Inc/stm32f4xx_hal_conf.h），编译会报
 *         error: unknown type name 'ADC_HandleTypeDef'
 *     修法是去 CubeMX 加这个外设再生成，**不是**手改 hal_conf.h（那是生成物）。
 *     完整流程见 docs/10 §B。
 * ==========================================================================*/
#include "bsp_fault.h" /* HardFault 取证（CFSR/HFSR 解码，别删）*/
#include "bsp_gpio.h"  /* 通用 GPIO 抽象（引脚配置 / 读写 / 翻转）*/
#include "bsp_log.h"   /* 日志后端（RTT / UART 两条）；LOG_* 宏在 Common/Inc/log.h */
#include "bsp_rtc.h"   /* 板载 RTC（LSE 32.768kHz + VBAT 掉电保持）*/
#include "bsp_sd.h"    /* 板载 TF 卡座（SDIO 4-bit + PD3 卡检测）*/
#include "bsp_spi.h"   /* SPI1 + 板载 W25Q128（16MB SPI Flash）*/
#include "bsp_tick.h"  /* 时基与阻塞延时 */

#include "bsp_adc.h"   /* 采样链：TIM8 触发 ADC1 → DMA2（PA1，2.0MSPS）*/
#include "bsp_lcd.h"   /* 液晶屏：ST7796S · 8080 16 位并口走 FSMC */
#include "bsp_touch.h" /* 电容触摸 FT6336（I2C2）*/

#endif /* BSP_H */
