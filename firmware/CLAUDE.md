<!-- 本文件由 rules/tools/agent_doors.py 生成（薄转发层）· 规则正文见 AGENTS.md 与 rules/ · 勿手改 -->
# CLAUDE.md — G2026 固件工程

> **[MUST] 开工前置（所有 agent 通用）**：动手前先读 `../../../rules/01_任务路由协议.md` ——
> 判任务类型 → 装载对应 skill → **先输出一行 `[ROUTE]` 声明**（用了哪些 skill、参考了哪些 products 文档）再开工。
> 未声明就干活 = 违反工作台协议，用户可直接打断要求补声明。
>
> **⛔ 硬闸门（机器拦，不靠自觉）**：改文件 / 跑命令前先跑
> `python rules/tools/preflight.py <操作类型>` 拿**开工证** —— 它会把该读的
> skill 条款**正文打印出来**（治“装了 skill 却没读到”）。没证去做改动动作，会被
> `.codebuddy/settings.json` 的 `PreToolUse` 钩子**直接拦下**（退出码 2），
> stderr 会告诉你该去读哪一节。声明格式随之多一个 `证=` 字段。

> **薄转发层，不含规则正文。**

本目录的规则唯一真相源是 [`AGENTS.md`](AGENTS.md)（本目录）；逐级向上到
[`rules/AGENTS.md`](../../../rules/AGENTS.md)（全流程手册 + 分册）。

只认 `CLAUDE.md` 的 agent（Claude Code / TraeCode 系）读到这里即转入 `AGENTS.md`
（或逐级上级的 `AGENTS.md`），再按其中的路由表干活。

AI 边界：**不烧录**（先与人确认）· **不下单** · 不 `git reset --hard` · 不擦片 · 不改 `.ioc` 引脚时钟前先问人。
