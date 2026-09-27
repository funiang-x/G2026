#!/usr/bin/env python3
"""cubemx_sync —— `.ioc` 与 `Core/Inc/board_config.h` 的配置对账。

为什么需要它
------------
`.ioc`（CubeMX 的配置）与 `Core/Inc/board_config.h`（板级事实的唯一来源）是
描述**同一批引脚与时钟事实的两个文件**。任何一侧单独改动都会造成
"打开 CubeMX 才发现配置过期"的漂移 —— 而这类漂移**不会报错**：
固件照编、照烧，只是行为和你以为的不一样。

⇒ 这是本工程唯一保留的静态检查项（其余由编译器与 CMake 强制，见 docs/00 §1.3）。

判定口径
--------
· `--strict`（`fw.py lint` 用的就是它）：只拦**确定性矛盾** —— 两边都有值且不等。
· 单侧有值（例如 .ioc 里没有这个外设）：**提示但不拦**。理由：单侧有值是
  "还没配"或"已废弃"的正常状态，拦下来会逼着人写一堆占位宏。
· 默认（不加 `--strict`）：全部只提示，退出码恒为 0。方便手工看一眼。

⚠️ 它**不**检查：分层、孤儿宏、文档引用 —— 那些要么由编译器强制，
   要么属于纯开销（docs/00 §1.4）。"lint 通过"绝不等于"工程没问题"。

用法
----
    python Tools/cubemx_sync.py            # 只报告
    python Tools/cubemx_sync.py --strict   # 有确定性矛盾则退出码 1
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# 端口字母 → 数字（board_config.h 里用的是数字，避免 "PA9" 这种字符串）
PORT_INDEX = {"A": 0, "B": 1, "C": 2, "D": 3, "E": 4, "F": 5, "G": 6, "H": 7, "I": 8}


# ------------------------------------------------------------------ 解析
def parse_ioc(path: Path) -> dict[str, str]:
    """读 .ioc 的 `KEY=VALUE` 行。值里的 `\\:` 是 CubeMX 的转义，还原成 `:`。"""
    vals: dict[str, str] = {}
    if not path.is_file():
        return vals
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        k, v = line.split("=", 1)
        vals[k.strip()] = v.replace("\\:", ":")
    return vals


def parse_board_macros(path: Path) -> dict[str, str]:
    """读 board_config.h 里形如 `#define BOARD_X 123U` / `#define BOARD_X "abc"` 的宏。"""
    macros: dict[str, str] = {}
    if not path.is_file():
        return macros
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        m = re.match(r"\s*#\s*define\s+(BOARD_\w+)\s+(.+)$", line)
        if not m:
            continue
        name, raw = m.groups()
        raw = re.sub(r"/\*.*", "", raw)   # 先剥行尾注释，再剥引号/后缀（顺序不能反）
        raw = re.sub(r"//.*", "", raw).strip()
        raw = re.sub(r"[uUlL]+$", "", raw)
        macros[name] = raw.strip('"')
    return macros


def parse_hal_conf_macro(path: Path, name: str) -> str | None:
    """从 `stm32f4xx_hal_conf.h` 里取一个宏的**生效值**（已归一化）。

    ⚠️ 必须**先剥掉 `/* */` 块注释再匹配**：CubeMX 会把没启用的模块宏**注释掉**
    （`/* #define HAL_ADC_MODULE_ENABLED */`），只按行首匹配会把注释掉的当成生效的 ——
    那是这类检查最经典的假阴性。
    ⚠️ 还要剥掉 `U`/`L` 后缀：`8000000U` 直接 `int(..., 0)` 会抛 ValueError，
      被 `_num()` 吞成 None ⇒ 检查**退化成"仅一边有值"的提示**，看起来通过了、
      其实没在查。这个坑 2026-09-21 实测踩到（HSE_VALUE 检查第一次跑就是这症状）。
    """
    if not path.is_file():
        return None
    text = path.read_text(encoding="utf-8", errors="replace")
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)   # 块注释
    text = re.sub(r"//.*", "", text)                     # 行注释
    m = re.search(rf"^\s*#\s*define\s+{name}\s+([^\r\n]+)", text, re.M)
    if not m:
        return None
    val = m.group(1).strip()
    val = re.sub(r"[uUlL]+$", "", val)   # 8000000U -> 8000000
    return val.strip().strip('"')


def parse_ld_regions(path: Path) -> dict[str, int]:
    """读链接脚本 MEMORY 段的 {区名: 字节数}（复用体积模块，口径不重复实现）。"""
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import stm32_size  # noqa: PLC0415

    return {name: length for name, (_, length, _) in
            stm32_size.read_memory_regions(path).items()}


def ioc_pin_signal(ioc: dict[str, str], signal: str) -> tuple[int, int] | None:
    """按 .ioc 的 `<引脚>.Signal=<信号>` 反查引脚号，返回 (端口号, 引脚号)。

    形如 `PA9.Signal=USART1_TX` → (0, 9)。
    """
    for key, val in ioc.items():
        if not key.endswith(".Signal") or val != signal:
            continue
        m = re.match(r"^(P[A-I])(\d+)$", key[: -len(".Signal")])
        if m:
            return PORT_INDEX[m.group(1)[1]], int(m.group(2))
    return None


def ioc_gpio_output_pin(ioc: dict[str, str], want: tuple[int, int]) -> bool:
    """某个引脚在 .ioc 里是不是配成了 GPIO_Output。"""
    for key, val in ioc.items():
        if not key.endswith(".Signal") or val != "GPIO_Output":
            continue
        m = re.match(r"^(P[A-I])(\d+)$", key[: -len(".Signal")])
        if m and (PORT_INDEX[m.group(1)[1]], int(m.group(2))) == want:
            return True
    return False


# ------------------------------------------------------------------ 检查
class Result:
    def __init__(self) -> None:
        self.rows: list[tuple[str, str, str]] = []  # (状态, 项目, 说明)
        self.failed = 0
        self.warned = 0

    def ok(self, item: str, detail: str) -> None:
        self.rows.append(("OK", item, detail))

    def fail(self, item: str, detail: str) -> None:
        self.rows.append(("FAIL", item, detail))
        self.failed += 1

    def warn(self, item: str, detail: str) -> None:
        self.rows.append(("--", item, detail))
        self.warned += 1

    def report(self) -> None:
        width = max((len(r[1]) for r in self.rows), default=8)
        for status, item, detail in self.rows:
            print(f"  [{status}] {item:<{width}}  {detail}")


def _num(text: str) -> int | None:
    try:
        return int(text, 0)
    except (TypeError, ValueError):
        return None


def _cmp_int(res: Result, item: str, ioc_val: str | None, board_val: str | None,
             unit: str = "") -> None:
    """比一个整数：两边都有值且不等 → FAIL；只有一边有值 → 提示。"""
    a, b = _num(ioc_val or ""), _num(board_val or "")
    if a is None and b is None:
        res.warn(item, "两边都没有值")
    elif a is None:
        res.warn(item, f"仅 board_config.h 有值：{b}{unit}")
    elif b is None:
        res.warn(item, f"仅 .ioc 有值：{a}{unit}")
    elif a == b:
        res.ok(item, f"{a}{unit}")
    else:
        res.fail(item, f".ioc={a}{unit}  board_config.h={b}{unit}  ← 不一致")


def _cmp_str(res: Result, item: str, ioc_val: str | None, board_val: str | None) -> None:
    if not ioc_val and not board_val:
        res.warn(item, "两边都没有值")
    elif not ioc_val:
        res.warn(item, f"仅 board_config.h 有值：{board_val}")
    elif not board_val:
        res.warn(item, f"仅 .ioc 有值：{ioc_val}")
    elif ioc_val == board_val:
        res.ok(item, ioc_val)
    else:
        res.fail(item, f".ioc={ioc_val}  board_config.h={board_val}  ← 不一致")


def _cmp_pin(res: Result, item: str, ioc: dict[str, str], signal: str,
             port_macro: str, pin_macro: str, m: dict[str, str]) -> None:
    """比一个"信号 ↔ 引脚号"。.ioc 用 Signal 反查，board_config.h 用两个宏。"""
    got = ioc_pin_signal(ioc, signal)
    want_port, want_pin = _num(m.get(port_macro, "")), _num(m.get(pin_macro, ""))
    if got is None and want_port is None:
        res.warn(item, "两边都没有值")
    elif got is None:
        res.warn(item, f"仅 board_config.h 有值：port={want_port} pin={want_pin}")
    elif want_port is None or want_pin is None:
        res.warn(item, f"仅 .ioc 有值：{signal} → port={got[0]} pin={got[1]}")
    elif got == (want_port, want_pin):
        res.ok(item, f"{signal} = P{'ABCDEFGHI'[got[0]]}{got[1]}")
    else:
        res.fail(item, f".ioc: {signal} → port={got[0]} pin={got[1]}  "
                       f"board_config.h: port={want_port} pin={want_pin}  ← 不一致")


def check(root: Path) -> Result:
    res = Result()

    ioc_path = next(iter(root.glob("*.ioc")), None)
    board_path = root / "Core" / "Inc" / "board_config.h"
    if ioc_path is None:
        res.fail(".ioc", f"{root} 下找不到 .ioc")
        return res
    if not board_path.is_file():
        res.fail("board_config.h", f"找不到 {board_path}")
        return res

    ioc = parse_ioc(ioc_path)
    m = parse_board_macros(board_path)
    print(f"  .ioc            {ioc_path.name}")
    print(f"  board_config.h  {board_path.relative_to(root).as_posix()}")
    print()

    # ---- 芯片身份 ----
    _cmp_str(res, "MCU 型号", ioc.get("Mcu.UserName"), m.get("BOARD_MCU_NAME"))

    # J-Link 器件名：CubeMX 给的是完整 CPN（如 STM32F407VGT6），J-Link 用短名
    # （STM32F407VG）。所以判据是"前缀"而不是相等。
    cpn, jlink = ioc.get("Mcu.CPN", ""), m.get("BOARD_JLINK_DEVICE", "")
    if not cpn or not jlink:
        res.warn("J-Link 器件名", f"仅一边有值：CPN={cpn or '-'} JLINK={jlink or '-'}")
    elif cpn.startswith(jlink):
        res.ok("J-Link 器件名", f"{jlink} ⊂ {cpn}")
    else:
        res.fail("J-Link 器件名", f"CPN={cpn} 不以 {jlink} 开头  ← 烧录会连错器件")

    # ---- 时钟 ----
    _cmp_int(res, "HSE 晶振", ioc.get("RCC.HSE_VALUE"), m.get("BOARD_HSE_VALUE"), " Hz")
    _cmp_int(res, "目标 SYSCLK", ioc.get("RCC.SYSCLKFreq_VALUE"),
             m.get("BOARD_TARGET_SYSCLK_HZ"), " Hz")

    # ---- HSE_VALUE 的第三处：HAL 自己算时钟用的那份 ----
    # ★ 为什么必须查它：`board_config.h` 里写着"HSE 一致性由 fw.py lint 兜底"，
    #   而原先 lint 只比 .ioc ↔ board_config.h —— **从不读 hal_conf.h**。
    #   HAL 的 `HAL_RCC_GetSysClockFreq()` 用的是 `HSE_VALUE`，它错了则
    #   SystemCoreClock / 所有外设时钟推导（波特率、SPI 分频、定时器）全错，
    #   而**编译不报错**。这是"文档承诺了但没实现"的典型，2026-09-21 补齐。
    hal_hse = parse_hal_conf_macro(root / "Core" / "Inc" / "stm32f4xx_hal_conf.h",
                                   "HSE_VALUE")
    ioc_hse = ioc.get("RCC.HSE_VALUE")
    if hal_hse is None:
        res.fail("HAL 的 HSE_VALUE",
                 "Core/Inc/stm32f4xx_hal_conf.h 里没有生效的 `#define HSE_VALUE` "
                 "—— HAL 算时钟全靠它（缺失会编译报错，别删）")
    else:
        _cmp_int(res, "HAL 的 HSE_VALUE", ioc_hse, hal_hse, " Hz")

    # PLL 参数自洽：SYSCLK = HSE / M × N / P（P=2 固定）
    hse, pllm, plln = (_num(ioc.get("RCC.HSE_VALUE", "")),
                       _num(ioc.get("RCC.PLLM", "")), _num(ioc.get("RCC.PLLN", "")))
    sysclk = _num(ioc.get("RCC.SYSCLKFreq_VALUE", ""))
    if hse and pllm and plln and sysclk:
        derived = hse // pllm * plln // 2
        if derived == sysclk:
            res.ok("PLL 自洽", f"{hse}/{pllm}×{plln}/2 = {derived} Hz")
        else:
            res.fail("PLL 自洽", f"由 M/N 推出 {derived} Hz，但 .ioc 写着 {sysclk} Hz")
    else:
        res.warn("PLL 自洽", "HSE / PLLM / PLLN / SYSCLK 有缺项，跳过")

    # ---- 引脚（.ioc 的 Signal ↔ board_config.h 的两个宏）----
    _cmp_pin(res, "日志串口 TX", ioc, "USART1_TX",
             "BOARD_LOG_UART_TX_PORT", "BOARD_LOG_UART_TX_PIN", m)
    _cmp_pin(res, "日志串口 RX", ioc, "USART1_RX",
             "BOARD_LOG_UART_RX_PORT", "BOARD_LOG_UART_RX_PIN", m)
    _cmp_pin(res, "SPI2 SCK", ioc, "SPI2_SCK",
             "BOARD_SPI2_SCK_PORT", "BOARD_SPI2_SCK_PIN", m)
    _cmp_pin(res, "SPI2 MISO", ioc, "SPI2_MISO",
             "BOARD_SPI2_MISO_PORT", "BOARD_SPI2_MISO_PIN", m)
    _cmp_pin(res, "SPI2 MOSI", ioc, "SPI2_MOSI",
             "BOARD_SPI2_MOSI_PORT", "BOARD_SPI2_MOSI_PIN", m)
    _cmp_pin(res, "USB DM", ioc, "USB_OTG_FS_DM", "BOARD_USB_OTG_FS_DM_PORT",
             "BOARD_USB_OTG_FS_DM_PIN", m)
    _cmp_pin(res, "USB DP", ioc, "USB_OTG_FS_DP", "BOARD_USB_OTG_FS_DP_PORT",
             "BOARD_USB_OTG_FS_DP_PIN", m)

    # ---- 心跳灯：.ioc 里必须真的是 GPIO_Output ----
    led_port, led_pin = _num(m.get("BOARD_LED_PORT", "")), _num(m.get("BOARD_LED_PIN", ""))
    if led_port is None or led_pin is None:
        res.warn("心跳灯", "board_config.h 里没有 BOARD_LED_PORT/PIN")
    elif ioc_gpio_output_pin(ioc, (led_port, led_pin)):
        res.ok("心跳灯", f"P{'ABCDEFGHI'[led_port]}{led_pin} = GPIO_Output")
    else:
        res.fail("心跳灯", f"board_config.h 说灯在 P{'ABCDEFGHI'[led_port]}{led_pin}，"
                           f"但 .ioc 里它不是 GPIO_Output  ← 灯不会亮")

    # ---- 内存容量：board_config.h 与链接脚本必须一致 ----
    ld_path = next(iter(root.glob("*.ld")), None)
    if ld_path is None:
        res.fail("链接脚本", f"{root} 下找不到 .ld")
    else:
        regions = parse_ld_regions(ld_path)
        for macro, region in (("BOARD_FLASH_KB", "FLASH"),
                              ("BOARD_RAM_KB", "RAM"),
                              ("BOARD_CCM_KB", "CCMRAM")):
            kb = _num(m.get(macro, ""))
            ld_kb = regions.get(region)
            ld_kb = ld_kb // 1024 if ld_kb else None
            if kb is None or ld_kb is None:
                res.warn(f"{region} 容量", f"仅一边有值：{macro}={kb} .ld={ld_kb}")
            elif kb == ld_kb:
                res.ok(f"{region} 容量", f"{kb} KB")
            else:
                res.fail(f"{region} 容量",
                         f"{macro}={kb} KB  但 .ld 的 {region} 段是 {ld_kb} KB")

    # ---- CubeMX 的默认任务栈：.ioc 与生成物 freertos.c 必须一致 ----
    # 判据：.ioc 的 FREERTOS.Tasks01=<名>,<优先级>,<栈words>,... 与
    #       Core/Src/freertos.c 的 `.stack_size = <words> * 4`
    # ⚠️ 这一项能抓到"改了 .ioc 但没重新生成" —— 那是最隐蔽的一种漂移。
    tasks = ioc.get("FREERTOS.Tasks01", "")
    parts = tasks.split(",") if tasks else []
    fr_path = root / "Core" / "Src" / "freertos.c"
    if len(parts) < 3 or not fr_path.is_file():
        res.warn("默认任务栈", "缺 FREERTOS.Tasks01 或 freertos.c，跳过")
    else:
        want = _num(parts[2])
        m2 = re.search(r"\.stack_size\s*=\s*(\d+)\s*\*\s*4", fr_path.read_text(
            encoding="utf-8", errors="replace"))
        got = _num(m2.group(1)) if m2 else None
        if want is None or got is None:
            res.warn("默认任务栈", "解析不出栈深，跳过")
        elif want == got:
            res.ok("默认任务栈", f"{want} words = {want * 4} B（freertos.c 与 .ioc 一致）")
        else:
            res.fail("默认任务栈",
                     f".ioc={want} words  freertos.c={got} words  ← 改了 .ioc 但没重新生成？")

    return res


def main() -> int:
    ap = argparse.ArgumentParser(description=".ioc 与 board_config.h 的配置对账")
    ap.add_argument("--strict", action="store_true",
                    help="有确定性矛盾则退出码 1（fw.py lint 用这个）")
    ap.add_argument("--root", default=str(ROOT), help="工程根（默认脚本上一级）")
    args = ap.parse_args()

    res = check(Path(args.root))
    res.report()
    print()
    if res.failed:
        print(f"!! {res.failed} 处确定性矛盾（{res.warned} 处提示）。")
        print("   修法：让 .ioc 与 board_config.h 表达同一件事 —— "
              "改 .ioc 就在 CubeMX 里改并重新生成，改 board_config.h 就直接改。")
        return 1 if args.strict else 0
    print(f"对账通过（{res.warned} 处单侧有值的提示，不影响判定）。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
