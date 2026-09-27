#!/usr/bin/env python3
"""从 `.ioc` 里移除一个外设 —— 连同它的引脚、编号、初始化函数条目。

    python Tools/ioc_drop.py [--write] <外设名> [<外设名> ...]

为什么必须工具化
----------------
删一个外设要同时动 **5 处**，漏一处 CubeMX 不报错，但会留下悬空引用
（表现是"某天加新引脚时突然冒出重复/丢失"）：

  1) `Mcu.IPN=<外设>`  + `Mcu.IPNb`
  2) `Mcu.PinN=<引脚>` + `Mcu.PinsNb`   ← ⚠️ 重排后必须 0..N-1 连续无重复
  3) `<外设>.*=` 配置行
  4) `<引脚>.*=` 引脚行（Mode / Signal / GPIOParameters / GPIO_Label）
  5) `ProjectManager.functionlistsort` 里的 `MX_<外设>_Init-<外设>-...`

⚠️ 引脚是靠 `<引脚>.Signal=<外设>_xxx` **反查**出来的。若某引脚同时挂在别的
   外设上（共用引脚），删掉它会把那个外设也弄坏 —— 所以脚本先检查，发现
   共用就**拒绝执行**，不做"猜一个"这种事。

⚠️ `Mcu.PinN` 的 N 是 CubeMX 内部分配的索引，**手工重排出过真错误**
   （引脚重复 / 丢失）。所以这里统一由脚本重排并校验（与 `ioc_append.py` 同口径）。

⚠️ 必须写回 **CRLF**：CubeMX 自己保存的 `.ioc` 是 CRLF，写 LF 会制造满屏假 diff。

用法
----
    python Tools/ioc_drop.py spi2             # 只预览，不写
    python Tools/ioc_drop.py --write spi2     # 写入（先自动备份 .ioc）
"""
import argparse
import re
import shutil
import sys
from datetime import date
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ioc_common import check_pins, parse_indexed  # noqa: E402  (注入路径后才能 import)

ROOT = Path(__file__).resolve().parent.parent


def reindex(lines: list[str], prefix: str, drop_values: set[str]) -> tuple[list[str], int]:
    """从 `<prefix>N=值` 列表里删掉 drop_values，重排编号，返回 (新行列表, 删了几条)。

    ⚠️ **保持行的物理顺序不变，只改编号**。CubeMX 自己保存的 `.ioc` 是按 key 做
       **字符串排序**的（`Pin0, Pin1, Pin10, …, Pin19, Pin2, Pin20, …`）。若按数字序
       重新排列整个列表，语义虽然对，但 `git diff` 会显示成"每行都变了"，
       把真正的改动（删了哪几行）淹掉 —— 实测踩过一次。
    """
    pat = re.compile(rf"^{re.escape(prefix)}(\d+)=(.*)$")
    entries = parse_indexed("\n".join(lines), prefix)
    if not entries:
        return lines, 0

    # 旧编号升序 → 新编号 0..N-1。值必须唯一（check_pins 已保证）
    kept = [v for _, v in sorted(entries.items()) if v not in drop_values]
    new_index = {v: n for n, v in enumerate(kept)}

    out: list[str] = []
    dropped = 0
    for ln in lines:
        m = pat.match(ln)
        if not m:
            out.append(ln)
            continue
        val = m.group(2).strip()
        if val in drop_values:
            dropped += 1
            continue
        out.append(f"{prefix}{new_index[val]}={val}")
    return out, dropped


def drop_fn_entry(lines: list[str], periph: str) -> tuple[list[str], bool]:
    """从 `ProjectManager.functionlistsort` 去掉 `MX_<外设>_Init-<外设>-...` 条目。

    ⚠️ 重排编号必须用「剩余条目数 + 1」算。踩过的坑：拿 `\\d+-MX_` 正则去数条目会
       漏掉 `1-SystemClock_Config-RCC-...`（不以 MX_ 开头），于是算出重复编号。
    """
    for i, ln in enumerate(lines):
        if not ln.startswith("ProjectManager.functionlistsort="):
            continue
        items = ln.split("=", 1)[1].split(",")
        kept = []
        for it in items:
            parts = it.split("-")
            if len(parts) >= 3 and parts[2] == periph:
                continue
            kept.append(parts[1:] if len(parts) > 1 else parts)
        if len(kept) == len(items):
            return lines, False
        lines[i] = ("ProjectManager.functionlistsort="
                    + ",".join(f"{n + 1}-{'-'.join(k)}" for n, k in enumerate(kept)))
        return lines, True
    return lines, False


