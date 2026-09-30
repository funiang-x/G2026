<!-- 本文件由 rules/tools/agent_doors.py 生成（薄转发层）· 规则正文见 AGENTS.md 与 rules/ · 勿手改 -->
# Trae 项目规则 — 产品：G2026

> **[MUST] 开工前置（所有 agent 通用）**：动手前先读 `rules/01_任务路由协议.md` ——
> 判任务类型 → 装载对应 skill → **先输出一行 `[ROUTE]` 声明**（用了哪些 skill、参考了哪些 products 文档）再开工。
> 未声明就干活 = 违反工作台协议，用户可直接打断要求补声明。
>
> **⛔ 硬闸门（机器拦，不靠自觉）**：改文件 / 跑命令前先跑
> `python rules/tools/preflight.py <操作类型>` 拿**开工证** —— 它会把该读的
> skill 条款**正文打印出来**（治“装了 skill 却没读到”）。没证去做改动动作，会被
> `.codebuddy/settings.json` 的 `PreToolUse` 钩子**直接拦下**（退出码 2），
> stderr 会告诉你该去读哪一节。声明格式随之多一个 `证=` 字段。

本目录的门指向 `../../AGENTS.md`（上级入口）。全工作台规则唯一真相源 = 根 `AGENTS.md`（四区 + 技能路由）
与 `rules/AGENTS.md`（全流程手册 + 分册）；ZCode / WorkBuddy / CodeBuddy / Qoder 等原生读各级 `AGENTS.md`。
- 写/改代码：最懒可用解（ponytail 风格），完成前过工程内 `Tools/fw.py` 门禁
  （零告警只在 `--clean-first` 后算数；不许加 `-Wno-*` 消错）。
- AI 边界：**不烧录**（先与人确认）· **不下单** · 不 `git reset --hard` · 不擦片 · 不改 `.ioc` 引脚时钟前先问人。
- **本文件只指路，不复制规则。**

> 说明：`.trae/rules/` 是 Trae 唯一**不需要开关**就能自动生效的入口；项目根整目录加载，
> 子目录按需（对话提及该目录文件时）生效。若也要 Trae 读 `AGENTS.md`，需在
> 「设置 → 规则 → 导入设置」打开"**将 AGENTS.md 包含在上下文中**"。
