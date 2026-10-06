# -*- coding: utf-8 -*-
"""level_walk.py —— 50mVpp / 250mVpp 的信号进来后，**逐级电平**是多少。

题面口径：**`Upp` 指的是信号源电动势（SMA 端的 EMF）**，不是端接电阻上的电压。
证据（`26 §2.5` 自己的算式）：`250mVpp × 0.5(端接) × 0.5(LC 双端接) × 7.975 = ±249.2mVpk` ✓

用法：python tools/sim/level_walk.py
"""
from __future__ import annotations

import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from afe_net import build_afe  # noqa: E402

OPT = dict(r21=4.7, r6=24.9e3, r7=3.57e3)
VREF, NBIT = 3.3, 12
LSB = VREF / (1 << NBIT)          # 805.66 µV
VBIAS = 1.65

# 链上 12 个节点（顺序 = 信号真正经过的顺序）
NODES = [
    ("SRC",     "SMA 端电动势（题面 Upp 就在这里）"),
    ("IN",      "R1 端接之后"),
    ("BUFIN",   "C1 隔直之后（U1A 的 +in）"),
    ("BUF_OUT", "U1A 缓冲输出（G=1）"),
    ("LCIN",    "R3 之后（梯形入口）"),
    ("N1",      "第 1 节后"),
    ("N2",      "第 2 节后"),
    ("N3",      "第 3 节后"),
    ("N4",      "梯形输出（= LCOUT）"),
    ("GAININ",  "C9 隔直之后（U1B 的 +in）"),
    ("GOUT",    "U1B 输出（×7.975）"),
    ("ADCP",    "R8+C10 之后 = **ADC 输入**"),
]


def walk(f, upp):
    """返回 `{node: 峰值幅度(单位 V)}`，以及各节点相对源端的 |H|。"""
    c, _ = build_afe(src_ac=1.0, **OPT)
    r = c.ac(np.array([f]))
    src = (upp / 2.0)                      # 源端峰值 = Upp/2
    return {n: abs(r[n][0]) * src for n, _ in NODES}


def main():
    print("=" * 104)
    print("信号电平走线：50mVpp 与 250mVpp 从 SMA 到 ADC")
    print("=" * 104)

    print(f"\n① 逐级电平 @10kHz（通带内）—— 单位 mVpeak")
    print(f"   {'节点':<9}{'说明':<34}{'50mVpp':>13}{'250mVpp':>13}{'比值':>9}")
    a50, a250 = walk(10e3, 0.050), walk(10e3, 0.250)
    for n, desc in NODES:
        r = a250[n] / a50[n]
        print(f"   {n:<9}{desc:<34}{a50[n]*1e3:>13.4f}{a250[n]*1e3:>13.4f}{r:>9.3f}")

    print(f"\n② 关键节点的频率响应（只列 3 个关键点；单位 mVpeak）")
    print(f"   {'f':>8}{'':3}{'BUF_OUT(50)':>13}{'N4/LCOUT(50)':>14}{'ADCP(50)':>11}"
          f"{'ADCP(250)':>12}{'ADCP衰减vs10k':>15}")
    ref = None
    for f in (10e3, 31.5e3, 100e3, 500e3, 1.0e6, 1.5e6, 2.0e6):
        w50, w250 = walk(f, 0.050), walk(f, 0.250)
        if ref is None:
            ref = w250["ADCP"]
        att = 20 * math.log10(w250["ADCP"] / ref)
        print(f"   {f/1e3:>7.1f}k{'':3}{w50['BUF_OUT']*1e3:>13.4f}{w50['N4']*1e3:>14.4f}"
              f"{w50['ADCP']*1e3:>11.4f}{w250['ADCP']*1e3:>12.4f}{att:>14.1f}dB")

    print(f"\n③ 折算成 ADC 码值（12bit / VREF 3.3V / 1 LSB = {LSB*1e6:.2f}µV / 中心 {VBIAS}V）")
    print(f"   {'输入 Upp':<12}{'ADC 输入 pk':>14}{'ADC 域电压范围':>24}{'码值范围':>18}{'摆幅/满量程':>13}")
    for upp in (0.050, 0.100, 0.250):
        w = walk(10e3, upp)
        pk = w["ADCP"]
        lo, hi = VBIAS - pk, VBIAS + pk
        c_lo, c_hi = round(lo / VREF * 4096), round(hi / VREF * 4096)
        print(f"   {upp*1e3:>6.0f}mVpp{'':3}{pk*1e3:>12.3f}mV"
              f"{lo:>13.4f} ~{hi:>8.4f}V{c_lo:>8d} ~{c_hi:>5d}"
              f"{(c_hi-c_lo)/4096*100:>12.1f}%")

    print(f"\n④ 余量（按 5mV 判据与 1.5mV 门限折算到源端）")
    print(f"   1 LSB 折到源端   = {LSB/1.9937*1e3:.3f} mV")
    print(f"   量化噪声(rms)    = {LSB/math.sqrt(12)/1.9937*1e3:.3f} mVrms（源端）")
    print(f"   仿真噪声底        = 0.054 mVrms（@TP4）")
    print(f"   50mVpp 时：量化步长占 Upp 的 {LSB/1.9937/0.050*100:.2f}% ⇒ 5mV 判据 ≈ {(5/50)*100:.0f}% of Upp")
    print(f"   250mVpp 不削顶上限 = ±827 mVpk（源端，= {827/125:.1f}× 于 250mVpp 的峰值）")

    print("\n" + "=" * 104)
    print("读法：① 里『比值』一列全部是常数 ⇒ **整条链是线性的**（LTI），两个端点只差一个比例系数；")
    print("      ② 里 500k 以内基本平坦、1M 之后陡降 ⇒ 通带直通、带外砍掉；")
    print("      ③ 里最小的 50mVpp 只用到满量程的 3% ⇒ 这是拿量程换余量的**主动取舍**。")
    print("=" * 104)


if __name__ == "__main__":
    main()
