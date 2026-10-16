# -*- coding: utf-8 -*-
"""rterm_check.py —— 49.9Ω 端子电阻的替代：手上只有 1% 的 51Ω / 47Ω 怎么办？

**这三个不是同一种角色，不能一起拍脑袋：**
  · `R1` = **输入端接**（50Ω 系统匹配）——影响 VSWR 与那个"×0.5 的免费抗混叠增益"
  · `R3` = **LC 梯形的源端阻抗**、`R4` = **负载端阻抗** —— 它们**是滤波器的一部分**，
    改它们 = 改滤波器（`30 §5.3` 已记"`R3` 不是随便一个串阻"）

**49.9Ω 的来历**：它只是 E96 里最接近 50Ω 的值。真实信号源 50Ω±2%、电缆 50Ω±2~5%，
所以系统本来就不是精确的 50.0Ω —— 51Ω 相对 50Ω 是 +2.2%，在真实系统的不确定度之内。
但这属于"应该成立"，本文件把它**算出来**。

用法：python tools/sim/rterm_check.py
"""
from __future__ import annotations

import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import studies as ST  # noqa: E402
from afe_net import V, build_afe  # noqa: E402

OPT = dict(r21=4.7, r6=6.8e3, r7=1e3)   # 2026-10-15 实物
RSRC = 50.0
FS = 2.0e6


def db(x):
    return 20 * np.log10(np.maximum(np.abs(x), 1e-30))


def metrics(r1, r3, r4, n=260):
    old = (V["R1"], V["R3"], V["R4"])
    V["R1"], V["R3"], V["R4"] = r1, r3, r4
    try:
        c, _ = build_afe(src_ac=1.0, **OPT)
        fs = ST.sweep(1e3, 3e6, n)
        H = db(c.ac(fs)["ADCP"])
    finally:
        V["R1"], V["R3"], V["R4"] = old
    fold = (fs >= 1.5e6) & (fs <= 2.5e6)
    k1 = int(np.argmin(np.abs(fs - 1e3)))
    k5 = int(np.argmin(np.abs(fs - 5e5)))
    fc17 = np.logspace(4, math.log10(5e5), 17)
    hc = np.interp(np.log(fc17), np.log(fs), H)
    fg = np.logspace(4, math.log10(5e5), 2001)
    resid = float(np.max(np.abs(np.interp(np.log(fg), np.log(fs), H)
                                - np.interp(np.log(fg), np.log(fc17), hc))))
    pb = H[(fs >= 1e4) & (fs <= 5e5)]
    return dict(
        term=r1 / (RSRC + r1),                      # 端接分压比（×0.5 那个）
        vswr=(1 + abs((r1 - 50) / (r1 + 50))) / (1 - abs((r1 - 50) / (r1 + 50))),
        net=float(abs(10 ** (H[k1] / 20))),
        droop500=float(H[k5] - H[k1]),
        ripple=float(pb.max() - pb.min()),
        fold=float(H[fold].max()), margin=float(-H[fold].max() - 42.5),
        resid_db=resid, resid_mv=float((10 ** (resid / 20) - 1) * 250))


def main():
    base = metrics(49.9, 49.9, 49.9)
    r_par = 51.0 * 2700.0 / (51.0 + 2700.0)          # 51∥2.7k
    cases = [
        ("基准 49.9 / 49.9 / 49.9", 49.9, 49.9, 49.9),
        ("**全 51Ω**", 51.0, 51.0, 51.0),
        ("全 47Ω", 47.0, 47.0, 47.0),
        ("只 R1 换 51Ω", 51.0, 49.9, 49.9),
        ("只 R1 换 47Ω", 47.0, 49.9, 49.9),
        ("只 R3/R4 换 51Ω", 49.9, 51.0, 51.0),
        ("只 R3/R4 换 47Ω", 49.9, 47.0, 47.0),
        ("R1=51 + R3/R4=49.9（单侧）", 51.0, 49.9, 49.9),
        (f"组合 51∥2.7k = {r_par:.2f}Ω", r_par, r_par, r_par),
        ("组合 47+3.0 = 50.0Ω", 50.0, 50.0, 50.0),
    ]
    print("=" * 108)
    print("49.9Ω 端子电阻替代分析（判据：折叠余量 ≥0、标定残差 → 幅值误差 <5mV）")
    print("=" * 108)
    print(f"  {'方案':<26}{'端接比':>9}{'VSWR':>7}{'净增益':>9}{'vs基准':>8}"
          f"{'@500k':>9}{'通带峰谷':>10}{'折叠最差':>10}{'余量':>7}{'残差→mV':>9}")
    for tag, r1, r3, r4 in cases:
        m = metrics(r1, r3, r4)
        d = (m["net"] / base["net"] - 1) * 100
        print(f"  {tag:<26}{m['term']:>9.4f}{m['vswr']:>7.3f}{m['net']:>9.4f}{d:>+7.2f}%"
              f"{m['droop500']:>+8.3f}dB{m['ripple']:>9.3f}dB{m['fold']:>9.2f}dB"
              f"{m['margin']:>6.1f}dB{m['resid_mv']:>9.3f}")

    print(f"\n  容差 MC（L±10% / C±5%，n=200）看折叠带会不会跌破 −42.5dB：")
    for tag, r in (("基准 49.9", 49.9), ("全 51Ω", 51.0), ("全 47Ω", 47.0)):
        old = (V["R1"], V["R3"], V["R4"])
        V["R1"] = V["R3"] = V["R4"] = r
        try:
            ST.set_config(**OPT)
            d = ST.s2_mc(nrun=200, seed=20261003)
        finally:
            V["R1"], V["R3"], V["R4"] = old
        print(f"    {tag:<12} 折叠带中位 {d['fold_worst_med']:>7.2f}dB · 最坏 {d['fold_worst_min']:>7.2f}dB · "
              f"跌破 {d['fail']}/{d['n']} · 通带起伏最大 {d['ripple_max']:.3f}dB")

    print("\n" + "=" * 108)
    print("注：净增益的绝对值 *会* 变，但**它被固件的 17 点标定吸收**（标定表在本板实测建），")
    print("    所以真正要看的是 **通带峰谷 / 折叠余量 / 标定残差** 这三列，不是『vs 基准』那一列。")
    print("=" * 108)


if __name__ == "__main__":
    main()
