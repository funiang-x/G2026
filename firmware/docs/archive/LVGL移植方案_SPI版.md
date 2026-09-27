# LVGL 9.5 SPI 版移植 · 整体方案

> 状态：**待 funiang 确认**（未动手，本文只是方案）
> 日期：2026-09-26
> 目标：`f407vgt6` 工程 + 2.8″ ILI9341 SPI 屏（JMD2.8TFT）+ XPT2046 →
> 直接编译、直接烧录就能显示画面（最终跑起 lvgl_windows 里那套信号测量 UI）

> ⚠️ **2026-09-26 后记（本文写完之后发生的变化，照做前必读）**
>
> `f407vgt6` 已定位为**纯模板工程**（只保留板上真有器件的那些外设，具体开发在派生工程里做）。
> **SPI2 已按这个决定从 `.ioc` 移除**（连同 PB10 / PC2 / PC3 三个引脚），
> `Core/Inc/board_config.h` 里的 `BOARD_SPI2_*` 宏也一并删掉了。
>
> ⇒ 本文中所有"**SPI2 已有 / 已配 / 不用改**"的说法**都已失效**。
>    派生工程要驱动这块屏时，**先在 CubeMX 里把 SPI2 配回 Full-Duplex Master**
>    （PB10=SCK / PC2=MISO / PC3=MOSI，AF5，Mode 选 `Full_Duplex_Master`），
>    再按 `docs/10 §B` 往 `board_config.h` 加回宏。
>    排针位置见 `Core/Inc/board_config.h` 的"排针上的自由 IO"一节。
>
> 顺带：USART2 的登记（`BOARD_USART2_*`）同样已删 —— 它板上无器件、代码里也没人读。

---

## 一、先说结论

| # | 结论 | 依据 |
|---|---|---|
| 1 | **显示用 SPI2**（PB10/PC2/PC3），不用 SPI1 | SPI1 的 PA5/PA6/PA7 + PA4 已被**板载 W25Q128** 占用，且已上板验证（JEDEC `0xEF18`）|
| 2 | **触摸用软件 SPI**，不占硬件外设 | XPT2046 上限 ~2 MHz，显示要 21 MHz，共用一路互相拖累；单独配 SPI3 不值得 |
| 3 | **LVGL 内存池放 CCM**（64 KB 目前**完全空闲**）| 主 RAM 要留给 draw buffer + RTOS；池是纯 CPU 数据，SPI 走阻塞发送不碰 DMA |
| 4 | **不用动 SDIO** | 本方案用的引脚与 SDIO（PC8–PC12/PD2/PD3）无交集。少改一处就少一个风险点 |

---

## 二、屏的硬件事实（你给的 PDF + 实测）

- **型号** `JMD2.8TFT`（深圳金马鼎），驱动 IC **ILI9341V**
- **原生 240×320 竖屏**，视角 12:00 ⇒ UI 是 320×240 横屏，**必须靠 MADCTL 旋转 90°**
- 14 pin 排针，引脚（PDF 原厂定义）：

| 脚 | 名称 | 说明 |
|---|---|---|
| 1 | VCC | 3.3~5 V |
| 2 | GND | |
| 3 | CS | 片选，低有效 |
| 4 | RESET | 复位，低有效 |
| 5 | DC/RS | 低=命令，高=数据 |
| 6 | SDI(MOSI) | 显示 SPI 数据输入 |
| 7 | SCK | 显示 SPI 时钟 |
| 8 | LED | **背光，高电平点亮**（不控制就直接接 3.3 V）|
| 9 | SDO(MISO) | 显示 SPI 数据输出（**只写屏可以不接**）|
| 10 | T_CLK | 触摸 SPI 时钟 |
| 11 | T_CS | 触摸片选 |
| 12 | T_DIN | 触摸数据输入 |
| 13 | T_DO | 触摸数据输出 |
| 14 | T_IRQ | 触摸中断（轮询模式可不接）|

---

## 三、工程现状（实测，不是推断）

**芯片 / 时钟**：STM32F407VGT6 LQFP100 · 168 MHz（HSE 8 MHz → PLL M4/N168/P2）· FreeRTOS CMSIS_V2
**内存**：1 MB Flash · 128 KB RAM · **64 KB CCM**
**RTOS**：只有 CubeMX 的 `defaultTask`（256 words / 优先级 24），循环体在 `Task/Src/app_rtos.c`
**构建**：CMake + Ninja + arm-none-eabi-gcc 15.2.1 · `python Tools/fw.py build --clean-first`
**门禁**：`build --clean-first` 零告警 · `test` · `doccheck` · `selftest` · `sizecheck` · `lint`
**基线**：2026-09-26 跑 `fw.py build` → 编译成功，**基线是绿的**

