# AGENTS.md — 产品：G2026

<!-- my_skills:project-door -->
[MUST] 开工前置：先读 skill 库的 `C:/Users/funiang/.ai-skills/ROUTE.md`，按其 §1 走四步
（拿证 → 判类 → 装 skill → 读参考），**先输出 `[ROUTE]` 声明**再干活。
拿证命令：`python C:/Users/funiang/.ai-skills/tools/preflight.py <任务类型>`。

## 本工程的状态落点

当前进度与门禁基线只认工程根的 `PROJECT.md`（字段定义见
`C:/Users/funiang/.ai-skills/framework/阶段状态.md`）。**只在跨阶段或改基线时写它。**
<!-- /my_skills:project-door -->

> **薄转发层**：本文件只说明「这一层放什么、进来先读什么」，**规则正文不在这里**。
> 规则唯一真相源 = 根 `AGENTS.md`（四区 + 技能路由）与 `rules/AGENTS.md`（全流程手册 + 分册）。

## 本层放什么

正式产品 G2026：`firmware/`（固件）· `hardware/`（硬件）· `docs/`（文档）· `tools/`。

## 进来先做

1. 先读 `../../AGENTS.md`（上级入口），按其中的路由表进对应分册。
2. 写 / 改代码前先装载 `ponytail`（最懒可用解）；硬件活按根 `AGENTS.md` 的技能路由表加载对应 skill。
3. AI 边界：**不烧录**（先与人确认）· **不下单** · 不 `git reset --hard` · 不擦片 · 不改 `.ioc` 引脚时钟前先问人。

## AI 工作产物落点（[MUST]，2026-10-05 定）

- 会话临时产物（dump / 探针脚本 / JSON 证据 / 备份 / 缓存）**只许落 `<工程根>/_work/<任务名>/`**；
  禁止散落到根目录、`hardware/`、`docs/`、`firmware/`（历史教训：hardware/ 曾积累 120+ 散件）。
- 只有**定稿结论**才写成带编号文档进 `docs/` 或 `hardware/`，并到 `docs/INDEX.md` 登记。
- 文档口径（模板规范）：新文档编号**接续所在目录最大号**；文件名 `NN-主题-YYYYMMDD.md`；
  H1 `# NN · 标题（日期）`；开头写清性质与状态；**引用的本地路径必须真实存在**（引用即合同）。
- `hardware/` 只放三样：**编号文档**（含配图目录）、**`bom_final.csv`**、**`data/`**（被文档引用的
  数据/抽取/报告件）。可复用工具写 `tools/hardware/`；历史证据进 `_archive/`；其余任何新散件都会被
  自动归位到 `_work/hardware/`（机制见 `tools/tidy_workspace.py`）。
- 历史证据区**整体收进 `_archive/`**（`_review3~7`、`_eda`、`hardware-evidence`、`_review`、`_view*`、
  `firmware`，2026-10-05/06 起）——**冻结区**，编号文档引用其路径（已批量改写
  指向 `_archive/`），只读、不再新增。
- **`_archive/` 被 git 忽略，`rg` 默认搜不到里面的证据**——搜证据加 `--no-ignore`，或先读
  `_archive/MANIFEST.md`（归位器每次运行自动重生成）。格式可读性实测：`.epro2`=zip 可解包
  （`IMAGE/*.webp` 页面渲染图 AI 可直接看）、`.esch2`/`.log`/`.ioc`=文本、PDF 用分页读；
  Altium 二进制是唯一读不了的（其信息已抽取进 `04 §8`，无残留依赖）。
  指向 `_archive/`），只读、不再新增。
- 存量归位与每周自动清扫：`python tools/tidy_workspace.py`（默认 dry-run，`--apply` 执行）。

`firmware/` 派生自全项目模板 `projects/templet`（软件+硬件+文档骨架，含工作区治理）；
门禁命令见其 `Tools/fw.py`。整项目派生：`python ../templet/tools/derive_project.py <新名> --go`。
找资料先读 `docs/INDEX.md`（本产品的文档索引），别凭印象复述。

## 注意事项

- 本文件**只指路，不复制规则**；与上级文档冲突时，以上级为准。
- 改动本层结构（加/删子区）后，重跑 `rules/tools/agent_doors.py` 补门。