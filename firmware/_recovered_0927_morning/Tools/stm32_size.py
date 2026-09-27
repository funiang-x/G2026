#!/usr/bin/env python3
"""stm32_size —— 按链接脚本的 MEMORY 区，算固件真实占用 + 预算门禁。

为什么需要它
------------
`arm-none-eabi-size` 给的是 section 求和（text/data/bss），**不等于**链接器烧写
时的真实占用：section 之间有 ALIGN 对齐间隙，且 .data 的初值虽然运行时在 RAM，
烧写时仍占 Flash。构建日志里 `ld` 打的 `Memory region Used Size` 才是权威口径。

本模块用 LOAD 段边界复现该口径，实测与 `ld --print-memory-usage` **逐字节一致**：

    ROM 区（链接脚本里属性带 x 不带 w，如 `FLASH (rx)`）:
        占用 = max(paddr + filesz) - ORIGIN
        取 paddr：.data 的初值落在 Flash，其 LOAD 段 paddr 紧跟 .text，
                  所以这条式子天然把"Flash 上的初值"算进去了。
    RAM 区（属性带 w，如 `RAM (rwx)` / `CCMRAM (rw)`）:
        占用 = max(vaddr + memsz) - ORIGIN
        取 memsz：才能覆盖 .bss / heap / stack 这些 filesz=0 的 NOBITS 段。

★ 本模块是**唯一口径来源**：`fw.py size` 与 `fw.py sizecheck` 都调它。
  ⚠️ 回答"固件多大"之前先跑它，**不要**凭 `arm-none-eabi-size` 的三列下结论。

预算（BUDGETS）
---------------
超线即判失败（退出码 1）。预算按"这块芯片 + 这个工程的阶段"定，
**不是为了好看而定的上限**：

    ⚠️ 为了让 sizecheck 变绿而调高上限 = 把门禁拆了。
       要调必须先说清理由（docs/12 §B：改门禁属于"先说再做"）。

用法
----
    python Tools/stm32_size.py build/Debug/f407vgt6.elf            # 只看明细
    python Tools/stm32_size.py build/Debug/f407vgt6.elf --check    # 加预算门禁
    python Tools/stm32_size.py --preset Debug --check              # 从 board.env 推路径
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# ---------------------------------------------------------------- 预算（KB）
# ⚠️ 单位 KB。改这里等于改门禁 —— 理由要写进 docs/12 的授权记录。
# ⚠️ 数值取自"这块芯片的总量"，不是"当前用量的 1.2 倍" —— 后者会随用量漂移，
#    等于门禁自动放宽。
BUDGETS: dict[str, int] = {
    # 空白模板基线约 22 KB（HAL + FreeRTOS + RTT + 本工程代码）。
    # 给到 256 KB：留足加业务的空间，同时能挡住"误引一个大库"。
    "FLASH": 256,
    # 含 FreeRTOS 堆（configTOTAL_HEAP_SIZE = 15 KB）+ 默认任务栈 1 KB
    # + HAL 的静态缓冲。F407VG 的 RAM 段总共 128 KB。
    "RAM": 96,
    # 当前**不用** CCM（docs/00 暂缓）。上限写成全段，所以它现在拦不住东西 ——
    # 等真的把 CCM 用起来，再按实际需求收紧，并把理由写进 docs/12。
    "CCMRAM": 64,
}

_UNIT = {"": 1, "K": 1024, "M": 1024**2, "G": 1024**3}


def read_memory_regions(ld_path: Path) -> dict[str, tuple[int, int, bool]]:
    """解析链接脚本 MEMORY 块。返回 {区名: (ORIGIN, LENGTH, 是否只读代码区)}。

    只读代码区 = 属性含 x 且不含 w（如 FLASH (rx)）。
    """
    regions: dict[str, tuple[int, int, bool]] = {}
    if not Path(ld_path).is_file():
        return regions
    text = Path(ld_path).read_text(encoding="utf-8", errors="replace")
    m = re.search(r"MEMORY\s*\{(.*?)\}", text, re.S)
    if not m:
        return regions
    for line in m.group(1).splitlines():
        mm = re.match(
            r"\s*(\w+)\s*\(([^)]*)\)\s*:\s*ORIGIN\s*=\s*(0x[0-9A-Fa-f]+|\d+)"
            r"\s*,\s*LENGTH\s*=\s*(\d+)\s*([KMG]?)",
            line.strip(),
        )
        if not mm:
            continue
        name, attrs, org, ln, unit = mm.groups()
        is_rom = "x" in attrs and "w" not in attrs
        regions[name] = (int(org, 0), int(ln) * _UNIT[unit], is_rom)
    return regions


def _load_segments(elf: Path, readelf: str) -> list[tuple[int, int, int, int]]:
    """读 PT_LOAD 段，返回 [(vaddr, paddr, filesz, memsz), ...]。"""
    proc = subprocess.run([readelf, "-l", str(elf)], capture_output=True, text=True)
    segs: list[tuple[int, int, int, int]] = []
    for line in proc.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 6 and parts[0] == "LOAD":
            try:
                segs.append((int(parts[2], 16), int(parts[3], 16),
                             int(parts[4], 16), int(parts[5], 16)))
            except ValueError:
                continue
    return segs


def _section_size(elf: Path, readelf: str, want: str) -> int:
    """取单个 section 的字节数（找不到返回 0）。"""
    proc = subprocess.run([readelf, "-S", str(elf)], capture_output=True, text=True)
    for line in proc.stdout.splitlines():
        parts = line.replace("]", "] ").split()
        # 形如: [ 2] .text PROGBITS 080000c0 0000c0 006250 00 AX 0 0 4
        for i, tok in enumerate(parts):
            if tok == want and i + 4 < len(parts):
                try:
                    return int(parts[i + 4], 16)
                except ValueError:
                    return 0
    return 0


def memory_usage(elf: Path, ld_path: Path) -> tuple[dict[str, int], int]:
    """算各区占用与 .text 大小。返回 ({区名: 字节}, .text 字节)。

    工具缺失或文件不存在时返回 ({}, 0)，调用方自行处理。
    """
    readelf = shutil.which("arm-none-eabi-readelf")
    elf, ld_path = Path(elf), Path(ld_path)
    if not readelf or not elf.is_file():
        return {}, 0

    regions = read_memory_regions(ld_path)
    if not regions:
        return {}, 0

    segs = _load_segments(elf, readelf)
    used: dict[str, int] = {}
    for name, (org, length, is_rom) in regions.items():
        end = org
        for vaddr, paddr, filesz, memsz in segs:
            if is_rom:
                if org <= paddr < org + length and filesz:
                    end = max(end, paddr + filesz)
            else:
                if org <= vaddr < org + length and memsz:
                    end = max(end, vaddr + memsz)
        used[name] = end - org
    return used, _section_size(elf, readelf, ".text")


def format_report(used: dict[str, int], regions: dict[str, tuple[int, int, bool]],
                  text_size: int) -> str:
    """排成与 `ld` 构建日志同款的表，方便肉眼/脚本对账。"""
    lines = ["Memory region         Used Size  Region Size  %age Used"]
    for name, (org, length, _) in regions.items():
        u = used.get(name, 0)
        pct = (u / length * 100) if length else 0.0
        unit = "1 MB" if length >= 1024**2 else (
            f"{length // 1024} KB" if length >= 1024 else f"{length} B")
        lines.append(f"{name:>16}: {u:>10} B {unit:>12} {pct:>9.2f}%")
    lines.append(f"{'.text':>16}: {text_size:>10} B")
    return "\n".join(lines)


def check_budget(used: dict[str, int],
                 regions: dict[str, tuple[int, int, bool]]) -> list[str]:
    """返回超预算的条目（空列表 = 通过）。

    两个判据都要过：
      ① 用量 <= BUDGETS 里给该区的上限（缺项视为不检查）；
      ② 用量 <= 链接脚本给该区的实际容量 —— 超了根本链接不过，但显式报出来
         比让 `ld` 抛一堆 relocation 错更好读。
    """
    bad: list[str] = []
    for name, (_, length, _) in regions.items():
        u = used.get(name, 0)
        budget_kb = BUDGETS.get(name)
        if budget_kb is not None and u > budget_kb * 1024:
            bad.append(f"{name}: 已用 {u / 1024:.1f} KB > 预算 {budget_kb} KB"
                       f"（超 {(u - budget_kb * 1024) / 1024:.1f} KB）")
        if u > length:
            bad.append(f"{name}: 已用 {u / 1024:.1f} KB > 段容量 {length / 1024:.1f} KB")
    return bad


# ---------------------------------------------------------------- 命令行
def _resolve_from_board_env(preset: str) -> tuple[Path | None, Path | None]:
    """从 build/<preset>/board.env 取 ELF 与 LD —— 路径的唯一来源是 CMake。"""
    env_file = ROOT / "build" / preset / "board.env"
    if not env_file.is_file():
        return None, None
    vals: dict[str, str] = {}
    for line in env_file.read_text(encoding="utf-8", errors="replace").splitlines():
        if "=" in line and not line.lstrip().startswith("#"):
            k, v = line.split("=", 1)
            vals[k.strip()] = v.strip()
    elf = Path(vals["ELF"]) if "ELF" in vals else None
    ld = Path(vals["LD"]) if "LD" in vals else None
    return elf, ld


def main() -> int:
    ap = argparse.ArgumentParser(description="STM32 固件体积口径 + 预算门禁")
    ap.add_argument("elf", nargs="?", help="固件 .elf（省略则用 --preset 从 board.env 推）")
    ap.add_argument("--ld", help="链接脚本（省略则用 board.env 的 LD）")
    ap.add_argument("--preset", default="Debug", help="CMake 预设名（默认 Debug）")
    ap.add_argument("--check", action="store_true", help="跑预算门禁（超限返回 1）")
    args = ap.parse_args()

    env_elf, env_ld = _resolve_from_board_env(args.preset)
    elf = Path(args.elf) if args.elf else env_elf
    ld = Path(args.ld) if args.ld else env_ld

    if elf is None or ld is None:
        print(f"!! 无法确定固件/链接脚本路径。先 configure：cmake --preset {args.preset}",
              file=sys.stderr)
        return 2
    if not elf.is_file():
        print(f"!! 找不到固件 {elf} —— 先构建：python Tools/fw.py build --preset {args.preset}",
              file=sys.stderr)
        return 2
    if not shutil.which("arm-none-eabi-readelf"):
        print("!! 找不到 arm-none-eabi-readelf（随 gcc 工具链提供）", file=sys.stderr)
        return 2

    used, text_size = memory_usage(elf, ld)
    regions = read_memory_regions(ld)
    if not used:
        print(f"!! 解析失败：链接脚本 {ld} 里没有可识别的 MEMORY 块？", file=sys.stderr)
        return 2

    print(f"固件: {elf}")
    print(f"链接脚本: {ld}")
    print(format_report(used, regions, text_size))

    if not args.check:
        return 0

    bad = check_budget(used, regions)
    if bad:
        print("\n!! 体积超预算：", file=sys.stderr)
        for b in bad:
            print(f"   · {b}", file=sys.stderr)
        print(f"\n   预算表在本文件顶部的 BUDGETS（{Path(__file__).name}）。"
              f"\n   ⚠️ 调高上限 = 拆门禁。要调先把理由写进 docs/12 的授权记录。",
              file=sys.stderr)
        return 1
    print("\n体积门禁通过。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
