# Middlewares/RTT —— SEGGER RTT 组件（已 vendor 进仓库）

## 这是什么

SEGGER 的 **RTT（Real Time Transfer）**：通过调试探针（J-Link）的内存访问能力，
在**不占用任何 UART** 的前提下做实时日志/双向通信。本工程把它当作日志出口的
默认后端（`LOG_BACKEND=RTT`，见 `BSP/Src/bsp_log.c`）。

## 为什么放在这里（而不是外部路径引用）

历史上 RTT 是通过 CMake 变量 `RTT_ROOT` **指向仓库外的只读目录**引用的。
这个设计**已经失败过一次**：那个外部目录被删除后，整个工程连
`cmake --preset` 都跑不起来（`ninja: error: '.../SEGGER_RTT.c' missing`）。

改成本目录 vendor 后的理由：

| 理由 | 说明 |
|---|---|
| **体积极小** | 4 个文件、约 124 KB。对比 LVGL（上千文件）、FreeRTOS（上百文件），代价可忽略 |
| **极其稳定** | RTT 协议多年未变，SEGGER 发布节奏远低于 HAL（后者要跟硅片 errata）。vendor 不会"过期" |
| **消除失效路径** | 仓库内文件不可能因为"外部目录被删"而消失。构建从此自包含 |
| **与既有约定一致** | `Middlewares/lvgl/`、`Middlewares/FreeRTOS/` 本来就是 vendor 的。RTT 同属"第三方只读库"这一类别 |
| **概念上不属于 Drivers/** | `Drivers/` 是芯片厂商的 HAL/CMSIS（跟芯片强绑定，由 `Tools/fetch_deps.py` 拉取）。RTT 是**芯片无关**的调试传输库，放 `Drivers/` 是概念错位 |

**保留 `-DRTT_ROOT=...` 覆盖能力**：`CMakeLists.txt` 仍支持
`-DRTT_ROOT=<其它目录>` 或环境变量 `RTT_ROOT`，方便你在别的工程里复用同一份源码。
默认值现在指向本目录，**开箱即用，无需任何外部设置**。

## 文件清单

| 文件 | 归属 | 能否改 |
|---|---|---|
| `SEGGER_RTT.c` / `SEGGER_RTT.h` | SEGGER 上游 | ❌ **不要改**（SEGGER 明确建议不要修改源码，否则可能与 J-Link 协议不兼容） |
| `SEGGER_RTT_ConfDefaults.h` | SEGGER 上游默认值 | ❌ 不改（改配置请用下面那个） |
| **`SEGGER_RTT_Conf.h`** | **本项目定制** | ✅ **要改配置就改这里** |

## 本项目对 `SEGGER_RTT_Conf.h` 的定制

```c
#define SEGGER_RTT_MAX_NUM_UP_BUFFERS     (3)
#define SEGGER_RTT_MAX_NUM_DOWN_BUFFERS   (3)
```

通道规划见 `BSP/Inc/rtt_channels.h`：

| 通道 | 用途 |
|---|---|
| 0 | **日志**：系统日志 + AI 闭环标记（`[AI_READY]` / `[AI_FAIL]`） |
| 1 | **数据**：二进制/波形/传感器数据，与日志分开以免刷屏干扰标记检测 |
| 2 | **命令**：上位机或 AI 下发的命令与应答 |

用到索引 2 ⇒ 上下行缓冲数都必须 ≥ 3。这里**显式钉住**而不是依赖默认值，
防止将来换 RTT 版本时默认值变化导致通道 2 静默失效。

## 来源与版本

- 上游：<https://github.com/SEGGERMicro/RTT>
- 版权：`(c) SEGGER Microcontroller GmbH`（源文件头部保留，符合其许可要求）
- 导入日期：2026-09-11
- 导入时的文件哈希见下（便于将来比对是否需要更新）：

```
51f8969b91694d322bfe6fd0a4a5015e67e89eb46b3cbf6d3a22095cfcb58a50  SEGGER_RTT.c
fe21e9618410807676e5e964ea76da11a1744d20b5c0125afc24f1474b09c595  SEGGER_RTT.h
f2f1e3a67bf139ba056860a745cf635d029e51f62be6d208f89412d25ea434a4  SEGGER_RTT_ConfDefaults.h
```

（`SEGGER_RTT_Conf.h` 是项目定制的，不记录上游哈希。）

## 如何更新

RTT 极少需要更新。若确需更新：

```bash
# 1. 从上游取新版（保持同样的 4 个文件）
# 2. 覆盖 SEGGER_RTT.c / .h / _ConfDefaults.h
# 3. ⚠️ 不要覆盖 SEGGER_RTT_Conf.h —— 那是我们定制的
# 4. 重新构建验证
python Tools/fw.py build
```

## 相关文件

- `BSP/Src/bsp_log.c` —— RTT 后端的实际使用者
- `BSP/Inc/rtt_channels.h` —— 通道号定义
- `CMakeLists.txt` 的 `segger_rtt` target —— 独立编译单元，只给 `-Wall`，
  不套 `proj_warnings`（第三方代码在 `-Wextra` 下全是噪音）
