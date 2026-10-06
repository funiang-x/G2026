#!/usr/bin/env python3
"""从 G2026 模板派生新项目：一条命令复制骨架（约定文件 + 工具 + 排除配置 + 空目录）。

用法:
  python tools/derive_project.py <新项目名>                  # dry-run，只打印方案
  python tools/derive_project.py <新项目名> --go             # 执行
  python tools/derive_project.py <新项目名> --dest D:/prj    # 指定父目录（默认与本工程同级）

随行内容: AGENTS.md / CLAUDE.md / .gitignore / tools/tidy_workspace.py / .vscode(settings)
          ——全部按新名改写"G2026"字样后复制（约定随本模板的演进而更新）。
内置生成: README.md / PROJECT.md / docs/INDEX.md 最小骨架（产品内容从零开始）。
空目录:   _work/ _archive/ hardware/（+ data/ 首篇文档需要时再建）。
固件不随行: STM32 工程用 skill 库模板派生（见执行后打印的清单），非 STM32 删掉该步骤。
"""
import shutil, sys
from pathlib import Path

SRC = Path(__file__).resolve().parents[1]          # 本模板工程根
COPY = ["AGENTS.md", "CLAUDE.md", ".gitignore", "tools/tidy_workspace.py", ".vscode/settings.json"]

README_T = """# {NAME} — <一句话产品定义>

> **三态口径**：凡涉及"上板/实测"的结论一律**未验证**，验证过的注明实测日期与证据链接。

**进门先读**：[`AGENTS.md`](AGENTS.md)（AI 工作规则）→ [`docs/INDEX.md`](docs/INDEX.md)（唯一文档索引）→ [`PROJECT.md`](PROJECT.md)（阶段/待办/门禁基线）。

## 目录导览

| 位置 | 内容 | 规则 |
|---|---|---|
| `docs/` | 产品级方案与全链路索引 | 编号文档 + INDEX 登记 |
| `hardware/` | 硬件编号文档 + BOM 真相源 + `data/`（被引用数据） | 白名单制，见 AGENTS.md |
| `firmware/` | 固件工程（STM32 模板派生后在此） | 规范手册见 `firmware/docs/00~13` |
| `tools/` | 跨域工具（tidy_workspace.py 等） | 可复用工具的家 |
| `_work/` | AI 会话临时产物唯一落点（git 忽略） | 每周自动清扫，14 天过期 |
| `_archive/` | 历史证据冻结区（git 忽略） | 清单见 `_archive/MANIFEST.md` |

## 门禁（改完必跑）

- 固件：`python firmware/Tools/fw.py verify`
- 工作区：`python tools/tidy_workspace.py`（默认 dry-run）
- EDA：按所用工具的检查命令，基线数字记录在 `PROJECT.md`

## 文档规范（[MUST]）

1. **编号**：新文档接续所在目录最大编号，永不复用。
2. **文件名**：`NN-主题-YYYYMMDD.md`（长期维护的表/工具可无日期）。
3. **H1**：`# NN · 标题（YYYY-MM-DD）`；开头写清**性质**（决策/审计/讲解/事故/拍板）与**状态**。
4. **登记**：写完必到 `docs/INDEX.md` 加一行——没登记=不存在。
5. **引用即合同**：文档引用的本地路径必须真实存在。
"""

PROJECT_T = """# {NAME}

阶段: ① 需求（约束冻结）

交付物: （尚未产出）

门禁基线: （首次跑门禁后回填数字与日期）

待办: 1. 需求冻结——第一篇编号文档（`hardware/01-…` 或 `docs/01-…`），写完登记 `docs/INDEX.md`

回退点: git 初始提交

下一步判据: 01 号需求文档定稿并登记 INDEX
"""

INDEX_T = """# {NAME} · docs 索引

> **给 agent 的入口**：接 {NAME} 的活，**先读本文件**再挑具体文档——按"何时读"对上号，
> 然后**打开原文读**。新增文档必在本表登记一行（文档 | 内容 | 何时读）。

| 文档 | 内容 | 何时读 |
|---|---|---|
| （第一篇 01 写完后登记在这里） | | |

## 与工程文件的边界（别搞混）

| 类别 | 真相源 |
|---|---|
| （按项目补：硬件事实=… / 应用参数=… / 数值复算=…） | |
"""


def main():
    args = sys.argv[1:]
    go = "--go" in args
    args = [a for a in args if a != "--go"]
    dest = Path(args[args.index("--dest") + 1]) if "--dest" in args else SRC.parent
    name = next((a for a in args if not a.startswith("--")), "")
    if not name:
        print(__doc__)
        return
    root = dest / name
    print(f"[{'APPLY' if go else 'DRY-RUN'}] 派生 {name} → {root}")
    plan = []
    for f in COPY:
        plan.append((f, (root / f), True))                      # 复制并替换名字
    for f, text in (("README.md", README_T), ("PROJECT.md", PROJECT_T)):
        plan.append((f, (root / f), text))                      # 内置模板
    plan.append(("docs/INDEX.md", root / "docs/INDEX.md", INDEX_T))
    for src, dst, t in plan:
        kind = "改写复制" if t is True else "内置模板"
        print(f"  {kind}  {dst}")
    print("  空目录  _work/ _archive/ hardware/")
    if not go:
        print("(dry-run，加 --go 执行)")
        return
    if root.exists():
        print(f"⛔ {root} 已存在，停止")
        return
    for src, dst, t in plan:
        dst.parent.mkdir(parents=True, exist_ok=True)
        if t is True:
            text = (SRC / src).read_text(encoding="utf-8").replace("G2026", name)
            dst.write_text(text, encoding="utf-8")
        else:
            dst.write_text(t.replace("{NAME}", name), encoding="utf-8")
    for d in ("_work", "_archive", "hardware"):
        (root / d).mkdir(parents=True, exist_ok=True)
    print(f"完成。接下来（手工三件）：")
    print(f"  1. 固件：STM32 用 skill 库模板派生 →")
    print(f"     python C:/Users/funiang/Desktop/Project/projects/templet/firmware/Tools/derive.py {name}-fw --dest \"{root}/firmware\" --verify")
    print(f"     （母体=projects/templet；整项目派生含固件用 ../templet/tools/derive_project.py）")
    print(f"  2. 重铺项目门：python C:/Users/funiang/.ai-skills/tools/skillman.py doors \"{root}\" --apply")
    print(f"  3. cd \"{root}\" && git init && git add -A && git commit -m \"初始骨架（派生自 G2026 模板）\"")
    print(f"  然后写第一篇 hardware/01-需求冻结-<日期>.md 并登记 docs/INDEX.md")


if __name__ == "__main__":
    main()
