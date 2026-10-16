#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H
/* ============================================================================
 *  board_config.h —— 板级事实的"唯一来源"（**手写文件**）
 *
 *  换一块板子，只需要改这一个文件：时钟从哪来、日志往哪出、AI 闭环怎么判定。
 *  各层都从这里取值，**不允许再散落硬编码**。
 *
 *  ⚠️ 本文件在 `Core/Inc/` 下，但**不是 CubeMX 生成物** ——
 *     CubeMX 只写它自己清单里的文件，不会碰这里。放心手写。
 *
 *  ---- 与 project_config.h 的分工 ----
 *      Core/Inc/board_config.h（本文件）  引脚 / 时钟 / 外设实例（"这块板子上有什么"）
 *      project_config.h（工程根）          周期 / 阈值 / 功能开关（"产品要怎么做"）
 *      两者都**不许**在 .c 里散落魔法数字。
 *
 *  ---- 加一个外设时（三步）----
 *      1) 去 CubeMX 里配好该外设（引脚 / 时钟 / DMA），保存 .ioc 并生成
 *      2) 建 Device/Src/bsp_xxx.c 与 Device/Inc/bsp_xxx.h，并在 bsp.h 里加一行 include
 *      3) 在本文件新建一个小节，把**值**抄进来（只抄值，不抄代码）
 *      ⚠️ 完整流程与常见坑见 docs/10 §B
 *
 *  ---- 小节怎么写（照抄这个形状）----
 *      · 一个外设一节，节标题用分隔线拉满 76 列
 *      · 引脚写**端口号 + 引脚号两个宏**（`PORT` 是 0=GPIOA 的枚举值），
 *        不要写 "PA9" 这种字符串 —— 驱动里要的是数字
 *      · 时钟/分频这类值用**物理量**表述（Hz / 级数），推导值由它算出来
 *      · 会"改了不生效"的开关，加一道 `#if` + `#error` 闸门（见文件末尾的说明）
 *      · ⚠️ 引脚/时钟改动后**必须重新上板验证** —— AI 无法替你判断接线
 * ==========================================================================*/

#include <stdint.h>

/* --------------------------------------------------------------- 芯片身份 ----
 * ★ 这一节**不只是给 C 用的**：顶层 CMakeLists.txt 会按名字读这几个宏，
 *   写进 `build/<预设>/board.env`，`Tools/fw.py` 再从那里取器件名 / 内存容量。
 *   ⇒ "这块芯片是什么"全工程只有这一处；换芯片改这里，烧录脚本和体积门禁
 *     跟着变，不会漏改（docs/11 §2）。
 *
 * ⚠️ 改这里只是改"脚本怎么称呼它"，**不会改变链接脚本**。换芯片还必须同步
 *    `STM32F407XX_FLASH.ld` 的 MEMORY 段、`startup_*.s`、`.ioc` 与
 *    `cmake/stm32cubemx/CMakeLists.txt`（生成物）—— 完整清单见 docs/10 §D。 */
#define BOARD_MCU_NAME "STM32F407VGTx"   /* ST / CubeMX 的完整型号（带尾缀 x） */
#define BOARD_JLINK_DEVICE "STM32F407VG" /* J-Link 的器件名（无尾缀） */

/* 内存容量。★ 与 STM32F407XX_FLASH.ld 的 MEMORY 段**必须一致** ——
 * fw.py sizecheck 拿它当分母算占用率，写错了门禁就是假的。
 * F407VG 实际是 SRAM1(112K) + SRAM2(16K) + CCM(64K)；前两者地址连续，
 * 链接脚本合成一个 128K 的 RAM 段，CCM 单列。 */
#define BOARD_FLASH_KB 1024U /* FLASH 段 */
#define BOARD_RAM_KB 128U    /* RAM 段（SRAM1+SRAM2） */
#define BOARD_CCM_KB 64U     /* CCMRAM 段 */

/* ---------------------------------------------------------------- 时钟源 ----
 * 本板：**8MHz** 外部晶振（HSE）→ PLL → SYSCLK = **144MHz**。
 *
 *  PLL 参数在 Core/Src/main.c 的 SystemClock_Config()（CubeMX 生成）：
 *      M=4 / N=144 / P=2 / Q=6
 *      VCOin  = 8/4    = 2.000MHz   （RM0090 要求 1~2MHz）
 *      VCOout = 2×144  = 288MHz     （要求 100~432MHz）
 *      SYSCLK = 288/2  = 144MHz
 *      USB    = 288/6  = 48.000MHz  （精确整除，USB_OTG_FS 与 SDIO 都需要）
 *      Flash  等待周期 = 5（120 < HCLK ≤ 150MHz 档）
 *
 *  ★★ 为什么**必须是 144MHz 而不是 168MHz**（2026-09-26 改，别改回去）：
 *      总设计 §2.1 要求 ADC 以 **2.0 MSPS** 采样（每 500ns 出一个点），
 *      且采样窗口取 3 个 ADCCLK。12 位转换最少 15 个 ADCCLK
 *      （3 采样 + 12 转换）⇒ 转换耗时 = 15/ADCCLK ≤ 416.67ns
 *      ⇒ **ADCCLK 必须 ≥ 30MHz**；而 ADCCLK 硬上限是 36MHz。
 *      ADCCLK = PCLK2/2，要落到 36MHz 就得 PCLK2 = 72MHz
 *      ⇒ SYSCLK = 144MHz（APB2 ÷2）。
 *      168MHz 下 PCLK2 = 84MHz：÷2 = 42MHz 超上限、÷4 = 21MHz 又太慢，
 *      **无解**。所以 144MHz 是这套采样参数的唯一解，不是偏好。
 *
 *  总线频率（由上面推出，改 PLL 必须同步重算）：
 *      AHB  = 144MHz   · APB1 = 36MHz (÷4) · APB2 = 72MHz (÷2)
 *      APB1 定时器时钟 = 72MHz · APB2 定时器时钟 = 144MHz（分频≠1 时翻倍）
 *      ADCCLK = PCLK2/2 = 36MHz
 *      ⇒ TIM8 时钟 144MHz，PSC=0 / ARR=71 ⇒ 更新事件 2.000MHz（= fs）
 *
 *  ⚠️ 换晶振时，本文件与 CubeMX 的 RCC **必须同时改**，且 PLL 参数要重算。
 *     只改一处 ⇒ `python Tools/fw.py lint` 会报确定性矛盾（那正是它存在的理由）。 */
#define BOARD_HSE_VALUE 8000000U
#define BOARD_TARGET_SYSCLK_HZ 144000000U

/* 派生频率：本文件与 .ioc 的 RCC.*Freq_Value 对账用（lint 只查 SYSCLK 与 HSE，
 * 这几个留在这里是为了让"改 PLL 时要一起动哪些数"看得见）。 */
#define BOARD_APB1_HZ 36000000U  /* PCLK1 */
#define BOARD_APB2_HZ 72000000U  /* PCLK2 —— ADCCLK 的来源 */
#define BOARD_ADCCLK_HZ 36000000U /* = PCLK2 / 2（ADC 硬上限） */

