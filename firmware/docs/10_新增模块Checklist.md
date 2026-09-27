# 10 — 新增模块 Checklist

> 状态：**已定稿**（2026-09-21）
>
> 照着打勾就不会漏。每一步都给了**判据**，不是"感觉做完了"。

## 先做两件事

- [ ] **定层**：这个模块是"什么时候做"（`Task/`）、"做什么"（`Operation/`）、
      还是"怎么操作硬件"（`Device/`）？→ `01_工程分层架构.md`
- [ ] **起名**：`层前缀_模块名`（`op_` / `bsp_` / `app_`）→ `04_命名规则.md`

> 判据（来自 `01`）：**换块板子也成立 → `Device/`；绑定这块板子的物理含义 →
> `Device/` 再包一层；业务决策 → `Operation/`。**

---

## A. 新增业务逻辑（`Operation/`，推荐起点）

- [ ] 1. `Operation/Inc/op_<名>.h` —— 写文件头（`02`）+ 类型 + 函数原型
- [ ] 2. `Operation/Src/op_<名>.c` —— 纯函数实现，**时间是参数**
- [ ] 3. **自查**：`.c` 里 `grep -n "HAL_\|vTask\|FreeRTOS\|bsp_"` → **必须 0 命中**
- [ ] 4. `Test/test_op_<名>.c` —— 写单测（含负向断言与边界值）
- [ ] 5. `Test/CMakeLists.txt` —— **手工加两行**（`OP_SRC` + `add_executable`）
- [ ] 6. `python Tools/fw.py test` → **退出码 0**
- [ ] 7. 接到 `Task/`：在组合根注入参数，在任务里调 `op_<名>_step()`
- [ ] 8. `python Tools/fw.py build --clean-first` → **零告警**

> ⚠️ **第 3 步是关键判据**，不是形式。规矩 2 的第一道墙是 CMake（拿不到 HAL 头），
> 但自己写 `extern void vTaskDelay(unsigned);` 能骗过编译期 —— 那由第 6 步的
> PC 侧链接拦（`undefined reference`）。
>
> ⚠️ **第 5 步漏了会怎样**：测试全过，但那个模块根本没被测。`Test/CMakeLists.txt`
> 刻意不写 GLOB，就是为了让"加测试"变成一个显式动作。

---

## B. 新增硬件驱动（`Device/`）

- [ ] 1. **先在 CubeMX 里配好外设**（引脚 / 时钟 / DMA），保存 `.ioc` 并生成
      - 判据：`Core/Src/<外设>.c` 里出现 `MX_<外设>_Init()`
- [ ] 2. 把**值**抄进 `Core/Inc/board_config.h`（**只抄值，不抄代码**）
      - 引脚写**端口号 + 引脚号两个宏**（`PORT` 是 0=GPIOA 的枚举值），不写 "PA5" 字符串
- [ ] 3. 建 `Device/Inc/bsp_<名>.h` + `Device/Src/bsp_<名>.c`，在 `Device/Inc/bsp.h`
      的清单里加一行 `#include`（**手工**，见下方说明）
- [ ] 4. 在 `.c` 里 `#include "stm32f4xx_hal.h"` 与 `"board_config.h"`，
      字段**抄 CubeMX 生成物**，别凭记忆
- [ ] 5. ⚠️ **不要自己写 `HAL_<外设>_MspInit/MspDeInit`** —— 那由 CubeMX 生成在
      `Core/Src/<外设>.c` 里（引脚/复用/时钟都在里面）。两边都定义 ⇒
      `multiple definition` **链接失败**。理由见 `11` §3
- [ ] 6. 中断回调写在 `Device/`，**只投事件不跑业务**（`09` §3）
- [ ] 7. `python Tools/fw.py build --clean-first` → 零告警
- [ ] 8. `python Tools/fw.py flash` + `python Tools/fw.py rtt` → 上板看日志
- [ ] 9. ⚠️ **引脚 / 时钟改动必须重新上板验证** —— AI 无法替你判断接线

> ⚠️ **`Device/Src/*.c` 是 CMake 的「显式列表」，不是 GLOB** ——
> 新文件**必须**加进顶层 `CMakeLists.txt` 的 `add_library(device STATIC ...)`，
> 否则**静默不编译**：不报错、`build` 还说"编译成功"，只是符号根本不在库里。
>
> 2026-09-23 实测踩过：新建 `bsp_spi.c` 后跑 `build --clean-first` 显示编译成功，
> 但 `arm-none-eabi-nm build/Debug/libdevice.a | grep bsp_spi` 是 **0 个符号**。
> 加进 `CMakeLists.txt` 后**还要重新 configure**（`cmake --preset <名> --fresh`）
> 才生效 —— `--clean-first` 只清产物，不重读源文件列表。
>
> ⚠️ 另外三层同理：`common` / `operation` / `task_*` 也都是显式列表。
> **判据**：加完文件跑一次
> `arm-none-eabi-nm build/Debug/lib<层>.a | grep <新函数名>`
> —— 有符号才算真的进去了。
>
> ⚠️ `Device/Inc/bsp.h` 也要手工把新头挂进去（它是"板级能力总表"，
> **故意不自动** —— 自动生成的总表没人读，而这张表是"这块板子上有什么"的答案）。