**当前没有 LVGL**（`Middlewares/` 下只有 RTT / FreeRTOS / CMSIS-DSP）。

### 3.1 引脚占用表

| 外设 | 引脚 | 状态 |
|---|---|---|
| SPI1 + PA4(软件CS) | PA5 / PA6 / PA7 | **板载 W25Q128 占用** |
| SPI2 | PB10(SCK) / PC2(MISO) / PC3(MOSI) | 排针自由，**给屏用** |
| USART1 | PA9 / PA10 | 日志（Debug-UART 预设）|
| USB_OTG_FS | PA11 / PA12 | |
| SDIO | PC8 / PC9 / PC10 / PC11 / PC12 / PD2 / PD3 | TF 卡（**本方案不碰**）|
| SWD | PA13 / PA14 | |
| LSE / HSE | PC14 / PC15 · PH0 / PH1 | RTC / 主晶振 |
| LED / 按键 | PB2 / PA0 | |

### 3.2 排针可用性（已核对原理图第 7 页「双路 80pin 排针」）

双路 80pin，**以下全部引出**：
`PA0–PA8, PA15` · `PB0–PB1, PB4–PB15` · `PC0–PC13` · `PD0–PD15` · `PE0–PE15`

⚠️ 两个例外：**PB2**（板上 LED）、**PB3**（JTDO，未引出）。
⇒ 排针上**没有 PB2 / PB3**，选引脚时要避开。

---

## 四、接线方案

### 4.1 显示（ILI9341，走 SPI2）

| 屏脚 | STM32 | .ioc 状态 | 备注 |
|---|---|---|---|
| 1 VCC | 3V3 | — | |
| 2 GND | GND | — | |
| 3 CS | **PB11** | 新增 GPIO_Output | 初始高（不选中）|
| 4 RESET | **PB13** | 新增 GPIO_Output | 初始高 |
| 5 DC | **PB12** | 新增 GPIO_Output | |
| 6 SDI | **PC3** | SPI2_MOSI（已配）| |
| 7 SCK | **PB10** | SPI2_SCK（已配）| |
| 8 LED | **PB14** | 新增 GPIO_Output | 高=亮；不想控制就直接接 3V3，省一个脚 |
| 9 SDO | **PC2** | SPI2_MISO（已配）| 可选；接了能读 ID 做自检，**建议接** |

选这组的理由：PB10–PB14 **连续 5 个脚**，接线最短；PC2/PC3 是 SPI2 原生复用。

### 4.2 触摸（XPT2046，软件 SPI）

| 屏脚 | STM32 | .ioc 状态 |
|---|---|---|
| 10 T_CLK | **PC4** | 新增 GPIO_Output |
| 11 T_CS | **PC5** | 新增 GPIO_Output |
| 12 T_DIN | **PC6** | 新增 GPIO_Output |
| 13 T_DO | **PC7** | 新增 GPIO_Input |
| 14 T_IRQ | **PC13** | 新增 GPIO_Input（可选，轮询模式可不接）|

选这组的理由：PC4–PC7 与 PC2/PC3 同排，触摸线和显示线能一起走。

### 4.3 需要新增的 .ioc 配置

**9 个 GPIO**（4 个输出 + 5 个输入）：
```
PB11 PB12 PB13 PB14        → GPIO_Output（推挽、无上下拉、低速；初值：CS/RST=高，DC/BL 随意）
PC4  PC5  PC6              → GPIO_Output
PC7  PC13                  → GPIO_Input（上拉）
```
**SPI2 要自己配** —— ~~已有，不用改~~（2026-09-26 已从模板 `.ioc` 移除，见本文开头后记）：
在 CubeMX 里配成 `Full_Duplex_Master`，PB10=SCK / PC2=MISO / PC3=MOSI，AF5。

---

## 五、软件架构

### 5.1 分层归属（照 `docs/01` 的四层规矩）

```
Middlewares/LVGL/                ← vendor 的 LVGL 9.5.0 源码 + lv_conf.h（第三方，等同 RTT 的地位）
Device/Inc/bsp_lcd.h
Device/Src/bsp_lcd.c             ← ILI9341 SPI 驱动（唯一碰 HAL 的地方）
Device/Inc/bsp_touch.h
Device/Src/bsp_touch.c           ← XPT2046 软件 SPI 驱动
Task/Inc/app_lvgl.h
Task/Src/app_lvgl.c              ← LVGL 任务：lv_init + 显示/触摸移植 + lv_timer_handler 循环
Task/Src/app_ui.c                ← UI（从 lvgl_windows 的 ui.c 搬，改 include）
```

**为什么这么放**（这是本方案唯一需要解释的分层判断）：