/* HSE 频率必须与 stm32f4xx_hal_conf.h 的 HSE_VALUE 一致。
 * ★ 不写 `#error` 断言的原因：HSE_VALUE 在 stm32f4xx_hal_conf.h 里，
 *   而那个头只能被 HAL 消费者 include（Common / Operation 拿不到）。
 *   所以这条一致性由 `fw.py lint` 的 CubeMX 对账（.ioc 的 RCC.HSE_VALUE）兜底。 */

/* ---------------------------------------------------------------- 日志出口 ----
 * 后端**只有 RTT 一条**（走 J-Link 调试探针，不占串口、不打断实时性）。
 *
 * ⚠️ **UART 后端已于 2026-10-06 移除**（`.ioc` 里不再配任何 USART ⇒ 板子没有
 *    串口出口）。原来这里的 `BOARD_LOG_UART_PORT` / `BAUD` / `TX_*` / `RX_*` / `AF`
 *    七个宏与 `--preset Debug-UART` 预设一并删掉；`bsp_log.c` 的串口后端代码也整段移除。
 *    要恢复：`.ioc` 勾回一个 USART 并重新生成 → 加回这组宏 → 恢复
 *    `bsp_log.c` 的 UART 分支（注意**不要**自己定义 `HAL_UART_MspInit`，
 *    与生成的 `Core/Src/usart.c` 会 multiple definition）。 */

/* ------------------------------------------------------------ 外设（已配）----
 * 以下外设由 CubeMX 配置并生成（见 .ioc）。引脚宏列在这里是为了
 * 让"接线事实"有一个可查的地方 —— 驱动实现在 Device/ 下的 bsp_*.c。
 *
 * ✅ 驱动**已就位**（2026-09-23）：`bsp_spi.c` / `bsp_rtc.c`
 *    都在 `Device/Src/` 下，且已挂进 `bsp.h` 能力总表。
 *    （本行原先写"还没有 bsp_spi.c / bsp_sd.c"，那是**过时描述**，已修正。）
 *    ⚠️ `bsp_sd.c` **2026-10-06 起已从构建下线**（SDIO 从 `.ioc` 移除）——
 *       源码留在 `Device/Src/`，但不进 `device` 目标、也不再挂进 `bsp.h`。
 *
 * ⚠️ 但**尚未上板验证** —— 只过了"编译 + 符号入库"（`arm-none-eabi-nm` 查得到符号）。
 *    "能编过"证明不了"能跑通"：Flash 读不读得出 JEDEC ID、卡识不识别得了，
 *    必须插上探针实测。**符号入库 ≠ 能跑。**
 *
 * ⚠️ 按键（`BOARD_KEY_*`）目前**只有登记，没有应用** —— 对比 `BOARD_LED_*`
 *    被 `app.c` 用着，按键没有任何 `app_key` / `op_key` 在读它。 */

/* SPI1 —— **板载 SPI Flash（W25Q128，16MB）专用**。
 * 证据：官方例程 014 的 spi_flash.h（`BSP_SPI = SPI1`、`GPIO_AF_SPI1`、
 * `NSS = GPIO_Pin_4`）。**PA4 是软件 CS**，由驱动自己拉，
 * 不是 SPI1 的硬件 NSS（所以 .ioc 里 PA4 保持 GPIO_Output）。
 *
 * ⚠️ SPI 模式：官方例程用 CPOL=High / CPHA=2Edge（模式 3）；
 *    本工程 .ioc 里 SPI1 是 CubeMX 默认（模式 0）。
 *    W25Q128 两种模式都支持，所以能用；要严格对齐例程就改 .ioc。
 *
 * ✅ **已上板验证**（2026-09-23）：读 W25Q128 的 JEDEC ID 得 `0xEF18`
 *    （Winbond + 128Mbit），与期望值**完全一致**。这一个数字同时证明了
 *    引脚接法、AF5 复用、SPI 时钟、**PA4 软件 CS 时序**、命令层全部正确。
 * ⚠️ **读写擦未验证** —— 测试代码已写（`app_flash_rw_test()`，会擦末扇区
 *    并做跨页写读回），但那次烧录因探针 USB 掉线没成功。 */
#define BOARD_SPI1_SCK_PORT 0 /* 0 = GPIOA */
#define BOARD_SPI1_SCK_PIN 5
#define BOARD_SPI1_MISO_PORT 0
#define BOARD_SPI1_MISO_PIN 6
#define BOARD_SPI1_MOSI_PORT 0
#define BOARD_SPI1_MOSI_PIN 7
#define BOARD_SPI1_AF 5           /* AF5 = SPI1/2 */
#define BOARD_SPI_FLASH_CS_PORT 0 /* PA4 · **软件 CS** */
#define BOARD_SPI_FLASH_CS_PIN 4

/* USART1 —— 与"日志出口"共用同一组引脚（见上）。板上**没有 CH340**，
 * 这组脚只引到 **H1 十脚调试座** ⇒ 走 UART 后端要自己接线。
 * ⚠️ 若你把日志后端改成 UART，它就**被日志占用了** ——
 *    别再把 USART1 当业务串口用，否则两个消费者抢同一个外设。 */
#define BOARD_USART1_AF 7

/* --------------------------------------------- 排针上的自由 IO（本模板未配）----
 * 本工程是**模板**：只保留板上真有器件的那些外设。下面这些脚在排针上是通的，
 * 但板上没有对应器件 ⇒ `.ioc` 里**没配**，这里也**不留 `BOARD_*` 宏**。
 *
 * 为什么不留宏：没有代码读的宏是**孤儿宏** —— 定义存在、看着像个开关，
 * 翻成 1 却什么都不会发生（见本文件末尾那段说明）。信息写注释里就够。
 *
 * 派生工程要用时，去 CubeMX 里配好，再回来按 docs/10 §B 加宏：
 *
 *   SPI2    全双工主机   PB10 = P1-23(SCK) · PC2 = P2-15(MISO) · PC3 = P2-17(MOSI)   AF5
 *   USART2  异步串口     PA2  = P1-1 (TX)  · PA3 = P1-6 (RX)                         AF7
 *
 * ⚠️ 这是**排针位置**，不是板载器件 —— 别以为板上有第二个 SPI / 第二个串口。
 * ⚠️ SPI2 是外接屏的常用选择（PB10–PB14 连续 5 脚，接线最短）。
 *    2026-09-26 按"模板只留板载器件"从 `.ioc` 移除；派生工程驱动 ILI9341
 *    那类屏时，先在 CubeMX 里把 SPI2 配回 Full-Duplex Master
 *    （SPI1 的 PA5/6/7 被板载 W25Q128 占了，用不了）。
 * --------------------------------------------------------------------------*/

