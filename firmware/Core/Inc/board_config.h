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
 * 默认后端是 **RTT**（走调试探针，不占串口、不打断实时性）——
 * 所以下面这一节**默认不被用到**，它是 `--preset Debug-UART` 的后端配置。
 *
 * 本板：USART1 = PA9(TX) / PA10(RX)，AF7。已由 CubeMX 配置（见 .ioc），
 * 所以 UART 后端是**可直接用**的（不是"宣称支持但没人用"）。
 *
 * ⚠️ 引脚/复用/时钟**不是**由 Device 层配的 —— 是 CubeMX 生成的
 *    `Core/Src/usart.c` 里的 `HAL_UART_MspInit` 配的（那是 .ioc 的产物）。
 *    Device 层的 bsp_log.c 只调 `HAL_UART_Init`，**刻意不自己定义 MSP**
 *    （两边都定义会 multiple definition，实测链接失败 —— 见 bsp_log.c 说明）。
 *
 *  ⇒ 所以下面这组宏分两类，用途不同，别混：
 *      · `PORT` / `BAUD`        —— **被 bsp_log.c 读**，是后端的行为参数
 *      · `TX_*` / `RX_*` / `AF` —— **接线事实的登记**，由 `fw.py lint` 与 .ioc 对账
 *        （保证"登记的接线"和"CubeMX 实际配的"不会漂移）。
 *        它们不参与驱动实现，改它们不会改变引脚 —— 改引脚要去 CubeMX 改。
 *    ⚠️ 这一区分是实测出来的：原先 Device 层自己配引脚，与生成的 usart.c
 *       撞了 `HAL_UART_MspInit`，`Debug-UART` 预设直接链接失败。 */
#define BOARD_LOG_UART_PORT 1     /* 1 = USART1（bsp_log.c 读它选外设实例） */
#define BOARD_LOG_UART_BAUD 115200U
#define BOARD_LOG_UART_TX_PORT 0  /* 0 = GPIOA —— 登记项，见上方说明 */
#define BOARD_LOG_UART_TX_PIN 9
#define BOARD_LOG_UART_RX_PORT 0
#define BOARD_LOG_UART_RX_PIN 10
#define BOARD_LOG_UART_AF 7       /* AF7 = USART1/2/3 —— 登记项 */

/* ------------------------------------------------------------ 外设（已配）----
 * 以下外设由 CubeMX 配置并生成（见 .ioc）。引脚宏列在这里是为了
 * 让"接线事实"有一个可查的地方 —— 驱动实现在 Device/ 下的 bsp_*.c。
 *
 * ✅ 驱动**已就位**（2026-09-23）：`bsp_spi.c` / `bsp_sd.c` / `bsp_rtc.c`
 *    都在 `Device/Src/` 下，且已挂进 `bsp.h` 能力总表。
 *    （本行原先写"还没有 bsp_spi.c / bsp_sd.c"，那是**过时描述**，已修正。）
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

/* USB_OTG_FS —— Device Only 模式，PA11(DM) / PA12(DP)。
 * 板上器件：**Type-C 座** —— 同时是 USB DFU 烧录口（按住 SW3/BOOT0 再复位
 * 进 ROM bootloader，见 docs/11 §4）。⚠️ DFU 只能烧录、**看不到日志**。
 *
 * ✅ `NVIC.OTG_FS_IRQn` 已使能（抢占优先级 5）⇒ `stm32f4xx_it.c` 里有
 *    `OTG_FS_IRQHandler`，`usb_otg.c` 的 MSP 里有 `HAL_NVIC_EnableIRQ`。
 *    （2026-09-22 之前这条是缺的，那时引脚时钟都配了但**枚举不会发生**。）
 * ⚠️ 仍未上板验证：能不能真的枚举成功，要插上 USB 才知道。 */
#define BOARD_USB_OTG_FS_DM_PORT 0 /* 0 = GPIOA */
#define BOARD_USB_OTG_FS_DM_PIN 11
#define BOARD_USB_OTG_FS_DP_PORT 0
#define BOARD_USB_OTG_FS_DP_PIN 12