- `bsp_lcd.c` / `bsp_touch.c` 碰 HAL ⇒ **必须 Device/**（R2：Operation 禁 HAL；R1：只有 Device 能用 HAL）
- `lv_port_disp` / `lv_port_indev` 同时依赖 `lvgl.h` 和 `bsp_lcd.h`
  ⇒ **只有 Task 层允许这个组合**：Operation 不许碰硬件，Device 不该反过来依赖 GUI 库
  ⇒ 所以它们并入 `Task/Src/app_lvgl.c`（不单独立文件，避免多一层空壳）
- `ui.c` 只 include `lvgl.h` + 标准库，看起来像纯逻辑，但 Operation 只允许 **C 标准库**
  ⇒ 放 Task/

### 5.2 显示链路

```
LVGL 渲染 → disp_flush_cb(area, px_map)
          → bsp_lcd_set_window(x1,y1,x2,y2)      (0x2A/0x2B/0x2C)
          → bsp_lcd_write_pixels(px_map, n)      (HAL_SPI_Transmit)
          → lv_display_flush_ready(disp)         ← 漏了这行界面永久卡死
```

**颜色格式**：LVGL 侧用 `LV_COLOR_FORMAT_RGB565_SWAPPED`（屏期望大端），BSP 直接按字节发。
若实测红蓝互换 ⇒ 改 `bsp_lcd.c` 里 MADCTL 的 BGR 位（bit3），**不要**同时改两处。

**MADCTL**：横屏 320×240 用 `0x28`（MV|BGR）。若画面横竖颠倒，试 `0x48` / `0xE8`。
这一项**必须上板试**，datasheet 定不了（取决于屏厂 FPC 走线）。

### 5.3 触摸链路

```
lv_timer_handler → touch_read_cb
                 → bsp_touch_read(&x,&y,&pressed)   (软件 SPI 读 XPT2046)
                 → 校准映射 → data->point.x/y
```

**校准值必须实测**（四角各点一次，把原始 ADC 值填进 `board_config.h`）。
先给一组占位值，第一次烧录时打印原始值来标定。

---

## 六、内存预算

| 项 | 大小 | 落点 | 依据 |
|---|---|---|---|
| LVGL 内存池 | **56 KB** | **CCM** | lvgl_windows 文档里 F405 板端实测：控件常驻 24 KB / **渲染峰值 48.5 KB**。池按常驻估会死锁（`LV_ASSERT_MALLOC` 自旋），必须按峰值 |
| draw buffer ×2 | 2 × 25.6 KB = **51.2 KB** | 主 RAM | 320×40 行 × 2 B，SPI 慢必须双缓冲（渲染与传输并行）|
| FreeRTOS heap | 15 KB | 主 RAM | 已有（`configTOTAL_HEAP_SIZE`）|
| 主栈 / bss | ~4 KB | 主 RAM | 已有 |

**主 RAM 合计 ≈ 70 KB / 128 KB**，余量充足。
**CCM 用 56 KB / 64 KB**，剩 8 KB。

⚠️ 两个前提，缺一不可：
1. **CCM 不可 DMA 访问** ⇒ 池放 CCM 的前提是 **SPI 走阻塞式 `HAL_SPI_Transmit`**（本方案就是）。
   将来若切 DMA，**draw buffer 仍在主 RAM**，所以不受影响。
2. 链接脚本里 `.ccmram` 段**已存在**（`STM32F407XX_FLASH.ld` 有 `>CCMRAM AT> FLASH`），不用改脚本。
   池用 `LV_MEM_ADR` 直接指向 `0x10000000`，**不占 .bss**。

**如果只跑 Hello LVGL**（不搬 UI），池 16 KB 就够 —— 但既然 UI 已经做好，直接按 56 KB 配。

---

## 七、改动清单（全部是 B 档，按 `docs/12` §1）

| # | 文件 | 改什么 | 影响面 | 回退 |
|---|---|---|---|---|
| 1 | `f407vgt6.ioc` | 追加 9 个 GPIO | CubeMX 重新生成 `Core/` | `git checkout f407vgt6.ioc` + 重新生成 |
| 2 | `Core/Inc/board_config.h` | 登记新引脚（PORT/PIN 两个宏）| `fw.py lint` 对账 | `git checkout` |
| 3 | `CMakeLists.txt` | 加 `lvgl` 库目标 + device/task 源文件 | **全部预设**（动过一次就出过"只有部分预设能编过"）| `git checkout` |
| 4 | `project_config.h` | LVGL 任务栈深 / 优先级 / 开关 | 产品行为 | `git checkout` |
| 5 | `Middlewares/LVGL/` | vendor 第三方源码 | 体积、告警基线 | 删目录 |
| 6 | `Device/`、`Task/` 新文件 | 新增 | — | 删文件 |
| 7 | 烧录 | 覆盖板上固件 | 不可逆 | 重新烧（`board_backup_20260923.bin` 有旧镜像）|

**动手前先备份**：`cp -r f407vgt6 ../_backup_20260926_lvgl/`，报出路径与文件数。

---

## 八、实施步骤（每步都有可见结果，不许跳步）

| 阶段 | 做什么 | 判据 |
|---|---|---|
| 0 | 备份工程 + 记录基线 | `fw.py build` 绿（已确认）|
| 1 | vendor LVGL + 建 `lvgl` 库目标（先不接任何驱动）| `fw.py build --clean-first` **零告警** |
| 2 | 改 `.ioc`（9 个 GPIO）+ CubeMX 生成 | `Core/Src/gpio.c` 里出现新引脚；`fw.py lint` 通过 |
| 3 | `board_config.h` 登记 + `bsp_lcd.c` + **纯色测试** | 上板：红→绿→蓝，**无红蓝互换** |
| 4 | 接 LVGL（`app_lvgl.c`）| 屏上显示 "Hello LVGL" |
| 5 | `bsp_touch.c` + 校准 | 串口打印坐标，手指位置对得上 |
| 6 | 搬 UI（`app_ui.c` + 中文字体）| 4 个页面能切、按钮有反应 |

⚠️ **第 3 步之前不碰 LVGL** —— 否则屏不亮时你分不清是"屏没点亮"还是"LVGL 配错"，
这是最常见的返工原因。

---

## 九、风险与未知（诚实标注）

| # | 项 | 状态 | 说明 |
|---|---|---|---|
| 1 | ILI9341 初始化序列 | **未验证** | 现成序列来自通用例程，寄存器值需上板核对 |
| 2 | MADCTL 旋转值 | **未验证** | `0x28` 是常见值，但取决于 FPC 走线，要试 |
| 3 | SPI2 实际能跑多快 | **未验证** | .ioc 里是 21 MHz；建议**先用 10.5 MHz**（预分频 4）跑通再提速 |
| 4 | LVGL 池放 CCM | **未验证** | 链接脚本支持，但"能不能真的不占 .bss"要 `sizecheck` 确认 |
| 5 | LVGL 源码的告警 | **未知** | 第三方库可能带告警；会给 `lvgl` 目标单独放宽（照 `segger_rtt` 的做法），不污染工程告警基线 |
| 6 | 触摸校准值 | **未验证** | 必须实测，不能照抄 |
| 7 | 探针 | **待确认** | `fw.py env` 显示 J-Link `Serial number: -1` —— 烧录前要确认探针插好 |

---

## 十、需要你拍板的 4 件事

**① 接线方案**
- **A（我推荐）**：按上面 §4 的表接（显示 PB10–PB14 + PC2/PC3，触摸 PC4–PC7 + PC13）
- **B**：你已经接了别的脚 → 告诉我实际接线，我按你的改

**② 首轮做到哪一步**
- **A（我推荐）**：代码一次写全（显示 + 触摸 + UI），但留一个开关分步验证
  （纯色 → Hello LVGL → 完整 UI）。因为移植代码都是现成的，多花不了多少时间
- **B**：先只做显示，屏亮了再接触摸和 UI。风险最小，但要两轮

**③ SDIO 怎么处理**
- **A（我推荐）**：**不动**。本方案用的引脚和 SDIO 无交集，改它反而要连带处理 `bsp_sd.c`
- **B**：从 .ioc 里移除 SDIO（释放 PC8–PC12/PD2/PD3，去掉启动时"TF 卡未插入"的日志）
  → 那要同步删/桩掉 `Device/Src/bsp_sd.c`，并改 `board_config.h` 和 `bsp.h`

**④ LVGL 源码从哪来**
- **A（我推荐）**：从 `C:\Users\funiang\Desktop\lvgl_windows\lv_port_pc_vscode-master\lvgl\`
  拷（**同一版本 v9.5.0**，本地拷秒级完成，且与 UI 验证过的版本严格一致）
- **B**：`git clone --depth 1 --branch v9.5.0`（需要联网，版本号还得自己核）

---

## 附：这套方案的来源

- 屏的参数：`C:\Users\funiang\Desktop\lvgl_windows\docs\3668626735JMD2.8TFT.pdf`（你提供的规格书）
- UI 与字体：`C:\Users\funiang\Desktop\lvgl_windows\lv_port_pc_vscode-master\src\ui\`（PC 上已验证）
- ILI9341 驱动骨架与初始化序列：`C:\Users\funiang\Desktop\2026G\lvgl-hmi\port\bsp_lcd_spi.c`
  （**但那份是文档区产物，从没编译过、没上过板** —— 搬进来要按本工程的规矩重写，不是照抄）
- 引脚事实：`Project\stm32prj\docs\...\第02章\硬件\立创梁山派·天空星开发板原理图_2024-01-17.pdf` 第 7 页