/* USB_OTG_FS —— **2026-10-02 S5 已从固件里拔除**（省 ~15KB FLASH；在"512KB 片子只
 * 剩 ~13KB"的当下最值钱的一刀）。依据：全工程只有 `MX_USB_OTG_FS_PCD_Init()` 一处
 * 引用，没有任何 USB 功能代码。
 *
 * ⚠️ **但 Type-C 座与 PA11/PA12 仍然有用**：按住 SW3/BOOT0 再复位进 ROM
 * bootloader 就能 DFU 烧录（见 docs/11 §4）—— 那是片内固化的，不依赖本工程代码。
 * ⚠️ 所以 `BOARD_USB_OTG_FS_*` 这四个宏**已删除**（留着就是"孤儿宏"：翻成什么都不会
 * 发生）。要恢复 USB 功能：`.ioc` 重新勾上 USB_OTG_FS（Device Only）+ 重新生成，
 * 把 `usb_otg.c/h` 与 `HAL_PCD_MODULE_ENABLED` 一起放回来。 */

/* SDIO —— **板载 TF 卡座 CARD1**，4-bit 模式（不是 SPI）。
 *
 * ⚠️⚠️ **2026-10-06：SDIO 已从 `.ioc` 移除，本节的宏与下面的接线描述
 *    目前是"历史登记"，与现行 CubeMX 工程不符** —— 板子不再有 TF 卡座。
 *    连带处置：`bsp_sd.c` 已从 `device` 目标下线、`bsp.h` 不再 include 它。
 *    要用回来：`.ioc` 勾回 SDIO（PC8~PC12 + PD2，AF12）+ PD3 卡检测 →
 *    重新生成 → 恢复 `CMakeLists.txt` 的 `Device/Src/bsp_sd.c` 与 `bsp.h` 的 include。
 *    下面这段描述**保留原样**（它是 2026-09-23 当时的实测记录，不重写）。
 *
 * ✅ `NVIC.SDIO_IRQn` 已使能（抢占优先级 5）。
 * ✅ `PD3`（卡检测）已配成 GPIO_Input + 上拉。
 * ✅ **卡检测已上板验证**（2026-09-23）：未插卡时自检正确报出"TF 卡未插入"。
 * ⚠️ **卡的读写未验证** —— 手上没卡，插卡后跑 `bsp_sd_init()` 才知道。
 *
 * ⚠️ **「启动绑架」问题已修复**（2026-09-23 上板实测后）：
 *    `MX_SDIO_SD_Init()` 里的 `HAL_SD_Init()` 在**没插卡时必然失败**，
 *    而它原来调 `Error_Handler()` → 停机路径 → **整块板子不启动**。
 *    实测调用栈：Reset_Handler → main → MX_SDIO_SD_Init → Error_Handler → 停机。
 *    修法：`Error_Handler` 改调 `app_init_failed()`（打日志后返回，不停机）。
 *    ⇒ 现在没插卡也能正常启动，只是多一行 `[E]` 日志。
 *
 * ⚠️ 试过但**无效**的修法（别再试一遍）：
 *    把 SDIO 从 `.ioc` 的 `ProjectManager.functionlistsort` 里删掉，
 *    或把该条目末位改成 `HAL-false` —— 两者都试过，`MX_SDIO_SD_Init()`
 *    仍留在 `main.c`。CubeMX 那个「Initialize all configured peripherals」
 *    区块会把**所有已配外设**都列上，压根不看 `functionlistsort`。
 *
 * ⚠️ CubeMX 生成的 `MX_SDIO_SD_Init()` 里 `BusWide` 是 **1-bit** ——
 *    这是**对的**：SD 卡必须先 1-bit 初始化、识别后再由
 *    `HAL_SD_WideBusOperation_Config()` 切到 4-bit。别以为配错了。 */
#define BOARD_SDIO_CLK_PORT 2 /* 2 = GPIOC */
#define BOARD_SDIO_CLK_PIN 12
#define BOARD_SDIO_CMD_PORT 3 /* 3 = GPIOD */
#define BOARD_SDIO_CMD_PIN 2
#define BOARD_SDIO_D0_PORT 2
#define BOARD_SDIO_D0_PIN 8
#define BOARD_SDIO_D1_PORT 2
#define BOARD_SDIO_D1_PIN 9
#define BOARD_SDIO_D2_PORT 2
#define BOARD_SDIO_D2_PIN 10
#define BOARD_SDIO_D3_PORT 2
#define BOARD_SDIO_D3_PIN 11
#define BOARD_SDIO_AF 12 /* AF12 = SDIO */
/* PD3 = 卡检测（`.ioc` 里配成 GPIO_Input + 上拉，Label = SD_CARD_DET，
 * 所以 main.h 里有 `SD_CARD_DET_Pin` / `SD_CARD_DET_GPIO_Port`）。
 * 板上经 R20（47k）上拉到 3V3 ⇒ **插卡 = 低电平**。 */
#define BOARD_SDIO_DET_PORT 3 /* 3 = GPIOD */
#define BOARD_SDIO_DET_PIN 3

/* ---------------------------------------------------------------------- RTC ----
 * 板上有 32.768 kHz 晶振（LSE，PC14/PC15）+ VBAT 电路，`.ioc` 里已激活 RTC。
 * **时钟源 = LSE**（生成代码 `rtc.c` 的 `HAL_RTC_MspInit` 里
 * `RTCClockSelection = RCC_RTCCLKSOURCE_LSE`）—— 比内部 LSI 精度高得多。
 *
 * ⚠️ 配 RTC 的三个坑（2026-09-22 实测，缺一个就**静默不激活**：
 *    不报错、不生成 `rtc.c`）：
 *    ① 它的模式是**虚拟引脚** `VP_RTC_VS_RTC_Activate`，不是 `RTC.VirtualMode`；
 *    ② 那个虚拟引脚的 `Signal` 值**要带 IP 前缀** —— `RTC_VS_RTC_Activate`，
 *       不是 XML 里字面写的 `VS_RTC_Activate`；
 *    ③ 时钟源要写 `RCC.RTCClockSelectionARG`（带 `ARG` 后缀的才是代码生成用的，
 *       只写 `RTCClockSelection` 会生成 LSI）。
 *
 * ✅ **已上板验证**（2026-09-23）：设时间成功 + 5 秒内走了 5 秒。
 *
 * ⚠️ **第四个坑（上板才暴露，花了两轮才修对）**：RTC 要真正走 LSE，
 *    **必须走 GUI，改 `.ioc` 无效**。
 *
 *    现象链：`HAL_RTC_SetTime` 失败 ⇒ 读 `RCC_BDCR` 见 `LSEON=0` 但 `RTCSEL=01`
 *    ⇒ 查 `SystemClock_Config` 发现**压根没有 `RCC_OSCILLATORTYPE_LSE`**。
 *
 *    ① **`RCC.LSEUsed=1` 手写进 `.ioc` 有效** —— 它让 LSE 振荡器被使能。
 *    ② **但 RTC 时钟源不行**：手写 `RCC.RTCClockSelectionARG=RCC_RTCCLKSOURCE_LSE`
 *       **CubeMX 不接受**（不报错、不删、静默忽略，界面仍显示 LSI）。
 *       ⇒ **必须在 Clock Configuration 页把 `RTC Clock Mux` 点成 LSE**，
 *         再 Generate Code。（旧文档 `docs/guides/cubemx_clock_config.svg`
 *         已删除，控件位置以 CubeMX 界面为准。）
 *    ③ 改对之后 CubeMX 会自动去掉 LSI（生成代码里 `RCC_OSCILLATORTYPE_LSI` 消失）
 *       —— 这是正确的收敛，别以为它漏了。
 *
 *    **判据（三项都要过）**：读 `RCC_BDCR`（0x40023870）
 *      `LSEON=1` · `LSERDY=1` · `RTCSEL=01`
 *    修好后实测 `0x00008103` ✓（修之前是 `0x00008100`）。
 *    ⚠️ **只看"走时正常"不够** —— LSI（±5%）也能走时，5 秒内看不出误差。
 *       实测旁证：RTC 用 LSI 跑了 27 分钟，时间漂了 10 分钟。
 *
 *    ⚠️ **CubeMX 不报错、`fw.py lint` 也不报** —— 它只对账引脚，不对账时钟源。
 *    ⚠️ **`.ioc` 不是 API，是 CubeMX 的内部状态快照**：有些字段只从 GUI 写。
 *       改完必须回读**生成代码**或**界面状态**，不能假定"写了就等于生效"。 */

