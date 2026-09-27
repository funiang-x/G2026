#!/usr/bin/env python3
"""在 `.ioc` 上「追加」外设配置 —— 只追加，**绝不重排**既有编号。

    python Tools/ioc_append.py <输出.ioc> <外设> [<外设> ...]

    外设可选：spi1 · sdio · nvic

输入固定是工程根的 `*.ioc`，输出写到指定路径（**不覆盖输入**）。
生成后必须交给 CubeMX 仲裁 + 生成代码，见 `docs/guides/01` §5：

    cd <CubeMX 安装目录>
    jre/bin/java -jar STM32CubeMX.exe -q <脚本>
    # 脚本内容：config load <输出.ioc> / project generate / exit

⚠️ 本脚本**不是**门禁，是配置辅助工具（`Tools/` 下的其它脚本才是门禁）。
⚠️ 为什么不让它直接改工程 `.ioc`：那属于"改 `.ioc`"，按 `docs/12` 要先说再做。
   本脚本只产出候选文件，由人决定是否采用。
"""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ioc_common import check_pins, parse_pins  # noqa: E402  (注入路径后才能 import)

ROOT = Path(__file__).resolve().parent.parent

SPI1_PINS = {"PA5": "SPI1_SCK", "PA6": "SPI1_MISO", "PA7": "SPI1_MOSI"}
SDIO_PINS = {"PC8": "SDIO_D0", "PC9": "SDIO_D1", "PC10": "SDIO_D2",
             "PC11": "SDIO_D3", "PC12": "SDIO_CK", "PD2": "SDIO_CMD"}

SPI1_CFG = ("SPI1.CalculateBaudRate=42.0 MBits/s\n"
            "SPI1.Direction=SPI_DIRECTION_2LINES\n"
            "SPI1.IPParameters=VirtualType,Mode,Direction,CalculateBaudRate\n"
            "SPI1.Mode=SPI_MODE_MASTER\n"
            "SPI1.VirtualType=VM_MASTER\n")

SDIO_CFG = ("SDIO.ClockDiv=0\n"
            "SDIO.IPParameters=ClockDiv,VirtualMode\n"
            "SDIO.VirtualMode=SD_4_bits_Wide_bus\n")

NVIC_CFG = ("NVIC.OTG_FS_IRQn=true\\:5\\:0\\:false\\:false\\:true\\:false\\:true\\:true\\:true\n"
            "NVIC.SDIO_IRQn=true\\:5\\:0\\:false\\:false\\:true\\:false\\:true\\:true\\:true\n")

# PD3 = TF 卡座的卡检测脚。板上经 R20（47k）上拉到 3V3，插卡接地 ⇒ 低电平。
PD3_CFG = ("PD3.GPIOParameters=GPIO_PuPd,GPIO_Label\n"
           "PD3.GPIO_PuPd=GPIO_PULLUP\n"
           "PD3.GPIO_Label=SD_CARD_DET\n"
           "PD3.Locked=true\n"
           "PD3.Signal=GPIO_Input\n")

# RTC：只要"激活时钟源"。
# ⚠️ RTC 的模式是用**虚拟引脚**表达的，不是 `RTC.VirtualMode` ——
#    RTC 的 IP 定义里写的是 `<Mode Name="RTC_Enabled"><Signal Name="VS_RTC_Activate"/>`，
#    所以 .ioc 里是 `VP_RTC_VS_RTC_Activate`，并且**要进 Mcu.PinN 列表**
#    （跟 `VP_FREERTOS_VS_CMSIS_V2` / `VP_SYS_VS_tim6` 一个道理）。
#    实测：只写 `RTC.VirtualMode=RTC_Enabled` 时 CubeMX 会静默忽略，
#    `Mcu.IPNb` 也不加 —— 不生成 rtc.c，也没有 MX_RTC_Init()。
# ⚠️ RTC 的时钟源（LSE）在 RCC 侧，本脚本不碰 —— 见 docs/guides/01 §5。
RTC_CFG = ("VP_RTC_VS_RTC_Activate.Mode=RTC_Enabled\n"
           "VP_RTC_VS_RTC_Activate.Signal=RTC_VS_RTC_Activate\n")


