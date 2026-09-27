#!/usr/bin/env python3
"""selftest —— 门禁自检：注入**真违规**，证明门禁会报错。

为什么必须有这个文件
--------------------
**「退出码 0」不是有效判据** —— 一个什么都不做的检查器同样返回 0。
静态检查器的失效是**无声的**：不报错、不影响构建、报告照样全绿。
所以自检必须做两件事：

1. **基线**：在未注入任何违规的工程副本上跑全部门禁 → 必须全绿。
   （排除"永远报错"的假检查器 —— 那种检查器看起来也很"严格"。）
2. **负向用例**：逐条注入**真违规** → 对应门禁**必须**报错。
   （排除"什么都不做"的假检查器。）

⚠️ 全部在**工程副本**上做，原工程全程只读。

⚠️ 改完 `Tools/` 下任何检查器后**必跑**这个。判据不是"我写对了"，
   而是"我跑过它、并且它按预期失败过"。

用法
----
    python Tools/selftest.py            # 静态门禁（秒级）
    python Tools/selftest.py --full     # 追加编译期用例（较慢，要 cmake 配置+编译）
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PY = sys.executable

# 复制工程时排除的东西：构建产物（大）、AI 记忆、版本库
COPY_IGNORE = shutil.ignore_patterns(
    "build", ".git", ".workbuddy-ai", ".workbuddy", "__pycache__", "*.pyc", "_backup*",
)


# ------------------------------------------------------------------ 跑命令
def run(cmd: list[str], cwd: Path, timeout: int = 900) -> tuple[int, str]:
    try:
        p = subprocess.run(cmd, cwd=str(cwd), capture_output=True, text=True,
                           timeout=timeout, errors="replace")
        return p.returncode, (p.stdout or "") + (p.stderr or "")
    except subprocess.TimeoutExpired:
        return 124, f"!! 超时（{timeout}s）"


# ------------------------------------------------------------------ 四个静态门禁
def gate_lint(root: Path) -> tuple[int, str]:
    return run([PY, "-u", "Tools/cubemx_sync.py", "--strict"], root)


def gate_doccheck(root: Path) -> tuple[int, str]:
    return run([PY, "-u", "Tools/check_doc_links.py"], root)


def _board_env(preset: str) -> dict[str, str]:
    """读原工程 CMake 生成的 build/<preset>/board.env（与 fw.py 同一来源）。

    ⚠️ 工程名**不在这里硬编码**。本函数存在的唯一理由：这里曾写死
       `build/Debug/f407vgt6.elf`，于是**派生工程改名后**，明明构建过了，
       `sizecheck` 却报"原工程还没构建" —— 一个只在改名时才现形的**假失败**。
       走 board.env ⇒ 派生只需改 CMakeLists 的 CMAKE_PROJECT_NAME 一处。
    """
    env_file = ROOT / "build" / preset / "board.env"
    if not env_file.is_file():
        return {}
    out: dict[str, str] = {}
    for line in env_file.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.lstrip().startswith("#") or "=" not in line:
            continue
        k, v = line.split("=", 1)
        out[k.strip()] = v.strip()
    return out


def gate_sizecheck(root: Path) -> tuple[int, str]:
    """体积门禁。ELF 与 .ld 直接指向原工程（只读）—— 副本里不复制 build/。"""
    be = _board_env("Debug")
    elf_path, ld_path = be.get("ELF"), be.get("LD")
    elf = Path(elf_path) if elf_path else None
    ld = Path(ld_path) if ld_path else next(iter(ROOT.glob("*.ld")), None)
    if elf is None or not elf.is_file() or ld is None:
        return 6, "!! 原工程还没构建（先 fw.py build），无法跑体积门禁用例"
    return run([PY, "-u", "Tools/stm32_size.py", str(elf), "--ld", str(ld), "--check"], root)


def gate_test(root: Path) -> tuple[int, str]:
    return run([PY, "-u", "Tools/fw.py", "test"], root)


def gate_build(root: Path) -> tuple[int, str]:
    return run([PY, "-u", "Tools/fw.py", "build"], root, timeout=1800)


# ------------------------------------------------------------------ 注入器
def _patch_board(root: Path, macro: str, new_value: str) -> None:
    """改 board_config.h 里某个宏的值。"""
    p = root / "Core" / "Inc" / "board_config.h"
    text = p.read_text(encoding="utf-8")
    patched, n = re.subn(rf"(^\s*#\s*define\s+{macro}\s+)\S+",
                         rf"\g<1>{new_value}", text, count=1, flags=re.M)
    assert n == 1, f"注入失败：board_config.h 里没找到 {macro}"
    p.write_text(patched, encoding="utf-8")


def _append_to_agents(root: Path, snippet: str) -> None:
    p = root / "AGENTS.md"
    p.write_text(p.read_text(encoding="utf-8") + "\n" + snippet + "\n", encoding="utf-8")


def _append_to_doc(root: Path, name_prefix: str, snippet: str) -> None:
    """往 docs/<前缀>*.md 追加一段（用来测"示例型文档不参与标识符对账"）。"""
    hits = sorted((root / "docs").glob(f"{name_prefix}*.md"))
    assert hits, f"注入失败：docs/{name_prefix}*.md 不存在"
    p = hits[0]
    p.write_text(p.read_text(encoding="utf-8") + "\n" + snippet + "\n", encoding="utf-8")


def _patch_size_budget(root: Path, region: str, kb: int) -> None:
    """把 stm32_size.py 的 BUDGETS 里某个区调到 kb（用来证明门禁真的会拦）。"""
    p = root / "Tools" / "stm32_size.py"
    text = p.read_text(encoding="utf-8")
    patched, n = re.subn(rf'("{region}":\s*)\d+', rf"\g<1>{kb}", text, count=1)
    assert n == 1, f"注入失败：stm32_size.py 的 BUDGETS 里没找到 {region}"
    p.write_text(patched, encoding="utf-8")


def _break_operation_layering(root: Path) -> None:
    """在 Operation 层 include HAL 头 —— 规矩 2 的编译期强制应该当场拦下。"""
    p = root / "Operation" / "Src" / "op_blink.c"
    text = p.read_text(encoding="utf-8")
    p.write_text(text.replace('#include "op_blink.h"',
                              '#include "op_blink.h"\n#include "stm32f4xx_hal.h"', 1),
                 encoding="utf-8")


def _use_rtos_in_operation(root: Path) -> None:
    """在 Operation 层调用 FreeRTOS 函数 —— PC 侧链接期应该报 undefined reference。"""
    p = root / "Operation" / "Src" / "op_blink.c"
    text = p.read_text(encoding="utf-8")
    text += ("\nextern void vTaskDelay(unsigned);\n"
             "void op_blink_layering_probe(void) { vTaskDelay(1U); }\n")
    p.write_text(text, encoding="utf-8")


# ------------------------------------------------------------------ 用例表
# (名字, 门禁, 注入, 期望"失败"的门禁名)
STATIC_CASES: list[tuple[str, str, object]] = [
    ("改 HSE 频率 → 与 .ioc 矛盾", "lint", lambda r: _patch_board(r, "BOARD_HSE_VALUE", "25000000U")),
    ("改心跳灯引脚 → 与 .ioc 矛盾", "lint", lambda r: _patch_board(r, "BOARD_LED_PIN", "5")),
    ("改日志串口 TX 脚 → 与 .ioc 矛盾", "lint", lambda r: _patch_board(r, "BOARD_LOG_UART_TX_PIN", "8")),
    ("改 RAM 容量 → 与链接脚本矛盾", "lint", lambda r: _patch_board(r, "BOARD_RAM_KB", "64U")),
    ("改 J-Link 器件名 → 前缀对不上", "lint", lambda r: _patch_board(r, "BOARD_JLINK_DEVICE", '"STM32F429ZI"')),
    ("引用不存在的文档 docs/99", "doccheck", lambda r: _append_to_agents(r, "见 `docs/99`。")),
    ("代码块里教人跑不存在的脚本", "doccheck",
     lambda r: _append_to_agents(r, "```bash\npython Tools/nonexistent_script.py\n```")),
    ("同编号文档出现两个 → 短引用二义", "doccheck",
     lambda r: (r / "docs" / "01_重复编号.md").write_text("# 重复\n", encoding="utf-8")),
    ("把体积预算调到 1 KB → 门禁必须拦", "sizecheck",
     lambda r: _patch_size_budget(r, "FLASH", 1)),
    # 这条测的是"豁免标记没有漏到相邻行"：第一行有标记（该被豁免），
    # 第二行没有（必须被拦下）。若实现写成了"看到标记就跳过整块/整个文件"，
    # 门禁会放行 ⇒ 这条用例失败 ⇒ 当场暴露。
    ("doclink-ignore 只豁免同一行（相邻行仍须检查）", "doccheck",
     lambda r: _append_to_agents(
         r, "旧路径 `Tools/已删除的脚本.py` <!-- doclink-ignore -->\n"
            "但这一行的 `Tools/也是不存在的.py` 必须被拦下")),
    # ---- 标识符对账（文档写在实现定型之前 → 名字会变，而这不报错）----
    ("文档提到不存在的工程标识符 → 必须报", "doccheck",
     lambda r: _append_to_agents(r, "启动靠 `app_rtos_start()` 交出控制权。")),
    ("文档提到不存在的配置宏 → 必须报", "doccheck",
     lambda r: _append_to_agents(r, "阈值写在 `APP_FILTER_ALPHA` 里。")),
]

FULL_CASES: list[tuple[str, str, object]] = [
    ("Operation 里 include HAL 头 → 编译期必须失败", "build", _break_operation_layering),
    ("Operation 里调 vTaskDelay → PC 链接期必须失败", "test", _use_rtos_in_operation),
]

# 正向用例：注入了**合法写法**，门禁必须**仍然通过**。
# ★ 为什么需要这一组：门禁有两种死法 ——
#   ① "什么都不做"（漏报）→ 由上面的负向用例挡；
#   ② "什么都报"（假阳性）→ 由这一组挡。
#   第二种更致命：报一堆假错之后，人就再也不看报告了，门禁等于不存在。
#   本工程已经踩过两次假阳性（`build/` 目录、`Core/Src/main.c:47:10` 的行号后缀），
#   所以把这几类**已确认合法**的写法固化成用例，防止改检查器时又踩回去。
POSITIVE_CASES: list[tuple[str, str, object]] = [
    ("占位符 xxx 不算死链", "doccheck",
     lambda r: _append_to_agents(r, "示例：`Operation/Src/op_xxx.c`、`Device/Src/bsp_xxx.c`")),
    ("区间写法 docs/01~13 不算死链", "doccheck",
     lambda r: _append_to_agents(r, "规则见 `docs/01~13`。")),
    ("编号占位符 docs/0x 不算死链", "doccheck",
     lambda r: _append_to_agents(r, "细则在 `docs/0x` 里。")),
    ("运行时目录 build/ 不存在不算死链", "doccheck",
     lambda r: _append_to_agents(r, "产物在 `build/Debug/` 下。")),
    ("行号后缀 main.c:47:10 不算死链", "doccheck",
     lambda r: _append_to_agents(r, "报错形如 `Core/Src/main.c:47:10: fatal error: ...`。")),
    # ★ 这一条挡的是"标识符对账把示例名也报出来" —— 示例型文档（docs/02~09）
    #   里全是虚构名字，对它们做对账会造出 40+ 条假阳性。
    ("示例型文档里的虚构名字不算陈旧标识符", "doccheck",
     lambda r: _append_to_doc(r, "04", "示例：`bsp_uart_send()` / `op_filter_t` / `app_link`。")),
    ("占位符标识符 op_xxx 不算陈旧", "doccheck",
     lambda r: _append_to_agents(r, "照着 `op_xxx_step()` 的形状写。")),
    ("截断写法 test_op_<名> 不算陈旧", "doccheck",
     lambda r: _append_to_agents(r, "单测放在 `Test/test_op_<名>.c`。")),
]

GATES = {
    "lint": gate_lint,
    "doccheck": gate_doccheck,
    "sizecheck": gate_sizecheck,
    "test": gate_test,
    "build": gate_build,
}


def main() -> int:
    ap = argparse.ArgumentParser(description="门禁自检（基线 + 负向用例）")
    ap.add_argument("--full", action="store_true", help="追加编译期用例（较慢）")
    ap.add_argument("--keep", action="store_true", help="保留副本目录（排错用）")
    args = ap.parse_args()

    cases = STATIC_CASES + (FULL_CASES if args.full else [])
    gates_used = sorted({g for _, g, _ in cases} | {g for _, g, _ in POSITIVE_CASES})

    tmp = Path(tempfile.mkdtemp(prefix="fw_selftest_"))
    print(f"副本目录: {tmp}\n")

    try:
        # ---------------- 基线：未注入时，全部门禁必须通过 ----------------
        print("== 基线（无注入，全部门禁必须通过）==")
        base = tmp / "baseline"
        shutil.copytree(ROOT, base, ignore=COPY_IGNORE)
        # 副本里没有 build/，而 fw.py test 需要 configure（会自己配）——
        # 但 gate_sizecheck 用的是原工程的 ELF，所以基线里不跑它，单独在用例里验。
        baseline_gates = [g for g in gates_used if g != "sizecheck"]
        bad_baseline: list[str] = []
        for g in baseline_gates:
            rc, out = GATES[g](base)
            mark = "OK  " if rc == 0 else "FAIL"
            print(f"  [{mark}] 基线 {g}  (rc={rc})")
            if rc != 0:
                bad_baseline.append(g)
                print("\n".join("        " + ln for ln in out.strip().splitlines()[-15:]))
        if bad_baseline:
            print(f"\n!! 基线不通过：{bad_baseline}")
            print("   一个在干净工程上就报错的门禁 = 假检查器（永远报错，没有判别力）。")
            print("   先修门禁，再谈负向用例。")
            return 1
        # 体积门禁的基线单独跑（它读原工程的 ELF，与副本无关）
        rc, out = gate_sizecheck(ROOT)
        print(f"  [{'OK  ' if rc == 0 else 'FAIL'}] 基线 sizecheck  (rc={rc})")
        if rc != 0:
            print("\n".join("        " + ln for ln in out.strip().splitlines()[-15:]))
            print("\n!! 体积门禁基线不通过 —— 先 fw.py build，或看上面的超预算明细。")
            return 1
        print()

        # ---------------- 负向用例：每条都必须被拦下 ----------------
        print("== 负向用例（注入真违规，对应门禁必须报错）==")
        failures: list[str] = []
        for i, (name, gate, mutate) in enumerate(cases, 1):
            work = tmp / f"case{i:02d}"
            shutil.copytree(ROOT, work, ignore=COPY_IGNORE)
            mutate(work)  # type: ignore[operator]
            rc, out = GATES[gate](work)
            ok = rc != 0
            print(f"  [{'OK  ' if ok else 'FAIL'}] {i:>2}. {name}   ({gate} rc={rc})")
            if not ok:
                failures.append(name)
                print("        ⚠️ 注入了违规但门禁仍然通过 —— 这条规则**没有强制力**。")
                print("\n".join("        " + ln for ln in out.strip().splitlines()[-10:]))
            if not args.keep:
                shutil.rmtree(work, ignore_errors=True)

        print()

        # ---------------- 正向用例：合法写法不得被误报 ----------------
        print("== 正向用例（合法写法，门禁必须仍然通过）==")
        false_alarms: list[str] = []
        for i, (name, gate, mutate) in enumerate(POSITIVE_CASES, 1):
            work = tmp / f"pos{i:02d}"
            shutil.copytree(ROOT, work, ignore=COPY_IGNORE)
            mutate(work)  # type: ignore[operator]
            rc, out = GATES[gate](work)
            ok = rc == 0
            print(f"  [{'OK  ' if ok else 'FAIL'}] {i:>2}. {name}   ({gate} rc={rc})")
            if not ok:
                false_alarms.append(name)
                print("        ⚠️ 合法写法被误报 —— 这是**假阳性**。")
                print("           门禁的第二种死法：报得越多，人越不看报告，"
                      "最后等于没有门禁。")
                print("\n".join("        " + ln for ln in out.strip().splitlines()[-10:]))
            if not args.keep:
                shutil.rmtree(work, ignore_errors=True)

        print()
        if false_alarms:
            print(f"!! {len(false_alarms)} 条合法写法被误报：")
            for f in false_alarms:
                print(f"   · {f}")
            print("\n   修检查器（放宽判定），别删用例。")
            return 1
        print(f"正向用例通过：{len(POSITIVE_CASES)} 条合法写法均未被误报。")

        if failures:
            print(f"\n!! {len(failures)} 条负向用例未被拦下：")
            for f in failures:
                print(f"   · {f}")
            print("\n   这说明对应门禁**对该类违规无感**。修门禁，别改用例。")
            return 1
        print(f"自检通过：基线全绿 + {len(cases)} 条负向用例全部被拦下 "
              f"+ {len(POSITIVE_CASES)} 条正向用例无误报。")
        if not args.full:
            print("（静态用例已跑完。编译期用例要 --full —— 它们较慢，但能证明"
                  "规矩 2 的两道墙真的存在。）")
        return 0
    finally:
        if not args.keep:
            shutil.rmtree(tmp, ignore_errors=True)
        else:
            print(f"\n副本保留在：{tmp}")


if __name__ == "__main__":
    sys.exit(main())
