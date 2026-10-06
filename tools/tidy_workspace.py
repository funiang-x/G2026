#!/usr/bin/env python3
"""G2026 工作区归位器——AI 会话散落文件的自动归类（约定见 AGENTS.md「AI 工作产物落点」）。

用法:
  python tools/tidy_workspace.py            # dry-run，只打印打算做什么
  python tools/tidy_workspace.py --apply    # 执行（move 不 delete，垃圾除外）

规则:
  删除: __pycache__/ 与 *.pyc（可再生）；_work/ 内超过 14 天的旧产物（生产完即清理）
  归档: *bak* → _archive/（根目录与 hardware/）
  归类: hardware/ 白名单(bom_final.csv)与编号文档之外的一切散件 → _work/hardware/<类型>/
  冻结证据区(永不动): 根 _archive/（含 _review3~7/_eda/hardware-evidence 与全部历史件）
  每次执行后重生成 _archive/MANIFEST.md（证据区内容清单，给 AI 和人查）
  白名单: 编号文档(^[0-9]{2}-) bom_final.csv tools/ firmware/ docs/ 根 *.md .git .ai-skills-state
  保护: 48h 内修改过的文件跳过（防打断进行中的 AI 会话）
  日志: 每次执行写 _work/tidy-log/<时间>.log
"""
import re, shutil, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GARD = 48 * 3600
WORK_EXPIRE = 14 * 24 * 3600   # _work/ 产物保留期：生产完即清理
NOW = time.strftime("%Y%m%d-%H%M")
ARC_PURPOSE = {
    "_eda": "EDA 会话工作区：探针脚本 / 页面快照(.esch2) / 证据 JSON / 整包快照(.epro2，zip 可解包、IMAGE/*.webp 为页面渲染图)",
    "_review": "原理图源码快照（final-20260929、final-20260930 等改图前后对照）",
    "_review3": "评审期脚本与 EDA 快照（compile_all.py 沙箱构建入口在此）",
    "_review4": "评审期脚本与证据（resolve_bom.py、信号流图册等）",
    "_review5": "评审期脚本与 EDA 快照（gen_pcb_layout.py、J5fix 回退包等）",
    "_review6": "45~48 号讲解文档的生成脚本与数据",
    "_review7": "评审期脚本与快照",
    "_view": "图纸渲染快照（历史）", "_view2": "图纸渲染快照（历史）",
    "_view3": "图纸渲染快照（49 号 AFE_flow.png 被文档引用）",
    "hardware-evidence": "早期硬件工作脚本与中间产物（05~13 号文档按名引用）",
    "hardware": "hardware/ 门文件的 .bak 归档",
    "firmware": "firmware/ 门文件的 .bak 归档",
}
# hardware/ 白名单（2026-10-06 起只留编号文档 + BOM；被引用数据在 data/ 目录、工具在 tools/hardware/、
# 历史证据在 _archive/——若未来新文档引用了 hardware/ 下的新散件，把文件名加回 HW_WHITELIST）
HW_WHITELIST = {"bom_final.csv"}
HW_FROZEN = set()
ROOT_FROZEN_PREFIX = ("_archive", "_work", "_eda", "_review")


def is_old(p: Path) -> bool:
    return time.time() - p.stat().st_mtime > GARD


