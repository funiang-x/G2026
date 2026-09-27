# G2026 — 状态与恢复说明（2026-09-28）

> 本项目在迁移到 products 时遭并发进程清空一次，已按下方说明恢复。**后面再搞之前先读这条。**

## 方案文档（干活前先读）

- **[docs/01_软件架构与DSP方案.md](docs/01_软件架构与DSP方案.md)** — 定稿：fs=2.0MHz/N=4096/BH4/单点DFT读数口径/8阶BW fc=800k/17点标定；模块清单、任务时序、误差预算、风险表。
- `tools/verify_dsp.py` → `tools/report_dsp_verify.md` — 上述每个数字的数值复算；**改任何采样/滤波参数前先重跑它**。

## 现状

- **固件**：`firmware/` = 2026-09-26 23:41 的自洽态（从门禁自检临时副本恢复）。
  已验证：`build --clean-first` 成功、doccheck 435 条引用与基线一致、PC 单测全绿。
- **`firmware/_recovered_0927_morning/`**：09-27 上午版的 43 个文件存档
  （新任务结构 `app_tasks.c/h`、09-27 的 `G2026.ioc` 等）。**不要删**——补增量时要用。

## 待补的 09-27 下午增量 —— ✅ 2026-09-28 已全部补齐（ZCode 会话）

1. ~~`lv_conf.h`：打开 `LV_FONT_MONTSERRAT_28/16` 字体、注册 `lv_log_register_print_cb`~~ ✅
   另开了 `LV_USE_LOG 1`（不开它 register_print_cb 编不过，lv_log.c 整个被 `#if LV_USE_LOG`
   包裹）；回调移植自 `_recovered_0927_morning` 版 app_lvgl.c。
2. ~~`bsp_lcd`：写像素接口改成 `app_lvgl.c` 调用的 `bsp_lcd_write_pixels` 签名~~ ✅
   核实：当前 app_lvgl.c 的 generic-mipi `send_color` 已在调它，**无需再改**。
3. ~~`bsp_touch`：触摸 IC 是 **FT6336（FT5x06 系，7 位地址 0x38）**，不是 GT911~~ ✅
   已重写 bsp_touch.c/h + board_config.h；寄存器事实取自屏厂例程
   `02_STM32例程/TK035F9629_ST7796版/STM32_16位并口_标准库_电容触摸` 的
   `touch_CTP.c`（`FT6206_ADDR=0x70 // 地址为0x38要移一位`）。

**门禁（2026-09-28）**：`build --clean-first` 零告警（FLASH 294KB/1MB · RAM 54KB/128KB）、
`test` 全绿、`lint` 通过、`doccheck` 435 条无死链。

## .ioc 的 FSMC/I2C2 治愈记录（2026-09-28，ZCode 会话）

**症状**：CubeMX 每次生成弹 `[FSMC, I2C2] not configured or wrong parameter values`；
后来 FSMC 页整个锁死（Memory type 下拉不可改、字段红框、报"not fully configured"）。

**根因**：09-26 脚本把 FSMC 参数写成了**两套上下文族并存**
（`-Lcd1` 全套 13 键 + `-NorPsramChipSelect1_1` 全套 13 键），模型认为同时选中了
多个内存类型变体 → 状态不自洽；且按 db 定义（`db/mcu/IP/FSMC-f4_nornandmux_fsmc7_v4_0_Modes.xml`
与 `I2C-i2c1_v1_5_Cube_Modes.xml`）还缺 9 个 FSMC 参数、3 个 I2C2 参数。

**修法**（全部已验证）：
1. 删除 `-Lcd1` 参数族（13 键）+ 从 IPParameters 摘除；
2. 补 `-NorPsramChipSelect1_1` 族的 9 个缺失参数 + `NORSRAM_Instance`/`Extended`，
   值全部照抄当时生成代码（保证输出不变）；补 I2C2 的
   `I2C_Coeff_DF=0`/`Analog_Filter=I2C_ANALOGFILTER_ENABLE`；
3. **无头生成**（`STM32CubeMX.exe -q`，注意 exe4j 立即返回、JVM 后台跑完，看
   `~/.stm32cubemx/STM32CubeMX.log`）→ fsmc.c/i2c.c/main.c/gpio.c/main.h 与备份
   **逐字节一致**；CubeMX round-trip 接受全部新键、未再写回 `-Lcd1` 参数族；
4. 四道门禁全绿。

**遗留（留人）**：PC5(TP_INT) 改 GPIO_Input 仍未做（R6）；CubeMX 里打开 FSMC 页
确认下拉可编辑、生成时警告是否消失，尚未人眼复核。备份：
`firmware/_backup_20260928_cubemx_regenerate/` + `G2026.ioc.bak_20260928_prefsmc_clean`。

**⚠️ 留给人的（上板前）**：PC5(TP_INT) 在 `.ioc` 里仍是 `GPIO_Output` 初始低，
而 FT6336 的 INT 是 **IC 的输出脚**，有对打风险 —— 上板前在 CubeMX 把 PC5 改成
`GPIO_Input`（R6：改 `.ioc` 引脚需人批准）。代码侧 bsp_touch 不碰这个脚。

## 遗留

- 原路径 `stm32prj\G2026` 若还有空壳文件夹：是进程占用的残留，确认没有程序开着它后整个删掉。
- 迁移期间 `DWIN_prj`（4 个 DGUS 屏工程）在本机消失，与本项目无关但一并留意。
