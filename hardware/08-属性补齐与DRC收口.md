# G2026 底板 · 元件「值」补齐与 DRC 收口（2026-09-29）

> **本轮做了什么**：把原理图上 **41 个元件空着的「值」属性**从 **EDA 库器件**补齐，
> 并记录 DRC 改前/改后计数与**剩余项**。
>
> **工具**：`~/.zcode/skills/easyeda-schematic-net-fanout/scripts/eda_sch.py`（落图三件套）
> · `~/.zcode/skills/easyeda-viewer/scripts/viewer.py`（离线看图纸）
> · 事实依据见该 skill 的 `LESSONS.md` **T-21/T-22**（`lib_Device` 可用）。
>
> **三态口径**：以下均为 **已验证**（实测回读）；凡标 `[TBD]` / "待拍板"的为**未完成**。

---

## 1. 值从哪来（关键：不是猜的）

上一轮把这件事搁置，理由是"`lib_Device` 取不到库属性，值只能靠人确认"。
**本轮实测推翻了它**（skill 台账 T-21）：

| 调用 | 实测结果 |
|---|---|
| `lib_Device.search(name)` | ✅ 返回器件记录（uuid / libraryUuid / symbolName / footprintName） |
| `lib_Device.get([uuid])` | ✅ 返回器件全量，**`property.otherProperty` 就是权威属性表**（含 `Value`、`Tolerance`、`Supplier Part`…） |
| `lib_Device.getByLcscIds(["C17414"])` | ✅ 按立创料号查，同样带 `otherProperty` |
| `lib_Symbol.search(name)` | ✅ 返回 `description`（如 `"1KΩ (1001) ±1%"`），可作**兜底** |

⚠️ 两个坑：`get()` 必须传**数组** `get([uuid])`；**批量查库要分批**（一次 20+ 型号会失败，每批 ≤8 稳定）。

**判定"需要补"的口径**：器件的 `otherProperty` **有 `Value` 键**且值为空。
连接器 / IC / 测试点**根本没有 `Value` 键** → 不算空、DRC 也不报。

---

## 2. 补了什么（41 个，全部来自库）

### PWR-电源与背光（15）

| 位号 | 型号 | 值 | 位号 | 型号 | 值 |
|---|---|---|---|---|---|
| C16 | TAJB226K025RNJ | 22uF | R13 | 0805W8F1003T5E | 100kΩ |
| C17 | TAJB226K016RNJ | 22uF | R20 | 0805W8F1003T5E | 100kΩ |
| C18 | CC0805KRX7R9BB104 | 100nF | R15 | 0805W8F4701T5E | 4.7kΩ |
| C19 | CC0805KRX7R9BB104 | 100nF | R16 | 0805W8F4701T5E | 4.7kΩ |
| C20 | CC0805KRX7R9BB104 | 100nF | R17 | 0805W8F0000T5E | 0Ω |
| R11 | 0805W8F1001T5E | 1kΩ | R18 | 0805W8F0000T5E | 0Ω |
| R12 | 0805W8F1001T5E | 1kΩ | R19 | 0805W8F0000T5E | 0Ω |
| R14 | 1206W4F390JT5E | 39Ω | | | |

### AFE-前端模拟（26）

| 位号 | 型号 | 值 | 位号 | 型号 | 值 |
|---|---|---|---|---|---|
| R1 / R3 / R4 | 0805W8F499JT5E | 49.9Ω | C2 | 0805CG562J500NT | 5.6nF |
| R2 / R5 / R9 / R10 | 0805W8F1002T5E | 10kΩ | C4 / C6 | 0805CG103J500NT | 10nF |
| R6 | 0805W8F3002T5E | 30kΩ | C8 | 0805CG182J500NT | 1.8nF |
| R8 | 0805W8F1000T5E | 100Ω | C10 | CL10C471JB8NNNC | 470pF |
| JP1 / JP2 / JP3 | 0805W8F0000T5E | 0Ω | C11 | TAJB226K025RNJ | 22uF |
| L1 | XRCD54-4R7M | 4.7uH | C12 | TAJB226K016RNJ | 22uF |
| L3 / L5 | CD54-220M | 22uH | C13 | CL21A106KAYNNNE | 10uF ※ |
| L7 | XRCD54-120M | 12uH | C1 / C9 | 1206CG104J500NT | 100nF |

※ C13 库里 `otherProperty.Value` 为空，走 `lib_Symbol.search().description` 兜底得 `10uF`。