/* SWD 调试口 —— PA13(SWDIO) / PA14(SWCLK)。**不要**复用为 GPIO，
 * 否则烧录/调试会失联（且症状是"探针连不上"，很容易误判成探针坏了）。 */

/* ------------------------------------------------------------------ 指示灯 ----
 * **PB2 = 板上 LED2**，两条独立证据：① 原理图标 `PB2 LED&BOOT1`；
 * ② 官方例程 002 用 `GPIO_SetBits(GPIOB, GPIO_Pin_2)` 点灯 ⇒ **高电平点亮**。
 * CubeMX 里配成 GPIO_Output（推挽、无上下拉、低速），**初始电平 RESET（低）**，
 * 即上电是灭的。
 *
 * ⚠️ 初始电平**必须**是 RESET：既然"高电平点亮"，初始设 SET 就等于**上电就亮**，
 *    与"初始应为灭"自相矛盾。
 *    （2026-09-23 修正：本行原先误写成"初始电平 SET（高）"，与 `.ioc` 里的
 *      `PB2.PinState=GPIO_PIN_RESET` 不符。**代码一直是对的，错的是这条注释** ——
 *      这类"文档与实现不符"比代码 bug 更危险，因为它会误导后续判断。）
 *
 * ⚠️ PB2 同时是 **BOOT1**，板上带 10k 下拉（保证默认从 Flash 启动）。
 *    `BOOT0 = 0` 时 PB2 不参与启动模式判定，所以当 LED 用没有副作用。
 * ⚠️ 引脚事实已交叉核对，但**点灯本身未上板实测**。 */
#define BOARD_LED_PORT 1 /* 1 = GPIOB */
#define BOARD_LED_PIN 2

/* -------------------------------------------------- ⚠️ 用户按键（PA0/SW2）----
 * 板上 **SW2 物理接在 PA0**（按下 = 高电平，经 10k 下拉；2026-09-23 上板验证过）。
 *
 * 2026-09-28 变更：**模拟输入挪到 PA1（ADC1_IN1，用户在 CubeMX 里配置并拍板）**
 * —— 原因：① PA0 上还挂着 SW2 的按键电路（10k 下拉 + 触点），对模拟前端是
 * 一个多余的并联负载/干扰路径；② PA1 干净（ADC1_IN1），CubeMX 里
 * `PA1.Signal=ADCx_IN1` 已配置。⇒ **PA0 从此空闲**（SW2 电路仍在板上但没有
 * 代码消费它；要用按键就在 .ioc 里把 PA0 配成 GPIO_Input + PULLDOWN 再建
 * bsp_key —— 别两边都留半截配置）。 */


/* ------------------------------------------------------------ ADC1 + DMA + TIM8 ----
 * 依据：总设计 §2.1 / §5.6。**这一节是采样链的唯一来源**。
 *
 *  采样率 fs = 2.000 MHz（TIM8 TRGO 触发，每个更新事件一次转换）
 *  N = 4096 点 ⇒ 记录长度 2.048ms，Δf = fs/N = 488.28125 Hz（≤500Hz ✓）
 *  ADC 占空 = 416.67/500 = 0.8333（转换完还有 83ns 余量，不需要连续转换模式）
 *
 * ⚠️ ADCCLK = 36MHz（= PCLK2/2），采样 3 周期 ⇒ 转换 15 ADCCLK = 416.67ns。
 *    这是"每 500ns 出一个点"能成立的**全部依据**，改采样周期数必须重算。
 *
 * ⚠️ **缓冲必须放 SRAM1/SRAM2，绝不能放 CCM**：CCM 不可被 DMA 访问，
 *    放错**不报错、不告警**，采集结果恒为 0。
 *
 * ⚠️ **ADC 的触发/DMA 寄存器配置在 Device/Src/bsp_adc.c，CubeMX 只管引脚** ----
 *    2026-09-28 现状：用户已在 CubeMX 激活 ADC1（IN1@PA1、3 周期采样、软件
 *    触发、无 DMA），生成 `MX_ADC1_Init()`。bsp_adc.c 随后会**全量覆写**
 *    CR1/CR2/SQR/SMPR（TIM8 触发 + DMA2_Stream0 循环）—— 先后覆盖关系是
 *    刻意接受的：CubeMX 那份负责引脚 Analog 模式与"外设存在性"，本节的
 *    CHANNEL 宏保证两边选中**同一个通道**（IN1），触发/DMA 以 bsp_adc 为准。 */
#define BOARD_ADC_IN_PORT 0   /* 0 = GPIOA */
#define BOARD_ADC_IN_PIN 1    /* PA1 = ADC1_IN1（2026-09-28 起，.ioc 同款；PA0 上有 SW2 电路弃用） */
#define BOARD_ADC_CHANNEL 1U  /* ADC_CHANNEL_1 —— 必须与 .ioc 的 ADC1.Channel-1 一致 */
#define BOARD_ADC_SAMPLING_CYCLES 3U /* 采样周期数：3 ADCCLK（.ioc ADC_SAMPLETIME_3CYCLES 同款） */
#define BOARD_ADC_CLOCK_DIV 2U       /* ADCCLK = PCLK2 / 2 = 36MHz */

/* ADC 参考（mV）：**≈3.3V，不是 3.0V**（2026-10-02 按官方核心板工程文件核实）。
 *
 * 证据链（官方 `.epro` 网表 + 打印版 PDF P.6）：
 *   · VREF+(U1 pin21) 与 R1.1 / R2.1 / R4.1 / C5 / C6 / TP4 同网；
 *   · **R4 = 47Ω 贴**：VDDA → VREF+；而 VDDA = +3V3 经 L1（600Ω@100MHz 磁珠）；
 *   · 板载基准 U3（库名 REF3033AIDBZR / 料号 REF3030AIDBZR）**不贴**；
 *   · R1 / R2（各 0Ω）**均不贴** ⇒ EXT_CHIP_VERF+ 与 EXT_PIN_VERF+（排针 P1.2）
 *     两条外部基准通路都是断的。
 *   ⇒ 出厂 VREF+ ≈ VDDA ≈ 3.3V；**核心板排针 REF 针（EVK1.2）是悬空桩**。
 *
 * ⚠️ 旧注释按 REF3030 标称写 3000 —— 错在把"库里有这颗料"当成"装上去了"。
 * 影响面：幅值口径走**乘性标定表**（op_cal 的 g(k)），标定后自动吸收此常数；
 *   受影响的只有未标定回退路径、以及模拟前端的直流工作点核算
 *   （VBIAS = VREF/2 = 1.65V，`BOARD_VBIAS_SENSE_CODE_MID` 的半量程判据不受影响）。 */
