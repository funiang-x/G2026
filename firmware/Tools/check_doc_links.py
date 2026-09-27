#!/usr/bin/env python3
"""check_doc_links —— 文档与代码的一致性检查（两类）。

为什么必须靠工具
----------------
骨架（`AGENTS.md`）为了省行数，用**短引用** `docs/01` 而不是
`docs/01_工程分层架构.md`。`docs/01` **不是合法路径** —— 肉眼看不出对错。
而死链是**无声的**：没有报错、不影响构建，直到某次 AI 按错的路子干了活。

两类检查
--------
**① 路径引用**（扫反引号 + 围栏代码块里的命令行）
  1. 反引号里的路径 —— `` `docs/01` ``、`` `Tools/fw.py` ``、`` `Core/Src/main.c` ``；
  2. 围栏代码块内的命令行 —— 形如 `python Tools/fw.py build`。
     ⚠️ 只扫反引号时，"删了脚本但文档还教人跑它"这类死链**不会被发现**。

**② C 标识符对账**（工程自有前缀的标识符必须在代码里真的存在）
  文档是在**实现定型之前**写的，定型后函数名/宏名会变。这类不一致**没有任何
  报错**：固件照编、门禁全绿，直到 AI 照文档写出一个不存在的函数名才炸。
  实测：一次对账抓出 6 处（`app_rtos_start` 实际叫 `app_rtos_run`、
  `bsp_log_init` 参数表不符、`new_peripheral.py` 根本没实现、
  "`Operation/Src/` 为空骨架"这句在实现后已经不成立…）。
  ⚠️ 只查**名字是否存在**，不查签名/参数个数 —— 后者做不可靠。
     所以 `bsp_log_init(a, b)` 这种"名字对、参数错"的写法它拦不住。

判定规则
--------
· `docs/NN`（两位数字）→ 去匹配 `docs/NN*.md`，**必须恰好一个**。
  ⇒ 由此带来一条约束：**同编号前缀的文档不能有两个**（否则指向不确定）。
· 含 `/` 且以已知工程根开头（`Core/` `Drivers/` `Device/` `Task/` `Operation/`
  `Common/` `Test/` `Tools/` `Middlewares/` `cmake/` `docs/`）→ 路径必须存在。
· 支持 `*` 通配（如 `docs/0x_*.md`）。
· 以 `/` 结尾的视为目录，也必须存在。
· 工程自有前缀的标识符（`op_` `bsp_` `app_` `test_` `LOG_` `APP_` `BOARD_`）→
  必须在代码里出现过。
· 其它（不含 `/` 的裸文件名、URL、`<占位符>`）→ 跳过：它们不是路径声明。
· **同一行含 `doclink-ignore` 则整行跳过** —— 给"必须提到已删除名字"的场合用：
  变更记录、迁移说明、命名规范里的示例名。这些地方写旧名字是**正确的**，
  不写反而看不懂改了什么。
  ⚠️ 用它来掩盖真问题 = 把门禁拆了。它只豁免**同一行**，不豁免相邻行。

用法
----
    python Tools/check_doc_links.py            # 扫全部
    python Tools/check_doc_links.py --verbose  # 连"检查了哪些引用"一起打
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# 参与扫描的文件：骨架 + 人读的 README + docs 下的规则文档
SCAN_GLOBS = ("AGENTS.md", "README.md", "docs/*.md")

# 只有以这些工程根开头的引用才当成"路径声明"，其余一律跳过。
# 为什么：反引号里还大量出现 `now_ms`、`bsp_uart`、`-Wall` 这类**不是路径**的东西，
# 一刀切地"含 / 就当路径"会造出一堆假阳性，最后没人看报告。
PATH_ROOTS = ("Core/", "Drivers/", "Device/", "Task/", "Operation/", "Common/",
              "Test/", "Tools/", "Middlewares/", "cmake/", "docs/", "build/")

# 这些引用**允许不存在**：它们是"运行时/按需生成"的目录，不存在是正常状态。
# ⚠️ 不排除它们的话，`fw.py clean` 之后跑 doccheck 会凭空报一堆死链 ——
#    而"清干净了反而报错"会让人不再信这个门禁。
# ⚠️ 判据是**前缀**（`build/` 与它下面的一切），不是精确相等 ——
#    写成精确相等时 `build/Debug/` 会被漏掉。这个缺口是 2026-09-21 由
#    selftest 的**正向用例**当场抓到的（负向用例抓不到假阳性）。
OPTIONAL_PREFIXES = ("build/",)

SHORT_REF = re.compile(r"^docs/(\d{2})$")
# `docs/0x` / `docs/0x_*.md`：`x` 是"任意编号"的占位符，展开成正则再匹配
SHORT_REF_X = re.compile(r"^docs/([0-9x]{2})(_?\*?\.md)?$")
# 形如 `docs/01 §强制表` / `Tools/fw.py` / `Core/Src/main.c`（后面可能跟中文说明）
CODE_SPAN = re.compile(r"`([^`\n]+)`")
FENCE = re.compile(r"^\s*(```|~~~)")
# 代码块里的命令行：抓 `python Tools/xxx.py` / `Tools/xxx.py`
CMD_PATH = re.compile(r"(?:^|[\s\"'])((?:Tools|Test)/[\w./-]+\.(?:py|sh|cmd))")
# 路径尾部可能跟着行号列号（docs 里引用具体代码位置时用），比较前要剥掉
LINE_SUFFIX = re.compile(r":\d+(:\d+)?$")
# 占位符：xxx / yyy / zzz 表示"这里是你自己的名字"，不是真实路径
PLACEHOLDER = re.compile(r"(?:^|[/_])[xXyYzZ]{3}(?=[._/]|$)")
# 整行豁免标记：给"必须提到已删除路径"的场合（变更记录 / 迁移说明）。
# ⚠️ 只豁免**同一行** —— 见文件头。
IGNORE_MARKER = "doclink-ignore"

# ---- ② C 标识符对账 ----------------------------------------------------------
# 只查**工程自有前缀**的标识符（docs/04 规定的命名前缀）。
# ★ 为什么只查这些：文档里还大量出现 `now_ms`、`uint32_t`、`configASSERT` 这类
#   "不是本工程定义的"名字 —— 一刀切地查全部标识符会造出成百上千条假阳性，
#   而假阳性会让门禁失去可信度（那比没有门禁更糟）。
#   工程自有前缀的判据是**完备**的：按 docs/04，本工程自己的函数/宏必然带前缀。
OWN_IDENT = re.compile(
    r"\b("
    r"(?:op|bsp|app|test)_[a-z][a-z0-9_]*"     # 函数 / 变量 / 文件
    r"|LOG_[EWID]"                             # 日志宏
    r"|(?:APP|BOARD|PROJ)_[A-Z0-9_]+"          # 配置宏
    r")\b"
)
# 索引里要扫的代码文件：只有这些才可能"定义"一个工程标识符。
#
# ⚠️⚠️ **绝对不要把 `Tools/*.py` 加进来。**
#    踩过：一开始把 Tools/ 也扫了，于是 `selftest.py` 里那句
#    "注入 `app_rtos_start()`" 的**测试数据**进了符号索引 ——
#    检查器于是认为"代码里有这个名字"，负向用例**静默失效**（门禁变绿）。
#    ⇒ 索引必须只包含**真正的工程代码**，不包含"检查工程代码的那套东西"。
#    这条由 `selftest.py` 的负向用例守着（"文档提到不存在的标识符 → 必须报"）。
# ⚠️ `.vscode/*.json` 同理不加：里面的名字是编辑器配置，不是工程符号。
CODE_GLOBS = ("Core/**/*.[ch]", "Common/**/*.[ch]", "Device/**/*.[ch]",
              "Task/**/*.[ch]", "Operation/**/*.[ch]", "Test/**/*.[ch]",
              "*.h", "*.ld", "*.ioc", "Middlewares/RTT/*.[ch]")
WORD = re.compile(r"\b[A-Za-z_][A-Za-z0-9_]*\b")

# ★ 标识符对账**只查这些文档**。
#
# 判据：**这份文档在描述"本工程实际怎么做"，还是"代码该怎么写"？**
#   · 前者（骨架 / 索引 / 五条链 / 安全边界 / 新增模块清单 / 溯源 / 提案流程）
#     —— 里面的名字是**引用**，必须真的存在。
#   · 后者（`docs/02`~`09`：文件头 · include · 命名 · 注释 · 类型 · 模块组织 ·
#     风格 · 模式速查）—— 里面大量是**示例名**（`bsp_uart_send` / `op_filter_t` /
#     `app_link` / `op_alarm`），它们**本来就不该存在**。
#
# ⚠️ 对后者做对账会造出 40+ 条假阳性。而假阳性会让门禁失去可信度 ——
#    报一堆假错之后人就不看报告了，那比没有门禁更糟。
#    ⇒ 这是**按规则选视图**，不是全局一刀切（见用户级记忆「检查器纪律」）。
# ⚠️ 代价要认：改 `docs/02`~`09` 时，里面的名字**靠人核**。
#    实测就漏过一次：`docs/09` 的组合根示例写着 `app_rtos_start()`，
#    而实际函数叫 `app_rtos_run()` —— 是靠 grep 手工找出来的，不是这个门禁抓的。
SYMBOL_CHECK_FILES = (
    "AGENTS.md", "README.md",
    "docs/README.md", "docs/00_*.md", "docs/01_*.md", "docs/10_*.md",
    "docs/11_*.md", "docs/12_*.md", "docs/13_*.md",
)


def _clean(ref: str) -> str:
    """把反引号里的内容收拾成"可能的路径"：剥掉说明文字、锚点、行号后缀。"""
    ref = ref.strip()
    # 去掉 `docs/01 §强制表` 这种后面跟中文说明的
    ref = re.split(r"[\s（(]", ref, maxsplit=1)[0]
    ref = ref.rstrip("。，、；：,.;:")
    # ⚠️ 顺序不能反：先 rstrip 掉尾部标点，再剥行号。
    #    反过来的话 `` `Core/Src/main.c:47:10: fatal error: ...` `` 会先被切到
    #    `Core/Src/main.c:47:10:`，此时 `:\d+$` 匹配不上（尾部还有个冒号），
    #    于是 rstrip 之后残留 `:47:10` —— 实测踩到的假死链。
    ref = LINE_SUFFIX.sub("", ref)
    return ref


def _check_one(root: Path, ref: str) -> tuple[bool, str]:
    """判定一个引用。返回 (是否 OK, 说明)。"""
    if ref.startswith(("http://", "https://", "mailto:")):
        return True, "URL，跳过"
    if "<" in ref or ">" in ref:
        return True, "含占位符，跳过"
    if "|" in ref:
        return True, "含竖线（枚举写法），跳过"
    # `docs/01~13`：区间写法，不是一条路径
    if "~" in ref:
        return True, "区间写法，跳过"
    # `Operation/Src/op_xxx.c`：xxx 是"这里填你的模块名"的占位符，不是路径声明。
    # 约定写在 docs/README.md §短引用约定 —— 示例路径一律用 xxx。
    if PLACEHOLDER.search(ref):
        return True, "含 xxx 占位符，跳过"
    # 运行时生成的目录：不存在是正常状态，不是死链（前缀判定，含子路径）
    if any(ref == p or ref.startswith(p) for p in OPTIONAL_PREFIXES):
        return True, "运行时目录，跳过"

    # 短引用：docs/01 → docs/01*.md，且必须唯一
    m = SHORT_REF.match(ref)
    if m:
        hits = sorted(root.glob(f"docs/{m.group(1)}*.md"))
        if len(hits) == 1:
            return True, f"→ {hits[0].name}"
        if not hits:
            return False, f"docs/{m.group(1)}*.md 不存在"
        return False, ("同编号有 %d 个文档（%s）—— 短引用指向不确定，"
                       "改文件名时保持编号唯一" % (len(hits), ", ".join(h.name for h in hits)))

    # `docs/0x` / `docs/0x_*.md`：x 当任意数字
    mx = SHORT_REF_X.match(ref)
    if mx:
        digits = mx.group(1).replace("x", "[0-9]")
        tail = mx.group(2) or ""
        pat = f"docs/{digits}*.md" if tail == "" else f"docs/{digits}{tail}"
        hits = sorted(root.glob(pat))
        if hits:
            return True, f"→ {len(hits)} 篇"
        return False, f"{pat} 匹配到 0 个"

    if not ref.startswith(PATH_ROOTS):
        return True, "非路径声明，跳过"

    # 目录（以 / 结尾）
    if ref.endswith("/"):
        return ((root / ref).is_dir(), f"{ref} {'存在' if (root / ref).is_dir() else '目录不存在'}")

    # 通配
    if "*" in ref:
        hits = sorted(root.glob(ref))
        return (bool(hits), f"匹配到 {len(hits)} 个")

    target = root / ref
    if target.exists():
        return True, "存在"
    # 给出"差一点"的提示：同目录下有没有名字相近的？
    near = sorted(p.name for p in target.parent.glob(target.name.split("_")[0] + "*")) \
        if target.parent.is_dir() else []
    hint = f"（同目录相近的：{', '.join(near[:3])}）" if near else ""
    return False, f"不存在{hint}"


def build_symbol_index(root: Path) -> set[str]:
    """把工程里所有代码文件里的"词"收集成一个集合 —— 用于标识符对账。

    ★ 做法故意粗糙（扫词，不解析语法）：这里只回答一个问题
      "这个名字在代码里出现过吗"。真解析 C 会引入一堆解析失败，
      而解析失败会让检查**静默失效** —— 比粗糙更糟。
    ⚠️ 因此它**只查名字是否存在**，不查签名/参数个数。
    """
    symbols: set[str] = set()
    for pattern in CODE_GLOBS:
        for path in root.glob(pattern):
            if not path.is_file():
                continue
            try:
                text = path.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            symbols.update(WORD.findall(text))
    return symbols


def scan_file(root: Path, path: Path,
              symbols: set[str] | None = None
              ) -> tuple[list[tuple[int, str, str]], list[tuple[int, str, str]]]:
    """扫一个文件。返回 (问题, 已检查项)。"""
    bad: list[tuple[int, str, str]] = []
    checked: list[tuple[int, str, str]] = []
    in_fence = False

    for lineno, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
        if FENCE.match(line):
            in_fence = not in_fence
            continue

        # 整行豁免（只影响本行，不影响相邻行）
        if IGNORE_MARKER in line:
            continue

        # ---- ② 标识符对账：正反引号与代码块都要查（名字写错在哪都一样坏）----
        if symbols is not None:
            for m in OWN_IDENT.finditer(line):
                name = m.group(1)
                if PLACEHOLDER.search(name):
                    continue  # `op_xxx` 这类占位符
                # `test_op_<名>.c` 会被正则截成 `test_op_`（`<` 不是词字符）——
                # 这是**写法**造成的截断，不是陈旧标识符。两个判据都拦一下：
                #   ① 名字以 `_` 结尾（工程里没有这种标识符）
                #   ② 紧跟其后的是 `<`
                if name.endswith("_") or line[m.end():m.end() + 1] == "<":
                    continue
                if name in symbols:
                    checked.append((lineno, name, "标识符存在"))
                else:
                    bad.append((lineno, name,
                                "代码里没有这个名字 —— 文档写在实现定型之前？"
                                "（改文档，或确认代码里是不是改名了）"))

        if in_fence:
            # 代码块：只看命令行里引用的脚本
            for m in CMD_PATH.finditer(line):
                ref = m.group(1)
                ok, why = _check_one(root, ref)
                (checked if ok else bad).append((lineno, ref, why))
            continue

        # 正文：只看反引号
        for m in CODE_SPAN.finditer(line):
            ref = _clean(m.group(1))
            if not ref:
                continue
            ok, why = _check_one(root, ref)
            if why.endswith("跳过"):
                continue  # 不是路径声明，不进报告
            (checked if ok else bad).append((lineno, ref, why))

    return bad, checked


def main() -> int:
    ap = argparse.ArgumentParser(description="文档与代码的一致性检查（路径引用 + C 标识符）")
    ap.add_argument("--verbose", action="store_true", help="连检查通过的引用一起打")
    ap.add_argument("--root", default=str(ROOT))
    ap.add_argument("--no-symbols", action="store_true",
                    help="跳过 C 标识符对账（只查路径引用）")
    args = ap.parse_args()
    root = Path(args.root)

    files: list[Path] = []
    for g in SCAN_GLOBS:
        files += sorted(root.glob(g))
    files = [f for f in files if f.is_file()]
    if not files:
        print("!! 没找到可扫描的文档", file=sys.stderr)
        return 2

    symbols = None if args.no_symbols else build_symbol_index(root)
    if symbols is not None:
        print(f"代码符号索引: {len(symbols)} 个词")
        print("标识符对账范围: " + " · ".join(SYMBOL_CHECK_FILES))
        print("（docs/02~09 是示例型规范，名字本来就是虚构的，不参与对账 —— 见脚本内说明）\n")

    total_bad = total_checked = 0
    for f in files:
        rel = f.relative_to(root).as_posix()
        check_syms = symbols if any(f.match(g) for g in SYMBOL_CHECK_FILES) else None
        bad, checked = scan_file(root, f, check_syms)
        total_bad += len(bad)
        total_checked += len(checked)
        if bad:
            print(f"!! {rel}")
            for lineno, ref, why in bad:
                print(f"     {lineno:>4}: {ref}  —— {why}")
        elif args.verbose:
            print(f"OK {rel}（{len(checked)} 条引用全部有效）")

    print()
    print(f"扫描 {len(files)} 个文档，检查 {total_checked} 条引用"
          f"（含路径与工程自有标识符）。")
    if total_bad:
        print(f"!! {total_bad} 处不一致。")
        print("   修法：改文档让它跟上代码；或确认代码里是不是改名了（那就改文档）。")
        print("   ⚠️ 不要为了让检查变绿而把引用删掉 —— 那是把门禁拆了。")
        print("   ⚠️ 变更记录这类**必须提到旧名字**的行，行尾加 "
              "`<!-- doclink-ignore -->` 整行豁免。")
        return 1
    print("无死链、无陈旧标识符。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
