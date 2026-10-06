# CLAUDE.md — G2026 固件工程

<!-- my_skills:project-door -->
[MUST] 开工前置：先读 skill 库的 `C:/Users/funiang/.ai-skills/ROUTE.md`，按其 §1 走四步
（拿证 → 判类 → 装 skill → 读参考），**先输出 `[ROUTE]` 声明**再干活。
拿证命令：`python C:/Users/funiang/.ai-skills/tools/preflight.py <任务类型>`。

## 本工程的状态落点

当前进度与门禁基线只认工程根的 `PROJECT.md`（字段定义见
`C:/Users/funiang/.ai-skills/framework/阶段状态.md`）。**只在跨阶段或改基线时写它。**
<!-- /my_skills:project-door -->

> **薄转发层，不含规则正文。**

本目录的规则唯一真相源是 [`AGENTS.md`](AGENTS.md)（本目录）；逐级向上到
[`rules/AGENTS.md`](../../../rules/AGENTS.md)（全流程手册 + 分册）。

只认 `CLAUDE.md` 的 agent（Claude Code / TraeCode 系）读到这里即转入 `AGENTS.md`
（或逐级上级的 `AGENTS.md`），再按其中的路由表干活。

AI 边界：**不烧录**（先与人确认）· **不下单** · 不 `git reset --hard` · 不擦片 · 不改 `.ioc` 引脚时钟前先问人。