#define BOARD_ADC_VREF_MV 3300U
/* 模拟前端净增益（源端电动势 → ADC）。**2026-10-15 按实物修复后重核**：
 *   端接 ×0.5（R1 49.9Ω 与源 50Ω 分压）
 *   × LC 双端接插损 ×0.5（R3/R4 各 49.9Ω，8 阶 Butterworth 匹配时负载只拿到源的一半）
 *   × U1B 同相 ×(1 + R6/R7) = 1 + 6.8k/1k = **7.8**
 *   ────────────────────────────────
 *   = **1.95**   复核：`tools/sim/`（afe_net 已同步 R6=6.8k/R7=1k）EMF 400mVpp ⇒
 *                进 ADC 点 775mVpp @10kHz / 687mVpp @500kHz（板上实测 800 / ~700）
 *
 * 来历：`R7` 实物曾误贴 **100kΩ（丝印 `1003`）** ⇒ 1+R6/R7 = 1.15（实测 120→140mV，即"放不大"）。
 *   2026-10-15 换件为 `R6`=6.8k / `R7`=1k。
 *
 * ⚠️ U1B 级增益 7.8 ⇒ 闭环带宽 GBW/7.8 ≈ 1.28MHz，500kHz 处相对低频 **−1.05dB**。
 *    该跌落是 **LTI**，由标定表（op_cal 的 g(k)）吸收 —— 连本常数自身的舍入也一起吸收；
 *    受影响的只有**未标定回退路径**。
 * ⚠️ **换件/改增益后必须在本板重做 17 点标定**：旧表是按旧频响测的，不再有效。
 *
 * 码值 → 输入端 mV：lsb_in_mv = VREF/4096/净增益（app_engine.c 装配，按 float 用）。
 * 原 BOARD_FRONT_GAIN=4U 只是运放级增益、且漏算两个 ×0.5（14 号文档 D2/D7）——已废。 */
#define BOARD_NET_GAIN 1.95f

/* TIM8：fs 的来源。144MHz / (PSC+1) / (ARR+1) = 144e6 / 72 = 2.000MHz
 *
 * ⚠️ 这里**只有报数帧一个值**（ARR=71）。原有一个 `BOARD_TIM8_ARR_AUDIT=89`
 *    （审计帧 fs=1.6MHz，为"双帧变频识别 u_J"预留）**已删除** —— 它没有任何
 *    代码读，正是本文件末尾警告的"孤儿宏：翻成 1 什么都不会发生"。
 *    该功能见 `docs/02_优化方案_硬件与软件.md` H9；真要做得先统一 K 口径
 *    （report_opt.md 用的是 K71/K73，不是 K90），再加回宏并配上实现。 */
#define BOARD_TIM8_PSC 0U
#define BOARD_TIM8_ARR 71U /* fs = 2.000 MHz（K = 72） */

/* DMA：ADC1 → 内存，循环模式。
 * ⚠️ DMA2_Stream0 是 ADC1 的**固定**映射（通道 0），不能换流。
 * ⚠️ 优先级 5 = configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY（见 FreeRTOSConfig.h）——
 *    **正好等于**该值 ⇒ 中断里**允许**调 FromISR 系列（数值再小就不许了）。
 *    搬满一帧时 `HAL_ADC_ConvCpltCallback` 会 `xSemaphoreGiveFromISR` 唤醒测量任务。 */
#define BOARD_ADC_DMA_STREAM 0U       /* DMA2_Stream0 */
#define BOARD_ADC_DMA_IRQ_PRIORITY 5U /* 抢占优先级（分组 4，无子优先级） */
#define BOARD_ADC_BUF_LEN 4096U       /* N = 4096 点/帧 */
/* ⚠️ 采集是**单缓冲**（engine 的 s_adc_buf）：门控采集下生产者 2.048ms/帧、
 *    DSP ~13ms/帧，双缓冲只会让新帧盖掉未处理完的旧帧。原来的
 *    `BOARD_ADC_BUF_COUNT 2U` 是个与实现矛盾的孤儿宏，已删除。 */

/* --------------------------------------------------------------- 液晶屏（8080）----
 * 屏：3.5″ IPS 320×480 · 驱动 IC **ST7796S**（兼容 HX8357）· 40pin FPC 并口模块。
 * 接口：**8080 16 位并口，走 FSMC Bank1 (NE1)**，RS 接 A16。
 *
 * ---- 引脚：数据线 D0~D15（FSMC 在 LQFP100 上的唯一可用映射，不可自选）----
 * ⚠️ 这 20 个脚**不是随便挑的**：FSMC 的数据/控制线在 LQFP100 封装上固定，
 *    且 LQFP100 **没有 PF/PG 口** ⇒ 地址线只能用 A16~A18（PD11~PD13）、
 *    片选只能用 NE1（PD7；NE2/3/4 在 PG 口，本封装没有）。
 *    ⇒ 数据线只能落在 PD0/PD1/PD8~PD10/PD14/PD15 + PE7~PE15 上。 */
#define BOARD_LCD_FSMC_BANK 1U /* FSMC_NORSRAM_BANK1（NE1） */

#define BOARD_LCD_D0_PORT 3 /* 3 = GPIOD */ /* PD14 */
#define BOARD_LCD_D0_PIN 14
#define BOARD_LCD_D1_PORT 3 /* PD15 */
#define BOARD_LCD_D1_PIN 15
#define BOARD_LCD_D2_PORT 3 /* PD0 */
#define BOARD_LCD_D2_PIN 0
#define BOARD_LCD_D3_PORT 3 /* PD1 */
#define BOARD_LCD_D3_PIN 1
#define BOARD_LCD_D4_PORT 4 /* 4 = GPIOE */ /* PE7 */
#define BOARD_LCD_D4_PIN 7
#define BOARD_LCD_D5_PORT 4 /* PE8 */
#define BOARD_LCD_D5_PIN 8
#define BOARD_LCD_D6_PORT 4 /* PE9 */
#define BOARD_LCD_D6_PIN 9
#define BOARD_LCD_D7_PORT 4 /* PE10 */
#define BOARD_LCD_D7_PIN 10
#define BOARD_LCD_D8_PORT 4 /* PE11 */
#define BOARD_LCD_D8_PIN 11
#define BOARD_LCD_D9_PORT 4 /* PE12 */
#define BOARD_LCD_D9_PIN 12
#define BOARD_LCD_D10_PORT 4 /* PE13 */
#define BOARD_LCD_D10_PIN 13
#define BOARD_LCD_D11_PORT 4 /* PE14 */
#define BOARD_LCD_D11_PIN 14
#define BOARD_LCD_D12_PORT 4 /* PE15 */
#define BOARD_LCD_D12_PIN 15
#define BOARD_LCD_D13_PORT 3 /* PD8 */
#define BOARD_LCD_D13_PIN 8
#define BOARD_LCD_D14_PORT 3 /* PD9 */
#define BOARD_LCD_D14_PIN 9
#define BOARD_LCD_D15_PORT 3 /* PD10 */
#define BOARD_LCD_D15_PIN 10

