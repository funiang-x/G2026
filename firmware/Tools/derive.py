#!/usr/bin/env python3
"""derive —— 从本模板派生一个新工程（复制 + 改名 + 新 git 历史）。

为什么需要它
------------
"派生"**不是任何工具的功能** —— CMake 只管"为一个已有源码目录生成构建系统"，
`cmake --help` 的选项里只有 `-S` / `-B` / `-G` / `-D` 这类，没有"新建/复制工程"。
cmake-gui 里同样只有 Configure / Generate 和一堆缓存变量。
派生本质是**复制文件夹 + 改几处字符串**，属于文件系统操作。

但"改几处"要动的地方散在 3 个文件里，漏一处不会立刻报错 ——
最典型的是 `Tools/selftest.py` 曾写死 `build/Debug/<模板名>.elf`：
派生改名后，构建明明是好的，体积门禁却报"原工程还没构建"（rc=6）。
那类**只在改名时才现形**的假失败，靠人记是记不住的 ⇒ 收敛进这个脚本。

⚠️ **模板名不硬编码** —— 从 `CMakeLists.txt` 的 `set(CMAKE_PROJECT_NAME ...)` 读。
   那是唯一权威：`.elf` / `.bin` / `.hex` / `.map` 名与 `board.env` 的
   `PROJECT_NAME` 全由它派生 ⇒ 改它一处，`fw.py` 的 flash / rtt / verify 自动跟着对。

⚠️ 本脚本**只做同板派生**（改名）。换芯片还要动链接脚本、启动文件、`.ioc` 型号、
   `board_config.h` 的容量与引脚 —— 清单见 docs/10 §E.3。

用法
----
    python Tools/derive.py <新工程名>                # 派生到模板的上一级目录
    python Tools/derive.py <新工程名> --dest <目录>   # 指定父目录
    python Tools/derive.py <新工程名> --verify       # 派生后立刻跑 build + test
    python Tools/derive.py <新工程名> --no-git       # 不建 git 仓库

退出码约定
----
    0  成功
    1  参数非法（工程名不合法 / 与模板同名 / --dest 不存在）
    2  用法错误：缺参数等（⚠️ argparse **固定**用 2，改不了 —— 所以下面避开 2）
    3  模板结构异常（读不到工程名、缺关键文件）
    4  复制或改名失败
    5  --verify 验证失败（build 或 test 没过）
    6  目标目录已存在（**绝不覆盖** —— 要重来请先自己删）
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# 复制时排除的东西：构建产物（大）、版本库、AI 记忆、.ioc 自动备份
# —— 与 Tools/selftest.py 的 COPY_IGNORE 同源，别写成第三份。
COPY_IGNORE = shutil.ignore_patterns(
    "build", ".git", ".workbuddy-ai", ".workbuddy",
    "__pycache__", "*.pyc", "_backup*", "*.ioc.bak_*",
)

# 工程名规则：字母开头，只含字母/数字/下划线。
# 它要直接进 CMake 目标名与文件名，所以按 C 标识符来约束。
NAME_RE = re.compile(r"^[A-Za-z][A-Za-z0-9_]*$")

# CMakeLists.txt 里工程名的唯一权威写法
PROJ_NAME_RE = re.compile(r"^set\(CMAKE_PROJECT_NAME\s+([A-Za-z0-9_]+)\s*\)", re.M)


def fail(code: int, msg: str) -> int:
    print(f"!! {msg}", file=sys.stderr)
    return code


def read_template_name() -> str | None:
    """模板名 = `CMakeLists.txt` 里 `set(CMAKE_PROJECT_NAME xxx)` 的 xxx。"""
    cm = ROOT / "CMakeLists.txt"
    if not cm.is_file():
        return None
    m = PROJ_NAME_RE.search(cm.read_text(encoding="utf-8", errors="replace"))
    return m.group(1) if m else None


def warn_dirty_template() -> None:
    """模板工作区有未提交改动时提醒 —— 派生复制的是**工作区状态**，不是某个 commit。"""
    try:
        p = subprocess.run(["git", "status", "--porcelain"], cwd=str(ROOT),
                           capture_output=True, text=True, timeout=15)
    except Exception:
        return
    if p.returncode == 0 and p.stdout.strip():
        n = len(p.stdout.strip().splitlines())
        print(f"⚠️  模板工作区有 {n} 个未提交改动 —— 派生复制的是**当前工作区状态**，")
        print("   不是某个 commit。要基于干净版本派生，请先在模板里提交。")
        print()


def rename_files(target: Path, old: str, new: str) -> str | None:
    """改 3 个文件里的工程名。返回 None 表示成功，否则返回错误说明。"""
    # ① CMakeLists.txt —— 唯一权威
    cm = target / "CMakeLists.txt"
    t = cm.read_text(encoding="utf-8")
    t2, n1 = PROJ_NAME_RE.subn(f"set(CMAKE_PROJECT_NAME {new})", t)
    if n1 != 1:
        return f"CMakeLists.txt 里 CMAKE_PROJECT_NAME 命中 {n1} 处（应为 1 处）"
    cm.write_text(t2, encoding="utf-8")
    print(f"      CMakeLists.txt           CMAKE_PROJECT_NAME → {new}")

    # ② VS Code 调试配置（漏了它，调试器会去找一个不存在的 elf）
    lj = target / ".vscode" / "launch.json"
    if lj.is_file():
        t = lj.read_text(encoding="utf-8")
        old_ref = f"/build/Debug/{old}.elf"
        n2 = t.count(old_ref)
        lj.write_text(t.replace(old_ref, f"/build/Debug/{new}.elf"), encoding="utf-8")
        print(f"      .vscode/launch.json      executable → {new}.elf（{n2} 处）")

    # ③ .ioc：文件名 + 内部 2 行元数据
    #    那两行是**元数据**（不是 Mcu.PinN 那种 CubeMX 内部索引），手改风险低。
    old_ioc = target / f"{old}.ioc"
    if old_ioc.is_file():
        t = old_ioc.read_text(encoding="utf-8")
        t = t.replace(f"ProjectManager.ProjectFileName={old}.ioc",
                      f"ProjectManager.ProjectFileName={new}.ioc")
        t = t.replace(f"ProjectManager.ProjectName={old}",
                      f"ProjectManager.ProjectName={new}")
        (target / f"{new}.ioc").write_text(t, encoding="utf-8")
        old_ioc.unlink()
        print(f"      {old}.ioc → {new}.ioc     （含内部 2 行元数据）")
    else:
        print(f"      ⚠️ 找不到 {old}.ioc —— 已跳过（CubeMX 重新生成时自己会建）")

    return None


# 扫残留时要跳过的目录：生成物 / 第三方 / 版本库
SCAN_SKIP_DIRS = ("build", ".git", "Drivers", "Middlewares", "Core", "cmake")
# 只扫这些后缀 —— 工程名只可能出现在手写文件里
SCAN_SUFFIXES = {".md", ".txt", ".json", ".py", ".ioc", ".h", ".c", ".cmake", ".ld", ".s"}


def report_leftovers(target: Path, old: str, limit: int = 12) -> None:
    """派生后扫一遍旧工程名的残留 —— **只报告，不自动改**。

    为什么不在改名阶段一并替换：残留里混着**不该动**的东西 ——
    `docs/archive/` 是归档方案，`Tools/selftest.py` 的注释在讲"曾经写死过什么"。
    自动替换会把这类**历史事实**改成语义错误的句子（"曾写死过 probe_proj.elf"
    —— 可它从没写死过）。所以只列清单，并且**分成两类**让人判断。

    分类规则：`docs/` 下的是历史叙述（实测记录 / 归档），其余是活引用
    （根目录自述、Tools/ 里的代码与 docstring）。
    ⚠️ 芯片型号 `STM32F407VGT6` 是**大写**，本函数只匹配小写工程名，天然避开。
    """
    live: list[str] = []   # 可能该改
    hist: list[str] = []   # 历史记录，通常不用改

    for p in sorted(target.rglob("*")):
        if not p.is_file() or p.suffix not in SCAN_SUFFIXES:
            continue
        rel = p.relative_to(target).as_posix()
        if rel.startswith(SCAN_SKIP_DIRS):
            continue
        try:
            text = p.read_text(encoding="utf-8", errors="ignore")
        except Exception:
            continue
        for i, line in enumerate(text.splitlines(), 1):
            if old not in line:
                continue
            item = f"     {rel}:{i}  {line.strip()[:60]}"
            (hist if rel.startswith("docs/") else live).append(item)

    total = len(live) + len(hist)
    if total == 0:
        print("   ✅ 没有残留 —— 旧工程名已全部替换")
        return

    print(f"   ⚠️ 旧名 `{old}` 仍出现在 {total} 处（脚本**不自动改**，请逐个判断）：")
    if live:
        print(f"\n   【可能该改】自述 / 命令 / 代码兜底（{len(live)} 处）")
        for h in live[:limit]:
            print(h)
        if len(live) > limit:
            print(f"     … 还有 {len(live) - limit} 处")
    if hist:
        print(f"\n   【历史记录】通常不用改（{len(hist)} 处）")
        for h in hist[:limit]:
            print(h)
        if len(hist) > limit:
            print(f"     … 还有 {len(hist) - limit} 处")
    print("\n   ⚠️ `STM32F407VGT6` 是**芯片型号**，任何情况下都不要动。")


def run_quiet(cmd: list[str], cwd: Path) -> int:
    """跑一条命令，输出直通终端。"""
    return subprocess.call(cmd, cwd=str(cwd))


def derive(new: str, dest_parent: Path, use_git: bool, verify: bool) -> int:
    old = read_template_name()
    if old is None:
        return fail(3, "读不到模板名：CMakeLists.txt 里没有 `set(CMAKE_PROJECT_NAME ...)`")
    if new == old:
        return fail(1, f"新工程名与模板名相同（{old}）—— 换个名字")
    if not NAME_RE.match(new):
        return fail(1, f"工程名不合法：{new}（要求字母开头，只含字母/数字/下划线）")

    # 关键文件缺了 ⇒ 这不是完整的模板，或者工作区不完整
    required = ["CMakeLists.txt", "CMakePresets.json", "Tools/fw.py", f"{old}.ioc"]
    missing = [f for f in required if not (ROOT / f).exists()]
    if missing:
        return fail(3, "模板缺关键文件：" + " · ".join(missing))

    target = dest_parent / new
    if target.exists():
        return fail(6, f"目标已存在，不覆盖：{target}\n"
                       "   要重来请先自己删除，或用 --dest 换个父目录")

    warn_dirty_template()
    print(f"模板：{ROOT}")
    print(f"  工程名 {old} → {new}")
    print(f"  新位置 {target}")
    print()

    print(f"[1/4] 复制（排除 build/ · .git/ · .workbuddy-ai/ · *.ioc.bak_*）")
    try:
        shutil.copytree(ROOT, target, ignore=COPY_IGNORE)
    except Exception as e:
        return fail(4, f"复制失败：{e}")
    print("      完成")

    print("[2/4] 改名")
    err = rename_files(target, old, new)
    if err:
        return fail(4, f"改名失败：{err}\n   已复制的目录保留在 {target}，请自行检查或删除")

    if use_git:
        print("[3/4] 建新 git 仓库（不继承模板历史）")
        steps = [
            ["git", "init", "-q"],
            ["git", "add", "-A"],
            ["git", "commit", "-q", "-m", f"chore: 从 {old} 模板派生"],
        ]
        for cmd in steps:
            rc = run_quiet(cmd, target)
            if rc != 0:
                print(f"      !! `{' '.join(cmd)}` 退出码 {rc} —— 目录已建好，"
                      "但 git 这一步没成，请手动补")
                break
        else:
            print("      git init + 首次提交完成")
    else:
        print("[3/4] 跳过 git（--no-git）")

    if verify:
        print("[4/4] 验证：configure + build --clean-first + test")
        steps = [
            ["cmake", "--preset", "Debug"],
            [sys.executable, "Tools/fw.py", "build", "--clean-first"],
            [sys.executable, "Tools/fw.py", "test"],
        ]
        for cmd in steps:
            rc = run_quiet(cmd, target)
            if rc != 0:
                return fail(5, f"验证失败：`{' '.join(cmd)}` 退出码 {rc}\n"
                               f"   工程已派生在 {target}，但没通过验证")
        print("      构建 / 单测通过")
    else:
        print("[4/4] 跳过验证（加 --verify 可派生后立刻跑一遍 build + test）")

    print()
    print(f"✅ 派生完成：{target}")
    print()
    print("[残留检查] 旧工程名在文档 / 注释里的出现位置")
    report_leftovers(target, old)
    print()
    print("   下一步：")
    print(f"     cd {target}")
    print("     python Tools/fw.py env                    # 体检：工具链 / 探针 / 各预设")
    print("     python Tools/fw.py build --clean-first")
    print()
    print("   ⚠️ 只有**同一块板子**才不用改硬件配置。换芯片见 docs/10 §E.3。")
    print("   ⚠️ .ioc 改动后建议用 CubeMX 打开一次 + Generate Code，回读生成物确认。")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(
        prog="derive.py",
        description="从本模板派生一个新工程（复制 + 改名 + 新 git 历史）",
    )
    ap.add_argument("name", help="新工程名（字母开头，只含字母/数字/下划线）")
    ap.add_argument("--dest", default=None,
                    help="新工程放在哪个父目录下（默认：模板的上一级目录）")
    ap.add_argument("--no-git", action="store_true", help="不建 git 仓库")
    ap.add_argument("--verify", action="store_true",
                    help="派生后立刻跑一遍 build + test（较慢，约 1 分钟）")
    args = ap.parse_args()

    dest_parent = Path(args.dest).resolve() if args.dest else ROOT.parent
    if not dest_parent.is_dir():
        return fail(1, f"--dest 不是已存在的目录：{dest_parent}")

    return derive(args.name, dest_parent, not args.no_git, args.verify)


if __name__ == "__main__":
    sys.exit(main())
