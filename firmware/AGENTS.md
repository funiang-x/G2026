# AGENTS.md — STM32F407VGT6 四层工程模板（AI 工作手册 · 骨架）

<!-- my_skills:project-door -->
[MUST] 开工前置：先读 skill 库的 `C:/Users/funiang/.ai-skills/ROUTE.md`，按其 §1 走四步
（拿证 → 判类 → 装 skill → 读参考），**先输出 `[ROUTE]` 声明**再干活。
拿证命令：`python C:/Users/funiang/.ai-skills/tools/preflight.py <任务类型>`。

## 本工程的状态落点

当前进度与门禁基线只认工程根的 `PROJECT.md`（字段定义见
`C:/Users/funiang/.ai-skills/framework/阶段状态.md`）。**只在跨阶段或改基线时写它。**
<!-- /my_skills:project-door -->

> 给 AI 助手的**骨架**：只放"动手前必须知道"的硬事实与硬规则，超 3 行的展开一律在
> `docs/` 下。人读 `README.md`，机器读这里；**冲突时以本文件为准**。
> 下表是实测事实，**不要靠反复 `ls` / `which` / 读 `CMakeLists.txt` 重新发现**。
> **本文件只能由人修改**（见 §8）。

## 1. 环境事实（实测）

| 项 | 值 |
|---|---|
| 定位 | **STM32F407VGT6 四层工程模板**：CubeMX 直生成 + GCC/Ninja + J-Link SWD + RTT + GDB |
| 芯片 | STM32F407VGTx（1MB Flash / 192KB SRAM / 64KB CCM）· SYSCLK 168MHz · HSE 8MHz · FreeRTOS |
| 板卡 | 立创·梁山派·天空星 F407 开发板；已配 SPI2 / USART1 / USB_OTG_FS / SWD |
| 工具链 | `arm-none-eabi-gcc` 15.2.1（PATH 上）· CMake 4.3.1 + Ninja 1.13.2 |
| 探针 | J-Link SWD 4MHz，器件名 `STM32F407VG`；`C:/Program Files/SEGGER/JLink_V936/` |
| 日志 | SEGGER RTT 通道 0，源码 vendor 在 `Middlewares/RTT/` |
| CubeMX | `C:/Users/funiang/AppData/Local/Programs/STM32CubeMX`（用户级）· 固件包 F4 V1.28.3（`.ioc` 锁死） |
| 统一入口 | `python Tools/fw.py <env\|build\|flash\|rtt\|gdb\|verify\|size\|sizecheck\|test\|lint\|doccheck\|selftest\|clean>` |
| 分层 | `Task/`(调度) → `Operation/`(纯逻辑·电脑可测) → `Device/`(唯一碰 HAL) → `Core/`+`Drivers/`；`Common/` 横切零依赖 |
| 常量 | `Core/Inc/board_config.h`=硬件事实 · `project_config.h`=应用参数（**都不许在 `.c` 里散落魔数**） |
| 生成物 | `Core/`（CubeMX 写）· `Drivers/` · `cmake/stm32cubemx/` · `Middlewares/RTT/`（vendor） |

命名 `层前缀_模块名`（`op_blink` / `bsp_log` / `app_blink`）；依赖只写在 CMake 的 target 里 ——
**用了没声明的层，直接编译失败**。

## 2. 硬规则（优先级高于一切）

- **R1** 只能上层调下层：`Task → Operation → Device`，不许跳层（`Task` 直接调 `Device` 也不行）。唯一例外是组合根 `Task/Src/app.c`。
- **R2** `Operation/` **禁止** include 任何 HAL / FreeRTOS / `Device/` 头（含 `vTaskDelay`）。时间由 Task 以**参数**传入（`now_ms`）。
- **R3** 中断代码放 `Device/`（含 `HAL_xxx_Callback`），**只投事件不跑业务**；`Task/` 取出后再调 `Operation/`。
- **R4** CubeMX 生成物只读：`Core/` 下只在 `USER CODE BEGIN/END` 间写；`Drivers/`、`cmake/stm32cubemx/`、`Middlewares/RTT/` 一行都不改。
- **R5** 零告警**只在 `--clean-first` 后算数**：`fw.py build --clean-first` 的 warning+error 总数为 0 才算完成。不许加 `-Wno-*` 消错。
- **R6** 不可逆操作先问人（烧录 · 改引脚/时钟 · 改 `.ioc` · 改 `CMakeLists.txt` 目标划分 · 改门禁脚本）；`git stash`、`git reset --hard`、`flash --erase` **AI 不碰**。

冲突时：**安全（R6 → `docs/12`）> 分层（R1~R3）> 风格（R5）**。
细则：R1~R3 → `docs/01` ｜ R4 → `docs/01` 规矩 4 ｜ R5 → `docs/11` ｜ R6 → `docs/12` ｜ 提案 → `docs/13`

### 改完必须跑到哪一级（不许跳级交差）