**与设计文档交叉核对（抽样）**：R6=30kΩ / R7=10kΩ ⇒ 同相增益 `1+30k/10k = 4`，
与 `02-architecture.md`「GAIN（OPA2197 半 B，同相×4）」**逐字吻合**；
R1/R3/R4 = 49.9Ω（与 `06-修正BOM与落图清单.md` 一致）。→ **值可信**。

---

## 3. DRC 改前 / 改后（`eda_sch.py drc --all`）

| 时点 | 全部 | 致命错误 | 错误 | 警告 | 信息 |
|---|---|---|---|---|---|
| 改前（基线） | 82 | 0 | 0 | 34 | 48 |
| **改后** | **41** | **0** | **0** | 34 | **7** |

**41 条「元件『值』属性内容为空」全部清零。** 致命错误 / 错误 始终为 0。

> 注：`sch_Drc.check` 是**整设计级**——三页分别跑得到**同一组计数**（新事实，台账 T-23）。

---

## 4. 剩余 DRC（**均待人拍板，本轮只诊断未动**）

### 4.1 警告 ×34

| 条数 | 消息 | 根因 | 为什么没动 |
|---|---|---|---|
| **31** | 网络图元不在原理图格点上（F1~F5 / +5V_A / AGND / +5V / GND / Q1_G） | **元件放置时引脚就不在 10 单位格点上**（三页共 81 个标签偏 1~7 单位，多数偏 5）。例：AFE 的 `F1` 端口 x=986，其引脚 x=976 | 挪标签只消警告、治不了本；挪元件会牵动其**全部引脚的连线**（高风险、可能引入新悬空）→ 需先定"要不要重排栅格" |
| **3** | 元件的属性与供应商编号不匹配，建议使用器件标准化（约 63 个器件） | 器件属性与库未同步 | 本机**未找到**触发标准化的 API（`lib_*`/`dmt_*`/`sys_*` 穷举无果）→ 需在 EDA「工具 → 器件标准化」**人工点一次** |

**给用户的选项（4.1 第 1 类）**：
- **A. 不动**：31 条是**布局规范类**警告，不影响电气正确性（悬空/短路类为 0）。可接受后继续。
- **B. 只挪标签**：把 81 个 netport/netflag 吸附到最近格点并同步改短线端点。
  消警告，但引脚仍离格点（治标），且需 `delete + createNetPort`（台账 T-19 有误删先例）→ 中风险。
- **C. 连元件一起挪到栅格**：治本，但要重排整页并重建受影响的连线 → **高风险**，建议在**副本工程**上先试。

### 4.2 信息 ×7

| 条数 | 消息 | 性质 |
|---|---|---|
| 2 | 元件 U1A / U1B 的位号不符合建议规则（应为"字母+数字"） | 多部件元件写法，需确认 EDA 期望形式 |
| 2 | OPA2197ID 多部件元件不完整（U1A / U1B 各自缺其他部件） | **元件级重做**：符号 B 单元复用了 1/2/3 脚，而封装有 5/6/7 无对应引脚 |
| 1 | OPA2197ID 引脚与焊盘未对应（焊盘没有对应引脚：5、6、7） | 同上 |
| 2 | 开始/完成设计规则检查（头尾行） | — |

---

## 5. 复现命令

```bash
PY="C:/Users/funiang/.workbuddy-ai/binaries/python/versions/3.13.12/python.exe"
SK="C:/Users/funiang/.zcode/skills/easyeda-schematic-net-fanout/scripts/eda_sch.py"
V="C:/Users/funiang/.zcode/skills/easyeda-viewer/scripts/viewer.py"

"$PY" "$SK" audit --all                                   # 只读体检
"$PY" "$SK" value --all                                   # 干跑看计划
"$PY" "$SK" value --all --apply                           # 写回（值取自库）
"$PY" "$SK" drc   --all --export ./drc_logs --baseline ./drc_base.json
"$PY" "$V"  dump  --all --out ./_view && "$PY" "$V" serve --dir ./_view --port 8931
```

**验收信号**：`value` 回读逐件一致；`drc` 信息计数 48 → 7；全量日志落盘可比对。

## 6. 留档

- DRC 全量日志与基线：`~/.workbuddy-ai/tmp/drc_logs/`、`~/.workbuddy-ai/tmp/drc_logs_after/`、`drc_baseline.json`（临时目录，需长期保留请移入本目录）
- 「值」写回计划备份：`%TEMP%/eda_value_backup/value_plan_20260929_130916.json`
- 离线图纸快照：`viewer.py dump` 产物（`.esch2` 即整页源码，可随时还原）