def drop(lines: list[str], periph: str) -> tuple[list[str] | None, str | None]:
    """移除外设。成功返回 (新行列表, None)，失败返回 (None, 原因)。"""
    sig_pat = re.compile(rf"^([A-Za-z0-9_\-]+)\.Signal={re.escape(periph)}_")
    pins = {m.group(1) for ln in lines if (m := sig_pat.match(ln))}
    if not pins:
        return None, f"找不到 {periph} 的引脚（没有任何 `*.Signal={periph}_*`）"

    # 共用检查：这些引脚还挂在别的外设上吗
    shared = []
    for p in sorted(pins):
        own = f"{p}.Signal={periph}_"
        others = [ln for ln in lines if ln.startswith(p + ".Signal=") and not ln.startswith(own)]
        if others:
            shared.append(f"{p}（还挂在 {others[0].split('=', 1)[1]}）")
    if shared:
        return None, f"引脚被共用，拒绝删除：{'、'.join(shared)}"

    # 1) 删外设配置行 + 引脚行
    pin_prefixes = tuple(p + "." for p in pins)
    lines = [ln for ln in lines if not (ln.startswith(periph + ".") or ln.startswith(pin_prefixes))]

    # 2) 编号列表重排
    lines, _ = reindex(lines, "Mcu.Pin", pins)
    lines, _ = reindex(lines, "Mcu.IP", {periph})

    # 3) 计数键（必须在重排之后重新数）
    n_pin = len(parse_indexed("\n".join(lines), "Mcu.Pin"))
    n_ip = len(parse_indexed("\n".join(lines), "Mcu.IP"))
    for i, ln in enumerate(lines):
        if ln.startswith("Mcu.PinsNb="):
            lines[i] = f"Mcu.PinsNb={n_pin}"
        elif ln.startswith("Mcu.IPNb="):
            lines[i] = f"Mcu.IPNb={n_ip}"

    # 4) 初始化函数列表
    lines, _ = drop_fn_entry(lines, periph)

    return lines, None


def main() -> int:
    ap = argparse.ArgumentParser(description="从 .ioc 移除外设")
    ap.add_argument("--write", action="store_true", help="写入工程 .ioc（默认只预览）")
    ap.add_argument("periph", nargs="+", help="外设名（.ioc 里的写法，如 spi2 / usart2）")
    args = ap.parse_args()

    ioc = next(iter(ROOT.glob("*.ioc")), None)
    if ioc is None:
        print(f"在 {ROOT} 下找不到 .ioc")
        return 1

    text = ioc.read_text(encoding="utf-8")  # universal newlines → LF
    lines = text.split("\n")

    errs = check_pins(text, "输入")
    if errs:
        print("输入 .ioc 本身有问题，先修它：")
        for e in errs:
            print("  -", e)
        return 1
    print(f"输入 {ioc.name} · 引脚 {len(parse_indexed("\n".join(lines), 'Mcu.Pin'))} 个 / "
          f"外设 {len(parse_indexed("\n".join(lines), 'Mcu.IP'))} 个")
    print()

    done = []
    for p in args.periph:
        before_pins = len(parse_indexed("\n".join(lines), "Mcu.Pin"))
        new_lines, err = drop(lines, p.upper())
        if err:
            print(f"  ✗ {p}: {err}")
            return 1
        lines = new_lines
        after_pins = len(parse_indexed("\n".join(lines), "Mcu.Pin"))
        done.append((p.upper(), before_pins - after_pins))

    for name, n in done:
        print(f"  - 移除 {name}（连带 {n} 个引脚）")

    out = "\n".join(lines)
    errs = check_pins(out, "输出")
    if errs:
        print("\n输出校验失败（不写文件）：")
        for e in errs:
            print("  -", e)
        return 1
    print(f"\n输出校验通过：引脚 {len(parse_indexed("\n".join(lines), 'Mcu.Pin'))} 个 / "
          f"外设 {len(parse_indexed("\n".join(lines), 'Mcu.IP'))} 个，编号连续无重复")

    if not args.write:
        print("\n（预览模式，未写文件。加 --write 才写入。）")
        return 0

    bak = ioc.with_name(f"{ioc.name}.bak_{date.today():%Y%m%d}_drop")
    shutil.copy2(ioc, bak)
    ioc.write_text(out, encoding="utf-8", newline="\r\n")  # 必须 CRLF
    print(f"\n[备份] {bak.name}")
    print(f"[写入] {ioc}")
    print("下一步：交给 CubeMX 加载 + 生成，确认外设真的没了（见 memory 2026-09-26）。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