| 改了 | 最低验证 |
|---|---|
| `*.md` | `fw.py doccheck`（死链是无声的） |
| `Operation/` | `fw.py test`（PC 单测，秒级，不需要板子） |
| `Task/` · `Device/` · `Common/` 代码 | `fw.py build --clean-first` 零告警 |
| `.ioc` · `board_config.h` 的引脚/时钟 | 上行 + `fw.py lint` + **必须上板** |
| **新加外设驱动** | 上行 + **加进 `app_self_test()`** —— 否则它永远停在"未验证" |
| `Tools/` 门禁脚本 | 上行 + `fw.py selftest` |
| 内存布局 / 链接脚本 | 上行 + `fw.py sizecheck` |

## 3. 三条命令

```bash
python Tools/fw.py build --clean-first     # 构建（零告警口径）
python Tools/fw.py flash                   # 烧录（执行前和人对一下探针）
python Tools/fw.py rtt                     # 看日志，Ctrl+C 停
```

**构建报错从第一条 error 改起。** 体积**只认一个口径**：`fw.py sizecheck`
（实现 `Tools/stm32_size.py`，上限写在该文件顶部的 `BUDGETS`）；`arm-none-eabi-size`
的 Berkeley 三列**不是**占用。闭环验证 `fw.py verify`：退出码 **0**=`[AI_READY]` ·
**1**=`[AI_FAIL]` · **2**=超时（探针不在线时**不要**把"编译通过"说成"验证通过"）。

## 4. 任务路由表（动手前先读；**文档不存在时先去问人**，不要自行发挥）

| 我要做的事 | 先读 |
|---|---|
| 写/改分层代码（不确定放哪层） | `docs/01` |
| 新增模块（业务 / 驱动 / 任务 / 文档） | `docs/10`（四类逐步清单） |
| 写 `.c` `.h`（文件头 · include · 命名） | `docs/02` `docs/03` `docs/04` |
| 注释 · 类型 · 模块内部组织 · 找现成模式 | `docs/05` `docs/06` `docs/07` `docs/09` |
| 格式（缩进 / 括号 / 行宽） | `docs/08`（权威是 `.clang-format`） |
| 构建 · 烧录 · RTT · GDB · 崩溃定位 · 体积 | `docs/11` |
| **不确定某个操作能不能做** | `docs/12` ← **优先级最高** |
| 提规范 / 改规范 | `docs/13` |
| 这套规范从哪来、改过什么 | `docs/00` |

## 5. 写寄存器 / 外设代码前

**禁止凭记忆写寄存器位定义和复用号。** 顺序是：

1. 查 `Drivers/CMSIS/Device/ST/STM32F4xx/Include/stm32f407xx.h` 里的位定义宏；
2. 复用号查芯片数据手册的 *Alternate function mapping* 表（本仓库不携带 PDF，需要时向用户索要）；
3. 拿不准时**明确说"未验证"**，不要编造一个看起来合理的值。

## 6. 日志与诊断

- 打印一律走 `LOG_E/W/I/D(...)`（`Common/Inc/log.h`），不要直接 `printf`，也不要直接调 `SEGGER_RTT_Write`。
- 崩溃现场：`bsp_fault_report()` 会打印 PC/LR 与 `addr2line` 命令 —— **先把那条命令跑一遍再下结论**，不要靠猜。
- ⚠️ RTT 上行缓冲只有 1024 B 且**满则静默丢**：批量输出必须分块让路（见 `docs/11` §RTT 丢数据）。

## 7. 交付口径

**交付必须三态标注**：**已验证**（写出跑了哪条命令、结果如何）/ **未验证**（说明为什么没跑）/
**不适用**。"编译通过"不等于"能跑" —— 本工程的**外设收发与时钟实际频率尚未上板验证**。
交付前逐条过 `docs/10` §交付前的总检查。**不许伪造数据。**

## 8. 规范提案与变更记录

按 `docs/13` 流程：AI 写提案（**带证据**）→ 人审核 → 通过后才写入。
**只有人能改本文件**；临时笔记写 `docs/archive/` 或 `.workbuddy-ai/memory/`。

| 日期 | 改了什么 | 谁批准 |
|---|---|---|
| 2026-09-21 | 初版骨架：工程侧从 `stm32prj/template/AGENTS.md` 提炼（四层 + 四条规矩 + 门禁口径），文档侧从 `esp32_prj/docs/01~13` 提炼（骨架 ≤110 行 + 编号子文档 + 死链门禁 + 安全档位）；溯源见 `docs/00` | funiang 本次指令 |
| 2026-09-21 | §3 与 §4 的两处笔误修正：体积上限从 `Tools/size_budget.json` 改为 `Tools/stm32_size.py` 的 `BUDGETS`（实现时定为单文件，见 `docs/00` §1.4）；删掉 `Tools/new_peripheral.py` 引用（该脚手架未实现，`docs/10` §B 已给手工三步）。⚠️ 这两处是**实现与规范不一致**，按 `docs/13` 应由人批准 —— 已在本行留痕，请 funiang 追认 | 待追认 | <!-- doclink-ignore: 本行按变更记录惯例提到已删除的旧路径 -->