#define BOARD_LCD_NE1_PORT 3 /* PD7  · 片选 CS，低有效（FSMC_NE1） */
#define BOARD_LCD_NE1_PIN 7
#define BOARD_LCD_NOE_PORT 3 /* PD4  · 读选通 RD（FSMC_NOE）—— 只写屏也可不接 */
#define BOARD_LCD_NOE_PIN 4
#define BOARD_LCD_NWE_PORT 3 /* PD5  · 写选通 WR（FSMC_NWE） */
#define BOARD_LCD_NWE_PIN 5
#define BOARD_LCD_RS_PORT 3 /* PD11 · 寄存器选择 DC/RS（FSMC_A16） */
#define BOARD_LCD_RS_PIN 11

/* 背光与复位：普通 GPIO（CubeMX 生成，非 FSMC）
 *
 * ⚠️⚠️ 背光**恒亮、禁 PWM**（2026-09-28 拍板，干扰源管理红线）：
 *    PWM 调光的开关频率及其谐波是**等间隔谱线列**——20kHz 调光的前 25 次谐波
 *    全部落在 10k~500kHz 分析带内，在谱线图上是固定频率伪峰制造机
 *    （固定频率 = 会被误认为真信号；等间隔 = 一眼"像谐波族"），比随机噪声
 *    恶劣得多。竞赛装置根本不需要调光 —— PB0 只许数字开关（GPIO_Output，
 *    现状如此），**严禁**在 CubeMX 里把 PB0 改配成 TIM3_CH3 之类 PWM 通道
 *    （PB0 恰好是 TIM3_CH3 的复用脚，别"顺手"配）。 */
#define BOARD_LCD_BL_PORT 1 /* 1 = GPIOB */ /* PB0 · 高 = 背光亮 */
#define BOARD_LCD_BL_PIN 0
#define BOARD_LCD_RST_PORT 1 /* PB1 · 低有效，空闲置高 */
#define BOARD_LCD_RST_PIN 1

/* ---- 屏参数 ---- */
#define BOARD_LCD_WIDTH 320U  /* 横向像素（竖屏原生 320×480） */
#define BOARD_LCD_HEIGHT 480U /* 纵向像素 */

/* ---- FSMC 控制器时序：**在 `.ioc` 里，不在这里** ----
 * 2026-09-26 修正：原先以为 FSMC 参数无法写进 `.ioc`（当时 CubeMX 每次都卡在
 * `IP not ready for code generation: FSMC`），后来查明**卡死的原因是 CubeMX 自己
 * 的模板临时文件 `MXTmpFiles/` + `*.ftl` 残留在工程根** —— 清掉之后参数全部被接受。
 * ⇒ 现在 `MX_FSMC_Init()`（Core/Src/fsmc.c）由 CubeMX 完整生成。
 * **改时序请去 CubeMX 的 FSMC 页改 `.ioc`**，不要在这里加宏 ——
 * 加了也不会生效（没有代码读它），只会变成第二个真相源。
 *
 * ★★ 2026-10-13 现行值是 `AddressSetupTime=2` / `DataSetupTime=8`**
 *    （`.ioc` 的 `FSMC.AddressSetupTime1/DataSetupTime1` 与生成的 `fsmc.c` 一致）。
 *    写周期 ≈ (AS + DS + 1) HCLK = 11 HCLK = **76.4 ns/字** ⇒
 *    **吞吐 ≈ 26.2 MB/s**，整屏 480×320（153600 px / 307 KB）≈ **11.7 ms ⇒ 85.5 fps**。
 *    ⚠️ 2026-10-07 的核对结论是 AS=2/DS=12（104.2 ns/字 / 62.5 fps）；10-13 按"帧率拉满"
 *       收紧到 **DS=8**。**为什么取 8 不取 7**：DS=7 ⇒ 10 HCLK = 69.4 ns，对屏 spec 的
 *       TWC(min)=66 ns 只剩 **5%** 余量，而板上有 33Ω 串阻 + 排线负载（边沿会更慢）；
 *       DS=8 留 **16%** 余量，只慢 10%。要再快必须换扩展模式（见下）。
 *    ⚠️ 屏 spec 依据：ST7796S spec §7.4.1「8080 Series MCU Parallel Interface
 *       Characteristics」TWC（写周期）**Min 66 ns**（⇒ 等效上限 15.15 MHz）。
 *    ⚠️⚠️ **本文件、`bsp_lcd.c`、`project_config.h` 里原先写的"DS=85 / 597 ns/字 /
 *       3.3 MB/s / 整帧 90ms / 一个 draw buffer 9ms" 全是**过期数字** —— 那是
 *       2026-10-02 一次"为了把 CS 率压到 2 MHz 以下"的拍板，但它**从未落进 `.ioc`**：
 *       `.ioc` 的实际走向是 09-28 的 `AS=1/DS=15` → 10-06 的 `AS=2/DS=12`（**更快**）。
 *       git 证据：`git log -G DataSetupTime1 -- firmware/G2026.ioc`。
 *    ⇒ **凡引用"DS=85"做推论的注释都已按现行值改写**；要真按 DS=85 跑，
 *      必须先在 CubeMX 里改 `.ioc` 并重新生成，不能靠改注释。
 *
 * ⚠️ 时序取值依据（改之前先看）：单位 = 1 HCLK = 1/144MHz ≈ 6.944ns。
 *    现行 AS=2/DS=8 ⇒ 每字 ≈76 ns：写 tWRH/tWRL(≥15ns，DS 期间 55.6ns) 与
 *    读 tACC(≈100ns，走 Mode A 的读周期) 仍都满足。
 *    要提速就开扩展模式（AccessMode D + 读写时序分开，写能到 ~27.8ns/字 ≈ 72 MB/s），
 *    但注意 HAL 的约定是 `Timing`→BTR=**读**、`ExtTiming`→BWTR=**写**，极易搞反。
 *    ⚠️ 提速的**干扰代价由 `bsp_lcd` 的"LCD 总线闸"兜**（采样窗内 FSMC 一次不动），
 *       不再靠"全局放慢 FSMC"——但那条闸**必须上板用示波器验**（见 `hardware/` 相关页）。 */

/* 8080 的两个"寄存器地址"：FSMC_A16 = HADDR[17]（16 位总线时地址线右移 1）
 *   Bank1 基址 0x60000000；A16 置 1 ⇒ +0x20000
 *   ⇒ 命令 = 基址，数据 = 基址+0x20000。
 * ⚠️ 偏移是 1<<17 **不是** 1<<16：写成 1<<16 会打到 A15 上，症状是"屏完全没反应"。 */
