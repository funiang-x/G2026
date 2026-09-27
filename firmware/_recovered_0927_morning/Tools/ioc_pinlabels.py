#!/usr/bin/env python3
"""给 `.ioc` 里的引脚批量补 User Label —— 让 CubeMX 的引脚图能看出"这块板上它是干什么的"。

为什么需要它
------------
CubeMX 引脚图只显示**外设信号名**（SPI1_SCK / SDIO_D0 / USART1_TX），
看不出语义：PA4 是个普通 GPIO_Output，其实是 W25Q128 的软件片选；
PC8..PC12 是 SDIO，其实是板载 TF 卡座。User Label 补的就是这层语义，
但 GUI 只能一个引脚一个引脚地点（本工程 24 个）。

⚠️ **外设复用引脚的 label 必须两行一起写**（2026-09-26 实测，CubeMX 6.15.0）：
      PA6.GPIOParameters=GPIO_Label
      PA6.GPIO_Label=FLASH_MISO
    只写 `GPIO_Label` 一行会被 CubeMX **静默删除** —— 不报错、不警告，
    重新生成后 label 凭空消失，且 `git diff` 上看着像"从没改过"。

    实验记录（同一份 .ioc，一次加载对比三种情况）：
      PA5  只写 GPIO_Label        → 加载后被删，main.h 无宏
      PA6  写 GPIOParameters+Label → 保留，main.h 生成 TESTFMT_B_Pin/GPIO_Port
      PA7  不写（对照）            → 无变化
    ⇒ 所以本脚本对**没有 GPIOParameters 行**的引脚会新加一行；
      对**已有**该行的引脚（GPIO 模式，如 PA0-WKUP / PA4）则把 `GPIO_Label`
      **追加**进那个逗号列表 —— 追加顺序必须与 CubeMX 的字母序一致。

⚠️ label 会变成 `Core/Inc/main.h` 里的宏（`<label>_Pin` / `<label>_GPIO_Port`）。
   本工程代码取的是 `board_config.h` 的 `BOARD_*` 宏，不依赖这些生成宏，
   所以加 label **不影响现有代码**；但名字仍不要与已有宏重名。

用法
----
    python Tools/ioc_pinlabels.py            # 只预览差异，不写文件
    python Tools/ioc_pinlabels.py --write    # 写入（先自动备份 .ioc）

改完要**在 CubeMX 里重新加载工程**才能看到（File → Load Project）。
⚠️ 如果 CubeMX 已经开着这个工程，**先关掉它再加载**，否则它内存里的旧状态
   会在你按保存时把改动覆盖回去。
"""
import argparse
import re
import shutil
import sys
from datetime import date
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ioc_common import check_pins, parse_pins  # noqa: E402  (注入路径后才能 import)

ROOT = Path(__file__).resolve().parent.parent

# 引脚 → User Label。事实来源是 Core/Inc/board_config.h（本文件只是搬运工）。
# ⚠️ 已有 label 的引脚（PB2=green_led / PD3=SD_CARD_DET）**不在这里** —— 那是
#    用户自己起的名字，脚本不覆盖，保持原样。
PIN_LABELS = {
    # SPI1 —— 板载 W25Q128 SPI Flash（16MB）。PA4 是**软件片选**，不是硬件 NSS。
    "PA4": "FLASH_CS",
    "PA5": "FLASH_SCK",
    "PA6": "FLASH_MISO",
    "PA7": "FLASH_MOSI",
    # ⚠️ SPI2（PB10 / PC2 / PC3）**已从模板移除**（2026-09-26）——
    #    本工程定位成"只留板载器件"的模板，SPI2 是排针上的自由 IO，没有对应器件。
    #    ⇒ 这里也删掉，否则每次跑都报"找不到这个引脚"。
    #    派生工程要把 SPI2 开回来时：先在那边的 CubeMX 里配好，再把这三条加回来
    #    （排针位置见 Core/Inc/board_config.h 的"排针上的自由 IO"一节）。
    # USART1 —— 日志出口（默认后端是 RTT，这是 --preset Debug-UART 用的那组）
    "PA9": "LOG_TX",
    "PA10": "LOG_RX",
    # USB_OTG_FS —— Device Only
    "PA11": "USB_DM",
    "PA12": "USB_DP",
    # SDIO —— 板载 TF 卡座，4-bit 宽总线
    "PC8": "SD_D0",
    "PC9": "SD_D1",
    "PC10": "SD_D2",
    "PC11": "SD_D3",
    "PC12": "SD_CK",
    "PD2": "SD_CMD",
    # 用户按键 —— 板上 SW2，按下为高（外部 10k 下拉）
    "PA0-WKUP": "KEY_SW2",
    # 调试口 —— 不要复用为 GPIO，否则烧录失联
    "PA13": "SWDIO",
    "PA14": "SWCLK",
    # 时钟源
    "PH0-OSC_IN": "HSE_IN",
    "PH1-OSC_OUT": "HSE_OUT",
    "PC14-OSC32_IN": "LSE_IN",
    "PC15-OSC32_OUT": "LSE_OUT",
}


