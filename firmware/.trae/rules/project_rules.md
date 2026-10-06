# Trae 项目规则 — G2026 固件工程

<!-- my_skills:project-door -->
[MUST] 开工前置：先读 skill 库的 `C:/Users/funiang/.ai-skills/ROUTE.md`，按其 §1 走四步
（拿证 → 判类 → 装 skill → 读参考），**先输出 `[ROUTE]` 声明**再干活。
拿证命令：`python C:/Users/funiang/.ai-skills/tools/preflight.py <任务类型>`。

## 本工程的状态落点

当前进度与门禁基线只认工程根的 `PROJECT.md`（字段定义见
`C:/Users/funiang/.ai-skills/framework/阶段状态.md`）。**只在跨阶段或改基线时写它。**
<!-- /my_skills:project-door -->

本目录的门指向 `../../../AGENTS.md`（上级入口）。全工作台规则唯一真相源 = 根 `AGENTS.md`（四区 + 技能路由）
与 `rules/AGENTS.md`（全流程手册 + 分册）；ZCode / WorkBuddy / CodeBuddy / Qoder 等原生读各级 `AGENTS.md`。
- 写/改代码：最懒可用解（ponytail 风格），完成前过工程内 `Tools/fw.py` 门禁
  （零告警只在 `--clean-first` 后算数；不许加 `-Wno-*` 消错）。
- AI 边界：**不烧录**（先与人确认）· **不下单** · 不 `git reset --hard` · 不擦片 · 不改 `.ioc` 引脚时钟前先问人。
- **本文件只指路，不复制规则。**

> 说明：`.trae/rules/` 是 Trae 唯一**不需要开关**就能自动生效的入口；项目根整目录加载，
> 子目录按需（对话提及该目录文件时）生效。若也要 Trae 读 `AGENTS.md`，需在
> 「设置 → 规则 → 导入设置」打开"**将 AGENTS.md 包含在上下文中**"。