#define BOARD_LCD_BASE_ADDR 0x60000000U
#define BOARD_LCD_CMD_ADDR (BOARD_LCD_BASE_ADDR)
#define BOARD_LCD_DATA_ADDR (BOARD_LCD_BASE_ADDR + (1UL << 17))

/* -------------------------------------------------------- 触摸：FT6336（电容）----
 * 走 **I2C2**（PB10=SCL / PB11=SDA，AF4），100kHz 标准模式。
 *
 * ---- 器件事实（2026-09-28 取证改判，原记录"GT911"作废）----
 *  IC = **FT6336**（FT5x06 系）。证据：屏厂例程
 *  《TK035F9629_ST7796版/STM32_16位并口_标准库_电容触摸》touch_CTP.c
 *  （docs/ 整理资料下）：`FT6206_ADDR = 0x70 // 地址为0x38要移一位`
 *  ⇒ 7 位地址固定 **0x38**（无 GT911 的"复位时 INT 电平选地址"机制），
 *  8 位寄存器、只读轮询、不接 RST/INT。实现见 bsp_touch.c 文件头。
 *
 *  ⚠️ I2C 需要**外部上拉**（SDA/SCL 各 4.7k 到 3V3）—— STM32 的 I2C 是开漏，
 *     模块若没带，必须自己加，否则总线拉不起来（表现为 HAL_I2C 超时）。
 *
 *  ⚠️ **TP_INT（PC5）是 FT6336 的输出脚，不该由我们驱动**：
 *     ✅ **2026-10-02 已改（用户 Q2 批准，即 R6 的人批准）**：`.ioc` 与 `MX_GPIO_Init()`
 *     双双改为 **输入 + 上拉**（`GPIO_PuPd=GPIO_PULLUP` / `Signal=GPIO_Input`，
 *     照 `SD_CARD_DET(PD3)` 的既有写法）。代码侧 bsp_touch 不碰这个脚。
 *
 *  ⚠️ **PC4（前 TP_RST → 后 VBIAS 自检脚）已于 2026-10-05 整体拆除**：图上 `EVK1.9` 打 NC、
 *     `.ioc` 里 PC4 条目已删（PinsNb 52→51）⇒ 下面的 `BOARD_VBIAS_SENSE_PORT/PIN`
 *     两个登记宏随之删除。理由：它是一根**死线**——固件从未读它，"模拟轨在线"判据
 *     实际走 **AFE 输出帧的直流均值**（见下面的 `..._CODE_MID`），人工核对走 `TP5`
 *     测试点（万用表量 1.650V），这条引脚线两头都没有消费者。
 *     触摸复位与屏的 `LCD_RST` 共用（适配板上 18K+100nF RC），不受影响。
 *     ✅ **不配 `ADC1_IN14` 的拍板依然有效**：把 ch14 加进 ADC1 的**常规序列**会改掉
 *     `TIM8_TRGO + DMA` 采集链（1 通道 @2.0 MSPS，见 `adc.c`）。 */
#define BOARD_TOUCH_I2C_INSTANCE 2U /* 2 = I2C2 */
#define BOARD_TOUCH_I2C_ADDR 0x38U   /* 7 位地址（HAL 用时要 <<1），FT5x06 系固定 */
#define BOARD_TOUCH_I2C_SCL_PORT 1   /* 1 = GPIOB */
#define BOARD_TOUCH_I2C_SCL_PIN 10
#define BOARD_TOUCH_I2C_SDA_PORT 1
#define BOARD_TOUCH_I2C_SDA_PIN 11
#define BOARD_TOUCH_I2C_AF 4 /* AF4 = I2C1/2/3 */
/* ---- "模拟轨在线"判据（2026-10-02 拍板：不占 ADC 通道，用 AFE 输出帧的直流均值判）----
 * 机理：VBIAS = VREF/2，而 ADC1 的 VREF+ 与它**同源** ⇒ 两者同比缩放 ⇒
 *       无论核心板给的是 3.0V 还是 3.3V 基准，**半量程恒对应 VBIAS**（≈2048 码）。
 *       输入侧是 AC 耦合（R1 49.9Ω → C1 100nF）⇒ 即便接着信号源，均值也≈偏置。
 * 用途：**只作"这条模拟轨有没有电"的粗判**，不是 VBIAS 的绝对测量。
 * 容差 ±15% 覆盖：运放 Vos×8、R9/R10 容差、少量信号不对称。
 * 已知会带外（只告警不停机）：① `JP2` 被有意断开（AFE↔ADC 的 0Ω 断点）
 *                          ② 模拟轨/基准无电  ③ 输入被强直流量顶住。 */
#define BOARD_VBIAS_SENSE_CODE_MID 2048U /* 12 位半量程 */
#define BOARD_VBIAS_SENSE_TOL_PCT 15U
#define BOARD_TOUCH_INT_PORT 2 /* PC5 · **FT6336 的输出**——本工程不该驱动它（见上） */
#define BOARD_TOUCH_INT_PIN 5
#define BOARD_TOUCH_WIDTH 320U  /* 触摸有效区（= 屏分辨率，与屏同坐标） */
#define BOARD_TOUCH_HEIGHT 480U

/* ---- 触摸轴映射（2026-10-02 新增：由"触摸测试"页现场定，定完写回这三个宏）----
 * 它们修的是「**FT6336 原始轴序 → 面板物理坐标**」这一段（在 `bsp_touch_read` 里应用）。
 * ⚠️ **与显示方向无关** —— 显示旋转由 LVGL 自己负责：`lv_indev_read()` 每一拍都会按
 *    **当前** rotation 调 `lv_display_rotate_point()`（`indev/lv_indev.c`），
 *    而 indev 在 `lv_indev_create()` 时就绑定了默认 display。
 *    ⇒ 2026-10-02 那句"横屏映射一般需要它（SWAP）"是**错的**（2026-10-13 订正）：
 *      旋转不需要在这里补，这里只负责对齐**芯片自己的轴序**。
 *   · `BOARD_TOUCH_SWAP_XY=1`：交换两轴（FT6336 的第一轴沿**长边**时才需要）
 *   · `BOARD_TOUCH_INVERT_X/Y=1`：该轴镜像（由模组贴合方向决定）
 * 用法：进 UI 左侧页签最下面的 **TOUCH** 页，**点四个角**看十字准星落哪 —— 落点对了
 * 就把当时的开关组合写回这里（页面上会显示当前组合）。
 * ★ 2026-10-13 横屏定稿后定的值，**已上板实测：完全跟手** ✓
 *     定值链路（每一步都有证据，不是"转了几度推几个轴"）：
 *     ① **轴分配看量程**：J-Link 读芯片直出值（`g_touch_hist` 诊断，已删）
 *        得 x∈0..319、y∈0..479 ⇒ `P1_X` 沿**短边**、`P1_Y` 沿**长边** ⇒ `SWAP_XY=0`。
 *        旁证：屏厂例程 `touch_CTP.c` 同样把 `buf[5]/[6]`(P1_Y) 当显示 x。
 *     ② **方向靠实测**：全 0 时"点左上→报右下"（两轴都反）；只反一个轴时"x 跟手、y 反"
 *        ⇒ 两轴都要镜像 ⇒ `INV_X=1` + `INV_Y=1`。
 *   ⚠️⚠️ **最容易绕晕的一点**：LVGL 的 `ROTATION_270` 变换**把两个轴交叉了** ——
 *      `lx = ty`、`ly = 319 - tx`。所以**现象上"y 轴反"要动的其实是 `INV_X`**
 *      （它影响 tx ⇒ 影响 ly），改 `INV_Y` 只会让屏幕**横**坐标翻掉。
 *      本次为此白绕了三轮（"改了没变化"就是这么来的）。
 *      ⇒ 结论：**不要用屏幕现象反推这三个宏**。要么读芯片原值算，要么去 TOUCH 页翻开关实测。
 *   ⚠️ 若换屏/换模组批次：进左侧页签的 **TOUCH** 页，三个开关**当场生效**，点四角看准星凑对，
 *      再把屏上那行 `map SWAP=x INVX=y INVY=z` 写回这里。
 * ⚠️ 这三个宏只影响**坐标映射**，绝不要去动 bsp_lcd / MADCTL 的方向（会是两个来源）。 */