def append_pins(text, new_pins):
    n_old = len(parse_pins(text))
    anchor = re.search(rf"^Mcu\.Pin{n_old - 1}=.*$", text, re.M).group(0)
    block = "".join(f"Mcu.Pin{n_old + i}={p}\n" for i, p in enumerate(new_pins))
    text = text.replace(anchor + "\n", anchor + "\n" + block, 1)
    return text.replace(f"Mcu.PinsNb={n_old}", f"Mcu.PinsNb={n_old + len(new_pins)}", 1)


def add_ip(text, name):
    ipnb = int(re.search(r"^Mcu\.IPNb=(\d+)$", text, re.M).group(1))
    anchor = re.search(rf"^Mcu\.IP{ipnb - 1}=.*$", text, re.M).group(0)
    text = text.replace(anchor + "\n", anchor + f"\nMcu.IP{ipnb}={name}\n", 1)
    return text.replace(f"Mcu.IPNb={ipnb}", f"Mcu.IPNb={ipnb + 1}", 1)


def add_signals(text, pins, mode):
    if re.search(rf"^{re.escape(next(iter(pins)))}\.Mode=", text, re.M):
        return text
    block = "".join(f"{p}.Mode={mode}\n{p}.Signal={s}\n" for p, s in pins.items())
    anchor = re.search(r"^PA4\.Signal=GPIO_Output$", text, re.M).group(0)
    return text.replace(anchor + "\n", anchor + "\n" + block, 1)


def add_fn(text, entry):
    """往 `ProjectManager.functionlistsort` 末尾追加一条初始化函数。

    ⚠️ 编号必须用「现有条目数 + 1」算。实测踩过：拿 `\\d+-MX_` 正则去数条目
    会漏掉 `1-SystemClock_Config-RCC-...`（它不以 `MX_` 开头），
    于是算出**重复编号**（两个 7）。CubeMX 加载时能自己纠正，
    但生成物里会留着错编号，下次 `git diff` 看着莫名其妙。"""
    m = re.search(r"^ProjectManager\.functionlistsort=(.*)$", text, re.M)
    if not m:
        return text
    if entry.split("-", 1)[1].split("-")[0] in m.group(1):
        return text
    n = len(m.group(1).split(",")) + 1
    return text.replace(m.group(0), f"{m.group(0)},{n}-{entry}", 1)