def plan():
    moves, deletes = [], []
    # 根目录：只有 *.bak* 散文件归档
    for p in ROOT.iterdir():
        if p.name.startswith(ROOT_FROZEN_PREFIX) or p.name.startswith("."):
            continue
        if p.is_file() and re.search(r"bak", p.name, re.I) and is_old(p):
            moves.append((p, ROOT / "_archive" / p.name))
    # hardware/：白名单制——编号文档 + 白名单件留下，其余散件一律归类
    hw = ROOT / "hardware"
    for p in hw.iterdir():
        if p.name in HW_FROZEN or p.name in HW_WHITELIST:
            continue
        if re.match(r"^[0-9]{2}-", p.name) or p.name == "bom_final.csv":
            continue
        if not p.is_file() or not is_old(p):
            continue
        if re.search(r"bak", p.name, re.I):
            moves.append((p, ROOT / "_archive" / "hardware" / p.name))
        else:
            ext = p.suffix.lstrip(".") or "misc"
            moves.append((p, ROOT / "_work" / "hardware" / ext / p.name))
    # 垃圾：__pycache__ 与 *.pyc（firmware/docs/.git 除外）
    for cache in ROOT.rglob("__pycache__"):
        parts = set(cache.parts)
        if parts & {".git", "firmware", "docs", "node_modules", "_archive", "_work"}:
            continue
        if is_old(cache):
            deletes.append(cache)
    # _work/ 过期产物：生产完即清理（tidy-log 永久保留）
    work = ROOT / "_work"
    if work.exists():
        for p in work.rglob("*"):
            if not p.is_file() or "tidy-log" in p.parts:
                continue
            if time.time() - p.stat().st_mtime > WORK_EXPIRE:
                deletes.append(p)
    # 自检：白名单绝不出现在搬运清单
    for src, _ in moves:
        assert not re.match(r"^[0-9]{2}-", src.name), f"白名单被碰: {src}"
        assert "bom_final.csv" != src.name, f"白名单被碰: {src}"
    return moves, deletes


def write_manifest():
    """重生成 _archive/MANIFEST.md——给 AI 和人的证据区查阅入口（每次 --apply 后更新）。"""
    arc = ROOT / "_archive"
    if not arc.exists():
        return
    lines = [
        "# _archive 内容清单",
        "",
        "> `tools/tidy_workspace.py` 自动生成，每次运行后更新——勿手编。",
        "> 本目录被 git 忽略：`rg` 默认搜不到，搜证据加 `--no-ignore` 或按本清单路径直接取。",
        "",
    ]
    for d in sorted(arc.iterdir()):
        if d.is_dir():
            files = [p for p in d.rglob("*") if p.is_file()]
            size = sum(p.stat().st_size for p in files) / 1e6
            lines.append(f"- `{d.name}/` — {ARC_PURPOSE.get(d.name, '归档')}（{len(files)} 文件 · {size:.0f} MB）")
        else:
            lines.append(f"- `{d.name}` — {ARC_PURPOSE.get(d.name, '归档文件')}")
    (arc / "MANIFEST.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def run(apply: bool):
    moves, deletes = plan()
    tag = "APPLY" if apply else "DRY-RUN"
    print(f"[{tag}] 搬运 {len(moves)} 项，删除垃圾 {len(deletes)} 处")
    for src, dst in moves:
        print(f"  搬 {src.relative_to(ROOT)}  →  {dst.relative_to(ROOT)}")
    for d in deletes:
        print(f"  删 {d.relative_to(ROOT)}/")
    if not apply:
        print("(dry-run，加 --apply 执行)")
        return
    log = []
    for src, dst in moves:
        dst.parent.mkdir(parents=True, exist_ok=True)
        final = dst
        i = 1
        while final.exists():
            final = dst.with_name(f"{dst.stem}-{i}{dst.suffix}")
            i += 1
        shutil.move(str(src), str(final))
        log.append(f"MOVE {src.relative_to(ROOT)} -> {final.relative_to(ROOT)}")
    for d in deletes:
        if d.is_dir():
            shutil.rmtree(d)
        else:
            d.unlink()
        log.append(f"DELETE {d.relative_to(ROOT)}{'/' if d.is_dir() else ''}")
    logdir = ROOT / "_work" / "tidy-log"
    logdir.mkdir(parents=True, exist_ok=True)
    (logdir / f"{NOW}.log").write_text("\n".join(log) or "(nothing)", encoding="utf-8")
    write_manifest()
    print(f"完成：搬 {len(moves)}、删 {len(deletes)}；日志 _work/tidy-log/{NOW}.log；清单 _archive/MANIFEST.md 已更新")


if __name__ == "__main__":
    run("--apply" in sys.argv)