#define BOARD_TOUCH_SWAP_XY 0
#define BOARD_TOUCH_INVERT_X 1
#define BOARD_TOUCH_INVERT_Y 1

/* ------------------------------------------------------- LVGL 内存池（放 CCM）----
 * ★ 需求：**LVGL 的堆放 CCM**（64KB，CPU 独占、不占主 RAM、也不吃 DMA 带宽）。
 *
 * ⚠️ 与 DSP 争 CCM —— 这是本节最重要的约束：
 *    ⇒ 本工程按 **32KB 池 + 32KB DSP** 切：
 *         CCM 低 32KB（0x10000000~0x10007FFF）→ `.ccmram` 段
 *              = **FFT 工作块 s_fft（2N floats = 32KB）**   ← 2026-09-28 修正
 *         CCM 高 32KB（0x10008000~0x1000FFFF）→ LVGL 内存池
 *      代价（2026-09-28 起）：**加窗时域 s_xw（N floats = 16KB）与 g_mag 都放 SRAM1**
 *              —— 因为 `arm_cfft_f32` 要求 **2N** 的交织复数缓冲（官方约定
 *              "size 2*fftLen"），CCM 装不下 s_fft(2N) + s_xw(N)。
 *
 * ⚠️ **池放在 CCM 的"高半边"是刻意的**：链接脚本的 `.ccmram` 段从 CCM 起始
 *    地址往上涨，池放在高半边 ⇒ DSP 数组有完整的 32KB 连续空间，
 *    不会一放就撞上池。越界由链接脚本里的 `ASSERT` 拦下（见 STM32F407XX_FLASH.ld）。
 *
 * ⚠️ LVGL 的**绘制缓冲（draw buffer）绝不能放 CCM** —— 它在 SRAM1，
 *    见 Task/Src/app_lvgl.c。 */
#define BOARD_CCM_BASE_ADDR 0x10000000U
#define BOARD_CCM_SIZE_KB 64U
#define BOARD_LVGL_MEM_ADDR 0x10008000U  /* CCM 高半边（= 基址 + 32KB） */
#define BOARD_LVGL_MEM_SIZE_KB 32U       /* LVGL 主堆大小 */
/* ★ 2026-10-13 追加：LVGL 的**第二块池**（4KB，落在 SRAM1 的 .bss）。
 *
 * why 需要它：横屏业务界面实测吃 **≈21.4KB** LVGL 堆（65 个对象；PC 侧 `lv_mem_monitor`
 *   逐条计量，推导与实测写在 `Task/Src/app_ui.c` 文件头"内存"一节）。
 *   而 CCM 里**没有余量**：低半被 DSP 的 `s_fft`（2N float = 32KB）占死、
 *   高半就是主池那 32KB。主池 + 板侧基线（`lv_init` ≈3.1KB + 显示/indev/sysmon）
 *   ≈ 32~33KB ⇒ 差一点点，且**不留运行时余量** ⇒ 在 SRAM1 补 4KB：合计 ≈36KB。
 *
 * how：`Task/Src/app_lvgl.c` 的 `app_lvgl_init()` 里 `lv_mem_add_pool()`（LVGL 官方
 *   多池接口，仅 `LV_USE_STDLIB_MALLOC = LV_STDLIB_BUILTIN` 时有效）。
 * ⚠️ 代价 = RAM +4KB。预算以 `Tools/stm32_size.py` 的 `BUDGETS` 为**唯一口径**
 *   （当前 RAM 门禁 120KB，2026-10-13 由用户两次点名放开；本文件不再复述数字，
 *    免得又出现"三套口径互相矛盾"——2026-10-14 八轮订正）。
 * ⚠️ 它**不占 CCM**：那 64KB 已按上面的切分用满，别往那儿挪。 */
/* ★ 2026-10-14：**4 → 0**（页面改按需创建/用后销毁之后，这块 SRAM1 扩展不再需要）。
 * 依据：稳态 `heap free ≈ 24.6~26.9 KB`（池合计 34.5 KB ⇒ 实际用 ~9 KB），
 *   而峰值出现在"快滑时 2~3 页同时驻留"⇒ 观测到的 used 峰值 ~14.5 KB ⇒ 32 KB 主池
 *   （CCM）就够。⚠️ 判据（改后必须验）：开机 + 三页来回滑若干次，
 *   `[UI] LVGL 堆` 的 `free` **不得掉到 2 KB 以下**（低于它会静默分配失败）。*/
#define BOARD_LVGL_MEM_EXT_KB 0U

/* ---------------------------------------------------------------- 闭环契约 ----
 * AI 自动验证靠这两行标记判定固件是否真的跑起来了（详见 docs/11 §5）。
 * 改标记必须同步改 `Tools/fw.py`、`.vscode/tasks.json` 与 `AGENTS.md`，
 * 否则自动化会静默失效 —— 这是一条**跨文件契约**。 */
#define BOARD_BOOT_OK_MARKER "[AI_READY]"
#define BOARD_BOOT_FAIL_MARKER "[AI_FAIL]"

/* ============================================================================
 *  附：加"功能开关"时的一道闸门
 *
 *  一个 `#define BOARD_ENABLE_XXX 0` 如果**没有任何代码读它**，它是"孤儿宏"：
 *  定义存在、看着像个开关，翻成 1 却什么都不会发生 —— 这是最难查的一类静默失效。
 *
 *  把意图写成**编译期闸门**，它就不可能被误用：
 *
 *      #define BOARD_ENABLE_IWDG 0
 *      #if BOARD_ENABLE_IWDG
 *      #error "BOARD_ENABLE_IWDG=1，但工程里没有 bsp_iwdg 实现：请先补模块再启用"
 *      #endif
 *
 *  两个效果同时成立：① 记录了"我知道有这个需求，只是还没做"；
 *  ② 谁翻成 1 谁当场看到原因，而不是编译通过、运行无变化。
 *  顺带：宏被 `#if` 引用了，所以它不会变成"定义了却没人读"的孤儿宏。
 * ==========================================================================*/

#endif /* BOARD_CONFIG_H */
