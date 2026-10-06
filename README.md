# G2026 — 周期信号测量分析装置（电赛 G 题）

> 3.5″ 屏 8080 并口 + AFE 模拟前端 + STM32F407VGT6 核心板 + 自研底板的完整产品工程。
> **三态口径**：凡涉及"上板"的结论一律**未验证**（装置尚未制造），验证过的会注明实测日期与证据链接。

**进门先读**：[`AGENTS.md`](AGENTS.md)（AI 工作规则）→ [`docs/INDEX.md`](docs/INDEX.md)（全部文档的单一索引）→ [`PROJECT.md`](PROJECT.md)（阶段/待办/门禁基线）。

## 目录导览

| 位置 | 内容 | 规则 |
|---|---|---|
| `docs/` | 产品级方案与全链路索引（`INDEX.md` 是唯一入口） | 编号文档 + INDEX 登记 |
| `hardware/` | 硬件编号文档 01~50（需求→原理图→PCB→复核）+ BOM 真相源 `bom_final.csv` | 白名单制，见 AGENTS.md |
| `firmware/` | STM32 四层工程（CubeMX + CMake + FreeRTOS + LVGL），门禁 `firmware/Tools/fw.py` | 规范手册见 `firmware/docs/00~13` |
| `tools/` | 跨域工具（`tidy_workspace.py` 工作区归位、`verify_dsp.py` 数值复算、`sim/` 仿真环境） | 可复用工具的家 |
| `_work/` | AI 会话临时产物唯一落点（git 忽略） | 每周一 09:00 自动清扫 |
| `_archive/` | 历史证据冻结区（EDA 整包快照、早期评审脚本，文档引用已指向此处） | 只读 |

## 门禁（改完必跑）

- **固件**：`python firmware/Tools/fw.py verify`（env → lint → doccheck → test → sizecheck 一条龙；CubeMX 重生成后先读 `firmware/docs/11 §11`）。
- **EDA**：`easyeda sch check / sch nets / sch drc`（三页）+ `pcb drc`；基线数字记录在 `PROJECT.md`。
- **工作区**：`python tools/tidy_workspace.py`（默认 dry-run）。

## 文档规范（模板口径，[MUST]）

1. **编号**：新文档接续所在目录的最大编号（硬件 `hardware/51-…`，两位数字，永不复用）。
2. **文件名**：`NN-主题-YYYYMMDD.md`（日期=定稿日；可复用工具/长期维护的表可无日期，如 `04-pinout连接表.md`）。
3. **H1**：`# NN · 标题（YYYY-MM-DD）`；开头三行内写清**性质**（决策/审计/讲解/事故/拍板）与**状态**（已执行/待审/未验证）。
4. **登记**：写完必到 `docs/INDEX.md` 加一行（文档 | 内容 | 何时读）——没登记=不存在。
5. **引用即合同**：文档里引用的每个本地路径必须真实存在；引证据前先把它放进白名单或 `_archive/`。

## 如何派生新项目

> **模板母体 = `projects/templet`**（全项目模板：软件+硬件+文档+治理；本工程是它的派生实例）。

**一条命令**（默认 dry-run 先看方案，`--go` 执行）：

```bash
python tools/derive_project.py <新项目名> --dest <父目录> --go
```

自动完成：复制 AGENTS/CLAUDE/.gitignore/tidy_workspace.py/.vscode（按新名改写）、生成 README/PROJECT/INDEX 最小骨架、建 `_work/_archive/hardware` 空目录，并打印三步手工清单（固件模板派生 / 重铺项目门 skillman doors / git init）。
之后：写第一篇 `hardware/01-需求冻结-<日期>.md`，登记 INDEX，开始走编号文档流程。本工程的约定演进后（改 AGENTS/工具），**下次派生自动带上**——无需维护另一份模板。

## 历史与恢复

固件迁移/CubeMX 治愈的完整记录见 [`_archive/README-状态与恢复-20260928.md`]（原 README，历史留档）；
EDA 整包回退点见 [`PROJECT.md`](PROJECT.md) 的「回退点」节。
