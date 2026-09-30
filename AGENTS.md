<!-- 本文件由 rules/tools/agent_doors.py 生成（薄转发层）· 规则正文见 AGENTS.md 与 rules/ · 勿手改 -->
# AGENTS.md — 产品：G2026

> **[MUST] 开工前置（所有 agent 通用）**：动手前先读 `../../rules/01_任务路由协议.md` ——
> 判任务类型 → 装载对应 skill → **先输出一行 `[ROUTE]` 声明**（用了哪些 skill、参考了哪些 products 文档）再开工。
> 未声明就干活 = 违反工作台协议，用户可直接打断要求补声明。
>
> **⛔ 硬闸门（机器拦，不靠自觉）**：改文件 / 跑命令前先跑
> `python rules/tools/preflight.py <操作类型>` 拿**开工证** —— 它会把该读的
> skill 条款**正文打印出来**（治“装了 skill 却没读到”）。没证去做改动动作，会被
> `.codebuddy/settings.json` 的 `PreToolUse` 钩子**直接拦下**（退出码 2），
> stderr 会告诉你该去读哪一节。声明格式随之多一个 `证=` 字段。

> **薄转发层**：本文件只说明「这一层放什么、进来先读什么」，**规则正文不在这里**。
> 规则唯一真相源 = 根 `AGENTS.md`（四区 + 技能路由）与 `rules/AGENTS.md`（全流程手册 + 分册）。

## 本层放什么

正式产品 G2026：`firmware/`（固件）· `hardware/`（硬件）· `docs/`（文档）· `tools/`。

## 进来先做

1. 先读 `../../AGENTS.md`（上级入口），按其中的路由表进对应分册。
2. 写 / 改代码前先装载 `ponytail`（最懒可用解）；硬件活按根 `AGENTS.md` 的技能路由表加载对应 skill。
3. AI 边界：**不烧录**（先与人确认）· **不下单** · 不 `git reset --hard` · 不擦片 · 不改 `.ioc` 引脚时钟前先问人。

`firmware/` 是从 `projects/f407vgt6` 模板派生的四层工程，门禁命令见其 `Tools/fw.py`。
找资料先读 `docs/INDEX.md`（本产品的文档索引），别凭印象复述。

## 注意事项

- 本文件**只指路，不复制规则**；与上级文档冲突时，以上级为准。
- 改动本层结构（加/删子区）后，重跑 `rules/tools/agent_doors.py` 补门。