/* SDIO —— **板载 TF 卡座 CARD1**，4-bit 模式（不是 SPI）。
 *
 * ✅ `NVIC.SDIO_IRQn` 已使能（抢占优先级 5）。
 * ✅ `PD3`（卡检测）已配成 GPIO_Input + 上拉。
 * ✅ **卡检测已上板验证**（2026-09-23）：未插卡时自检正确报出"TF 卡未插入"。
 * ⚠️ **卡的读写未验证** —— 手上没卡，插卡后跑 `bsp_sd_init()` 才知道。
 *
 * ⚠️ **「启动绑架」问题已修复**（2026-09-23 上板实测后）：
 *    `MX_SDIO_SD_Init()` 里的 `HAL_SD_Init()` 在**没插卡时必然失败**，
 *    而它原来调 `Error_Handler()` → `app_panic()` → **整块板子不启动**。
 *    实测调用栈：Reset_Handler → main → MX_SDIO_SD_Init → Error_Handler → app_panic。
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

/* 模拟前端增益（docs/01 §1 定稿 ×4）：码值 → 输入端 mV 的折算系数用它
 * （lsb_in_mv = 3300/4096/GAIN，app_engine.c 装配）。前端 PCB 改增益只动这里。 */
#define BOARD_FRONT_GAIN 4U

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
 * ⇒ 现在 `MX_FSMC_Init()`（Core/Src/fsmc.c）由 CubeMX 完整生成：
 *      16 位 SRAM 模式 / Bank1 / 扩展模式关 / AS=1 HCLK / DS=15 HCLK / AccessMode A
 *   **改时序请去 CubeMX 的 FSMC 页改 `.ioc`**，不要在这里加宏 ——
 *   加了也不会生效（没有代码读它），只会变成第二个真相源。
 *
 * ⚠️ 时序取值依据（改之前先看）：单位 = 1 HCLK = 1/144MHz ≈ 6.944ns。
 *    DS=15 ⇒ 每字 16 HCLK = 111ns：写 tWR(≥15ns) 与读 tACC(≈100ns) 都够。
 *    代价是写吞吐约 18MB/s（300KB 整帧 ≈ 17ms）。
 *    要提速就开扩展模式（AccessMode D + 读写时序分开，写能到 27.8ns/字），
 *    但注意 HAL 的约定是 `Timing`→BTR=**读**、`ExtTiming`→BWTR=**写**，极易搞反。 */

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
 *     .ioc 里它现在是 GPIO_Output 初始低 —— 与 IC 的 INT 输出有**对打**风险。
 *     上板前建议在 CubeMX 把 PC5 改成 GPIO_Input（R6：改 .ioc 引脚要人批准）；
 *     代码侧 bsp_touch 不碰这个脚。TP_RST（PC4）保持输出高即可，
 *     例程证明不需要复位时序。 */
#define BOARD_TOUCH_I2C_INSTANCE 2U /* 2 = I2C2 */
#define BOARD_TOUCH_I2C_ADDR 0x38U   /* 7 位地址（HAL 用时要 <<1），FT5x06 系固定 */
#define BOARD_TOUCH_I2C_SCL_PORT 1   /* 1 = GPIOB */
#define BOARD_TOUCH_I2C_SCL_PIN 10
#define BOARD_TOUCH_I2C_SDA_PORT 1
#define BOARD_TOUCH_I2C_SDA_PIN 11
#define BOARD_TOUCH_I2C_AF 4 /* AF4 = I2C1/2/3 */
#define BOARD_TOUCH_RST_PORT 2 /* 2 = GPIOC */ /* PC4 · 低有效 */
#define BOARD_TOUCH_RST_PIN 4
#define BOARD_TOUCH_INT_PORT 2 /* PC5 · **FT6336 的输出**——本工程不该驱动它（见上） */
#define BOARD_TOUCH_INT_PIN 5
#define BOARD_TOUCH_WIDTH 320U  /* 触摸有效区（= 屏分辨率，与屏同坐标） */
#define BOARD_TOUCH_HEIGHT 480U

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
#define BOARD_LVGL_MEM_SIZE_KB 32U       /* LVGL 堆大小 */
#define BOARD_DSP_CCM_SIZE_KB 32U        /* 留给 .ccmram（DSP 数组）的上限 */

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
