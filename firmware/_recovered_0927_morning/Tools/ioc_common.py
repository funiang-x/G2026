#!/usr/bin/env python3
"""`.ioc` 操作的公共部分 —— `ioc_append` / `ioc_pinlabels` / `ioc_drop` 三个脚本共用。

为什么要抽出来
--------------
`parse_pins` / `check_pins` 原先在这三个脚本里**各抄了一份**（2026-09-26 合并）。
同一份校验抄三遍，代价不是"多几行"，而是**失败语义会漂移**：
"引脚编号必须 0..N-1 连续无重复"是手改 `.ioc` 最容易静默出错的地方
（实测踩过：手工重排 `Mcu.PinN` 造出引脚重复 + 丢失，且**不报错**，
只在某天加新引脚时突然冒出来）。改一处漏两处，等于这道校验不存在。

三个脚本的共同约定
------------------
· `.ioc` 是 **CRLF**（CubeMX 自己写的就是）。读的时候用 universal newlines 无所谓，
  但**写回去必须显式 `newline="\\r\\n"`** —— 写 LF 也能被 CubeMX 读，只是会制造
  满屏假 diff（"每行都变了"其实只差行尾）。
· `Mcu.PinN` 的 N 是 CubeMX **内部分配的索引**，不是用户可手写的序号。
  改完必须保持 0..N-1 连续 —— 所以每个会写 `.ioc` 的脚本收尾都要调 `check_pins()`。
· 端口号约定 0 = PA … 7 = PH（与 `board_config.h` 的 `BOARD_*_PORT` 同序）。

⚠️ 这些工具**都不是门禁**，是配置辅助工具（`Tools/` 下的 fw/cubemx_sync/check_doc_links
/selftest/stm32_size 才是门禁）。它们只产出候选文件或写前先备份，由人决定是否采用。
"""
import re

__all__ = ["parse_indexed", "parse_pins", "check_pins"]


def parse_indexed(text: str, prefix: str) -> dict[int, str]:
    """读 `<prefix>N=值` 形式的编号列表，返回 {N: 值}。

    例：`parse_indexed(text, "Mcu.Pin")` · `parse_indexed(text, "Mcu.IP")`。
    """
    pat = re.compile(rf"^{re.escape(prefix)}(\d+)=(.*)$", re.M)
    return {int(m.group(1)): m.group(2).strip() for m in pat.finditer(text)}


def parse_pins(text: str) -> dict[int, str]:
    """读 `Mcu.PinN=值`（`parse_indexed` 最常用的那个特例）。"""
    return parse_indexed(text, "Mcu.Pin")


def check_pins(text: str, label: str) -> list[str]:
    """引脚编号必须 0..N-1 连续、无重复。返回错误说明列表（空 = 通过）。

    `label` 只用来拼错误信息（调用方一般传 "输入" / "输出"）。
    """
    errs: list[str] = []
    pins = parse_pins(text)
    m = re.search(r"^Mcu\.PinsNb=(\d+)$", text, re.M)
    if not m:
        return [f"{label}: 找不到 Mcu.PinsNb"]
    n = int(m.group(1))
    if set(pins) != set(range(n)):
        errs.append(f"{label}: 编号不是 0..{n - 1} 连续 —— "
                    f"多={sorted(set(pins) - set(range(n)))} "
                    f"缺={sorted(set(range(n)) - set(pins))}")
    dup: dict[str, list[int]] = {}
    for k, v in pins.items():
        dup.setdefault(v, []).append(k)
    for v, ks in dup.items():
        if len(ks) > 1:
            errs.append(f"{label}: 引脚 {v} 出现在编号 {ks}（重复）")
    return errs