---

## C. 新增周期任务（`Task/`）

- [ ] 1. `Task/Inc/app_<名>.h` —— 声明**显式入口** `app_<名>_poll(uint32_t now_ms)`
- [ ] 2. `Task/Src/app_<名>.c` —— 实现周期逻辑（**不要**写进 `for(;;)` 循环体，见 `07`）
- [ ] 3. `Task/Src/app_rtos.c` —— 用 `xTaskCreate` 建任务，任务体里调 poll
- [ ] 4. `project_config.h` —— **先定义**这个任务的栈深 / 优先级宏（照抄文件里那段
      注释给的形状），再在 `xTaskCreate` 里用它们，**不写魔数**
- [ ] 5. `python Tools/fw.py build --clean-first` → 零告警
- [ ] 6. `python Tools/fw.py rtt` → 看任务是否在跑

> ⚠️ **已有任务（`defaultTask`）的参数不在这里** —— 它是 CubeMX 生成的，
> 栈深 / 优先级只能去 `.ioc` 的 `FREERTOS.Tasks01` 改（见 `11` §3）。
> 本节说的是**加新任务**。
>
> ⚠️ **不要用 CubeMX 的图形界面建任务**（那样任务会落在生成物 `Core/` 里）。
> 但要知道 CubeMX **会强制重建 `defaultTask`** —— 所以本工程的做法是
> 「CubeMX 建空壳 + 循环体写在 `app_rtos.c`」，不是「跟它抢」。

---

## D. 新增文档

- [ ] 1. 文件名 `docs/0x_<名>.md`（**编号必须唯一**，见 `docs/README.md`）
- [ ] 2. 顶部写状态（`已定稿` / `从代码提取，待确认` / `待确认`）
- [ ] 3. 在 `docs/README.md` 的清单表里登记
- [ ] 4. 在 `AGENTS.md` §任务路由表里加一行（**用短引用 `docs/0x`**）
- [ ] 5. `python Tools/fw.py doccheck` → **0 死链、0 陈旧标识符**

> ⚠️ 第 5 步不能省：骨架用的是短引用 `docs/0x`，那**不是合法路径**，肉眼看不出对错。
> 它还会对账**工程自有标识符**（`op_` / `bsp_` / `app_` / `LOG_` / `APP_` / `BOARD_`）
> —— 文档写在实现定型之前时，函数名/宏名会变，而这类不一致**没有任何报错**。
> ⚠️ `docs/02`~`09` 是示例型规范（里面的名字本来就是虚构的），不参与对账 ——
> 改那几篇时名字**靠人核**。

---

## E. 派生新工程 / 换板

> 本工程是**模板**，具体开发在派生工程里做。派生 = **复制 + 改名 + 清衍生物 + 新历史**。
> **同板派生只动 3 个文件**；换芯片才要动更多（§E.3）。

### E.1 同板派生：只改这几处

| 位置 | 改什么 |
|---|---|
| `CMakeLists.txt` 的 `set(CMAKE_PROJECT_NAME ...)` | **唯一权威** —— `.elf` / `.bin` / `.hex` / `.map` 名与 `board.env` 的 `PROJECT_NAME` 全从它派生 |
| `.vscode/launch.json` 的 2 处 `executable` | 指向新的 `.elf`，否则调试器找不到文件 |
| `<模板名>.ioc` 的文件名 + 内部 2 行 | `ProjectManager.ProjectFileName` 与 `ProjectManager.ProjectName` |
| `Core/Inc/board_config.h` | **同板不用改** —— 引脚 / 时钟本来就对 |

> ⚠️ **不要再加第四处硬编码**。`Tools/fw.py` 与 `Tools/selftest.py` 都从 `board.env`
> 取工程名（`PROJECT_NAME` / `ELF`）。
>
> 2026-09-26 实测：`selftest.py` 曾写死 `build/Debug/f407vgt6.elf`，派生改名后
> 体积门禁报"原工程还没构建" —— 一个**只在改名时才现形**的假失败（构建明明是好的）。
> 已改为读 `board.env`。判据：**"改了名还能跑"才算派生友好**。

### E.2 步骤