def add_label(lines: list[str], pin: str, label: str) -> tuple[list[str], str | None]:
    """给一个引脚补 label。返回 (新的行列表, 错误说明或 None)。

    只在**该引脚自己的行块**里动刀（连续 `PIN.xxx=` 行），不碰别的引脚。
    """
    idxs = [i for i, ln in enumerate(lines) if ln.startswith(pin + ".")]
    if not idxs:
        return lines, "在 .ioc 里找不到这个引脚"
    start, end = min(idxs), max(idxs) + 1
    if end - start != len(idxs):
        return lines, "该引脚的行块不连续（中间夹了别的行），跳过"

    block = lines[start:end]
    gpi = f"{pin}.GPIOParameters="
    lbl = f"{pin}.GPIO_Label={label}"

    at = next((i for i, ln in enumerate(block) if ln.startswith(gpi)), None)
    if at is None:
        # 外设复用引脚：本来没有 GPIOParameters 行 ⇒ 必须新加一行，
        # 否则下面那行 GPIO_Label 会被 CubeMX 静默删掉。
        block.insert(0, gpi + "GPIO_Label")
    else:
        cur = block[at].split("=", 1)[1]
        if "GPIO_Label" not in [p.strip() for p in cur.split(",")]:
            block[at] = gpi + cur + ",GPIO_Label"

    # 按字母序插入 label 行 —— 与 CubeMX 自己保存的顺序一致，避免制造假 diff
    pos = next((i for i, ln in enumerate(block) if ln > lbl), len(block))
    block.insert(pos, lbl)

    return lines[:start] + block + lines[end:], None


def main() -> int:
    ap = argparse.ArgumentParser(description="给 .ioc 的引脚批量补 User Label")
    ap.add_argument("--write", action="store_true", help="写入工程 .ioc（默认只预览）")
    args = ap.parse_args()

    ioc = next(iter(ROOT.glob("*.ioc")), None)
    if ioc is None:
        print(f"在 {ROOT} 下找不到 .ioc")
        return 1

    # universal newlines：CRLF → LF，最后统一写回 CRLF
    text = ioc.read_text(encoding="utf-8")
    lines = text.split("\n")

    errs = check_pins(text, "输入")
    if errs:
        print("输入 .ioc 本身有问题，先修它：")
        for e in errs:
            print("  -", e)
        return 1

    already = set(re.findall(r"^([A-Za-z0-9_\-]+)\.GPIO_Label=", text, re.M))
    print(f"输入 {ioc.name} · 引脚编号 0..{len(parse_pins(text)) - 1} 连续无重复")
    if already:
        print(f"已有 label（保持不动）：{'、'.join(sorted(already))}")
    print()

    # 从后往前处理，避免前面的插入让后面的行号失效
    todo = []
    for pin, label in PIN_LABELS.items():
        if pin in already:
            continue
        idxs = [i for i, ln in enumerate(lines) if ln.startswith(pin + ".")]
        todo.append((min(idxs) if idxs else 1 << 30, pin, label))
    todo.sort(reverse=True)

    added, failed = [], []
    for _, pin, label in todo:
        new_lines, err = add_label(lines, pin, label)
        if err:
            failed.append((pin, err))
        else:
            lines = new_lines
            added.append((pin, label))

    if added:
        print(f"将补上 {len(added)} 个 label：")
        for pin, label in sorted(added):
            print(f"  + {pin:<16} {label}")
    if failed:
        print(f"\n⚠️ {len(failed)} 个引脚跳过：")
        for pin, err in sorted(failed):
            print(f"  - {pin:<16} {err}")

    out = "\n".join(lines)
    errs = check_pins(out, "输出")
    if errs:
        print("\n输出校验失败（不写文件）：")
        for e in errs:
            print("  -", e)
        return 1
    print(f"\n输出校验通过：引脚编号 0..{len(parse_pins(out)) - 1} 连续无重复")

    if not args.write:
        print("\n（预览模式，未写文件。加 --write 才写入。）")
        return 0

    if out == text:
        print("\n内容无变化，不写文件。")
        return 0

    bak = ioc.with_name(f"{ioc.name}.bak_{date.today():%Y%m%d}_pinlabels")
    shutil.copy2(ioc, bak)
    ioc.write_text(out, encoding="utf-8", newline="\r\n")  # 必须 CRLF
    print(f"\n[备份] {bak.name}")
    print(f"[写入] {ioc}")
    print("下一步：在 CubeMX 里重新加载工程（File → Load Project）看引脚图。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