def norm_path(p):
    """Git Bash 的 `/c/Users/...` 转成 Windows 的 `C:/Users/...`。

    实测踩过：直接 `Path("/c/Users/...")` 在 Windows Python 下会变成
    `\\c\\Users\\...`，写文件时报 FileNotFoundError。"""
    m = re.match(r"^/([a-zA-Z])/(.*)$", p)
    return Path(f"{m.group(1).upper()}:/{m.group(2)}") if m else Path(p)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    dst = norm_path(sys.argv[1])
    wants = [w.lower() for w in sys.argv[2:]]

    src = next(iter(ROOT.glob("*.ioc")), None)
    if src is None:
        print(f"在 {ROOT} 下找不到 .ioc")
        return 1
    text = src.read_text(encoding="utf-8")

    base = check_pins(text, "输入")
    if base:
        print("输入文件本身有问题：")
        for e in base:
            print("  -", e)
        return 1
    print(f"[输入] {src.name} · 引脚编号 0..{len(parse_pins(text)) - 1} 连续无重复")

    if "spi1" in wants and not re.search(r"^Mcu\.IP\d+=SPI1$", text, re.M):
        text = add_signals(add_ip(append_pins(text, SPI1_PINS), "SPI1"),
                           SPI1_PINS, "Full_Duplex_Master")
        text = text.replace("SPI2.CalculateBaudRate=", SPI1_CFG + "SPI2.CalculateBaudRate=", 1)
        text = add_fn(text, "MX_SPI1_Init-SPI1-false-HAL-true")
        print("[加入] SPI1（PA5/6/7，AF5）")

    if "sdio" in wants and not re.search(r"^Mcu\.IP\d+=SDIO$", text, re.M):
        text = add_signals(add_ip(append_pins(text, SDIO_PINS), "SDIO"),
                           SDIO_PINS, "SD_4_bits_Wide_bus")
        text = text.replace("SPI1.CalculateBaudRate=", SDIO_CFG + "SPI1.CalculateBaudRate=", 1)
        text = add_fn(text, "MX_SDIO_SD_Init-SDIO-false-HAL-true")
        print("[加入] SDIO（PC8/9/10/11/12 + PD2，AF12）")

    if "nvic" in wants and "NVIC.SDIO_IRQn" not in text:
        anchor = re.search(r"^NVIC\.PendSV_IRQn=.*$", text, re.M).group(0)
        text = text.replace(anchor + "\n", NVIC_CFG + anchor + "\n", 1)
        print("[加入] NVIC：SDIO_IRQn + OTG_FS_IRQn（抢占优先级 5）")

    if "pd3" in wants and "PD3.Signal=" not in text:
        text = append_pins(text, {"PD3": "GPIO_Input"})
        anchor = re.search(r"^PA4\.Signal=GPIO_Output$", text, re.M)
        text = text.replace(anchor.group(0) + "\n", anchor.group(0) + "\n" + PD3_CFG, 1)
        print("[加入] PD3（TF 卡检测：GPIO_Input + 上拉，Label=SD_CARD_DET）")

    if "rtc" in wants and "VP_RTC_VS_RTC_Activate.Signal" not in text:
        text = append_pins(text, {"VP_RTC_VS_RTC_Activate": "VS_RTC_Activate"})
        text = add_ip(text, "RTC")
        anchor = re.search(r"^VP_SYS_VS_tim6\.Mode=.*$", text, re.M).group(0)
        text = text.replace(anchor + "\n", RTC_CFG + anchor + "\n", 1)
        if "RTCClockSelectionARG" not in text:
            m = re.search(r"^RCC\.IPParameters=(.*)$", text, re.M)
            text = text.replace(
                m.group(0),
                f"RCC.IPParameters=RTCClockSelection,RTCClockSelectionARG,{m.group(1)}", 1)
            a2 = re.search(r"^RCC\.PLLM=.*$", text, re.M).group(0)
            text = text.replace(
                a2 + "\n", a2 + "\n"
                + "RCC.RTCClockSelection=RCC_RTCCLKSOURCE_LSE\n"
                + "RCC.RTCClockSelectionARG=RCC_RTCCLKSOURCE_LSE\n", 1)
        text = add_fn(text, "MX_RTC_Init-RTC-false-HAL-true")
        print("[加入] RTC（虚拟引脚 + RCC 时钟源 LSE）")

    after = check_pins(text, "输出")
    if after:
        print("输出校验失败：")
        for e in after:
            print("  -", e)
        return 1
    print(f"[输出] 引脚编号 0..{len(parse_pins(text)) - 1} 连续无重复")

    # ⚠️ 必须写 CRLF：CubeMX 自己保存的 .ioc 是 CRLF（实测 228 行 = 228 个 CR）。
    # 写 LF 也能被 CubeMX 读，但会制造满屏假 diff（"每行都变了"其实只差行尾）。
    # read_text 默认做 universal newlines（CRLF→LF），所以这里统一转回 CRLF。
    dst.write_text(text, encoding="utf-8", newline="\r\n")
    print(f"[写出] {dst}")
    print("下一步：交给 CubeMX 仲裁 + 生成，见 docs/guides/01 §5")
    return 0


if __name__ == "__main__":
    sys.exit(main())