**用脚本（推荐）** —— 一条命令做完下面所有事：

```bash
python Tools/derive.py <新工程名>                # 派生到本模板的上一级目录
python Tools/derive.py <新工程名> --verify       # 额外跑 configure + build + test
python Tools/derive.py <新工程名> --dest <目录>   # 指定父目录
```

VS Code 里同样可用：`Ctrl+Shift+P` → `Tasks: Run Task` → **⑭ 派生新工程**
（会弹框让你输工程名，带 `--verify`）。

脚本内部做的事：复制（排除 `build/` · `.git/` · `.workbuddy-ai/` · `*.ioc.bak_*`）
→ 改名 3 处（§E.1）→ `git init` + 首次提交 → **残留检查**（列出文档 / 注释里
仍含旧名的地方，**不自动改** —— 那里面混着芯片型号与历史记录）。

退出码：`0` 成功 · `1` 参数非法 · `2` 用法错误 · `3` 模板结构异常 ·
`4` 复制 / 改名失败 · `5` 验证失败 · `6` 目标已存在。
⚠️ **目标已存在时拒绝执行、绝不覆盖。**

**手工做法**（脚本不可用时，与上面等价）：

```bash
# ① 先确认模板本身是绿的
python Tools/fw.py build --clean-first && python Tools/fw.py test

# ② 复制，剔掉衍生物
cd <模板的上一级目录>
cp -r <模板目录> <新工程名> && cd <新工程名>
rm -rf build .git .workbuddy-ai
rm -f *.ioc.bak_*      # 模板的 .ioc 自动备份，不属于新工程

# ③ 改名（§E.1 表里的 3 个文件）
mv <模板名>.ioc <新工程名>.ioc
#   .ioc 内部那 2 行是**元数据**（不是 Mcu.PinN 那种内部索引），手改风险低。
#   改完用 CubeMX 打开一次 + Generate Code，回读生成物确认没被吃掉。

# ④ 新历史（不继承模板的 commit）
git init && git add -A && git commit -m "chore: 从模板派生"

# ⑤ 验证
python Tools/fw.py build --clean-first && python Tools/fw.py test && python Tools/fw.py selftest
```

> ✅ **2026-09-26 实测**（两次：一次手工、一次走脚本）：把模板派生为 `derive_probe` /
> `probe_proj` 后，`build --clean-first` / `test` / `selftest` 全部通过，
> 产物随新工程名命名。**派生不需要改 `Tools/` 下任何脚本。**

> ⚠️ **示例业务先留着**：`op_blink`（心跳）与 `op_flash`（W25Q128 读写）不是残留，
> 是"分层怎么用"的活示范。要删得同步改两处 —— 顶层 `CMakeLists.txt` 的
> `operation` 源列表，与 `Test/CMakeLists.txt` 的 `OP_SRC` 及用例
> （那个文件刻意不用 GLOB，就是让"漏加"暴露出来）。

### E.3 换芯片（不只是改名）

除 §E.1 之外还要同步：

- [ ] 1. `STM32F407XX_FLASH.ld` —— MEMORY 段的容量与起址（文件名建议一并改）
- [ ] 2. 启动文件 —— 换系列要换 `startup_stm32f407xx.s`
- [ ] 3. `.ioc` —— 芯片型号（在 CubeMX 里改，**不要手写**）
- [ ] 4. `Core/Inc/board_config.h` —— 器件名 / 容量 / 引脚，**只抄值不抄代码**
- [ ] 5. `.vscode/launch.json` 的 `device` —— J-Link 器件名
- [ ] 6. `cmake/stm32cubemx/CMakeLists.txt` —— CubeMX 生成物，重新 Generate 即可
- [ ] 7. `python Tools/fw.py lint` 确认两边一致 → `build --clean-first`
- [ ] 8. ⚠️ **必须上板** —— 引脚 / 时钟改动 AI 无法替你判断接线

---

## 交付前的总检查

```bash
python Tools/fw.py build --clean-first     # 1. 零告警
python Tools/fw.py test                    # 2. PC 单测全过
python Tools/fw.py doccheck                # 3. 0 死链 + 0 陈旧标识符
python Tools/fw.py selftest                # 4. 门禁自证
python Tools/fw.py sizecheck               # 5. 体积不超线
```

需要上板的（探针不在线就如实标"未验证"，**不许用编译通过顶替**）：

```bash
python Tools/fw.py flash                   # 6. 烧录
python Tools/fw.py verify --duration 8     # 7. 闭环：看到 [AI_READY] ⇒ 退出码 0
```

三态标注（`AGENTS.md` §交付口径）：**已验证**（写出跑了哪条命令、结果如何）/
**未验证**（说明为什么没跑）/ **不适用**。
