# -*- coding: utf-8 -*-
"""stability_check.py —— AFE 会不会自激？

**为什么必须单独查**：`27 §3.3` 的 E1 就是"AC 增益、DC 工作点全对，**只有瞬态才抓到的
右半平面极点**" —— 自激在 AC 扫描里可以完全看不出来。

**拓扑上本来就有的三道保护**（本文件量化的是"外部网络吃掉了多少裕度"，不是"有没有保护"）：
  ① `U1A` 接成跟随器，但**反馈点取在输出引脚**上，`R3`(49.9Ω) 串在反馈点**之后**
     ⇒ 梯形大电容**不在环内**（经典"隔离电阻"手法，`30 §1.4` 已记其意图）；
  ② `U1B` 同理：反馈取在 `GOUT`，`R8`(100Ω) 在**之后**，`C10` **不在环内**；
  ③ `U1B` 噪声增益 = `1+R6/R7` = **7.975**（不是 1）。

⚠️ **本电路没有"单一相位裕度"**（本文件实测出来的）：
`VBIAS` 同时是 `U1A` 的 `+in` 偏置（经 `R2`）、`U1B` 的 `+in` 偏置（经 `R5`）、
以及 `U1B` 反馈网络的回地点（`R7`）⇒ 存在 **`FBJ → R7 → VBIAS → R5 → GAININ` 的第二条反馈路径**。
用串联注入法测"环增益"时实测到 `V(GAININ) ≈ V(FB)`（差模被抵消），说明**把它当单环量 PM 会得出假数字**。
⇒ 本文件改为问两个**定义明确**的问题：
  (A) 环内那个**外部极点**在哪、吃掉多少相位（可解析、可比较）；
  (B) **零输入长瞬态下，输出会不会持续振荡**（唯一能直接回答"自激"的判据）。

**模型边界（结论必须带上）**：内核的 OPA2197 宏模型是"2 级 + 单主极点"，
**会低估真实运放的高频相位损失** ⇒ 下面的相位数字只能**横向比较**（新/旧 R6R7、Cin 大/小），
**不能**当作"真实运放就是 90°"。真实器件的裕度是 datasheet 性质，**本仓没有该 datasheet**。

用法：python tools/sim/stability_check.py
"""
from __future__ import annotations

import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from afe_net import V, build_afe, opa2197  # noqa: E402
from mna import Circuit  # noqa: E402

OPT_LIVE = dict(r21=4.7, r6=24.9e3, r7=3.57e3)
R6_OLD, R7_OLD = 69.8e3, 10e3


def db(x):
    return 20 * np.log10(np.maximum(np.abs(x), 1e-30))


# ============================================================ ① 运放自身开环（= U1A 的环增益）
def opamp_open_loop():
    """U1A 的反馈点就是输出脚、β=1、负载被 R3 隔离在环外 ⇒ 环增益 = 裸开环 A(f)。"""
    c = Circuit("Aol")
    c.add_R("AGND", "0", 1e-6, "tie")
    opa2197(c, "X", "INP", "INN", "OUT", "+5V_A", "AGND", psrr_db=None)
    c.add_V("SRC", "0", dc=0.0, ac_mag=1.0, label="VSRC")
    c.add_R("SRC", "INP", 50.0, "Rs")
    c.add_R("INN", "0", 1e-6, "tie2")
    f = np.logspace(0, 9.5, 600)
    A = c.ac(f)["OUT"]
    m = np.abs(A)
    k = int(np.argmin(np.abs(m - 1.0)))
    ph = np.unwrap(np.angle(A))
    # |A|=1 处线性插值
    lf = np.log10(f)
    fc = 10 ** np.interp(math.log10(1.0), db(A)[::-1] * 0 + np.log10(m[::-1]), lf[::-1])
    pm = 180.0 + float(np.degrees(np.interp(math.log10(fc), lf, ph)))
    return dict(f=f, A=A, fc=fc, pm=pm, n_pole_est=1)


# ============================================================ ② 环内的"外部极点"
def inloop_pole(r6, r7, cin_diff, cin_cm):
    """U1B 的 −in 节点上，反馈网络阻抗 R6∥R7 与运放输入电容形成一个极点。
    它在环内 ⇒ 直接吃相位裕度。这是本设计**唯一**由外部元件决定的环内极点。"""
    rth = 1.0 / (1.0 / r6 + 1.0 / r7)
    ctot = cin_diff + cin_cm
    fp = 1.0 / (2 * math.pi * rth * ctot)
    return dict(rth=rth, ctot=ctot, fp=fp)


def phase_lag(fp, fc):
    return math.degrees(math.atan(fc / fp))


# ============================================================ ③ 零输入长瞬态：会不会持续振荡
def selfosc(cpar=0.0, r6=None, r7=None, cin=None, tstop=200e-6, dt=1e-9):
    """**输入置零**，跑长瞬态。自激的特征是输出**持续振荡**（末窗 p-p 不趋于 0）。
    起始瞬态会把所有模式都激励起来，所以"不稳定模式会自己长出来"，不需要外部激励。"""
    if cin is not None:
        V["Cin_diff"], V["Cin_cm"] = cin
    kw = dict(OPT_LIVE)
    if r6:
        kw["r6"] = r6
    if r7:
        kw["r7"] = r7
    c, _ = build_afe(**kw)                     # src_ac 默认 0；VSRC 的 dc 也是 0
    for i, (np_, nn_, dc, lb, am, ap) in enumerate(c.V):
        if lb == "VSRC":
            c.V[i] = (np_, nn_, dc, lb, 0.0, 0.0)
            c.VWAVE[i] = (lambda t: 0.0)
    if cpar:
        c.add_C("GOUT", "AGND", cpar, "Cpar_layout")
    ts, vs = c.tran(tstop, dt, method="trap")
    out = {}
    for node in ("GOUT", "ADCP", "BUF_OUT"):
        y = np.asarray(vs[node])
        def pp(a, b, y=y):
            s = y[(ts >= a) & (ts < b)]
            return float(s.max() - s.min()) if len(s) else float("nan")
        out[node] = dict(w1=pp(50e-6, 100e-6), w2=pp(150e-6, 200e-6))
    return out


def main():
    print("=" * 100)
    print("AFE 稳定性专项：会不会自激")
    print("=" * 100)

    a = opamp_open_loop()
    print(f"\n① 运放宏模型裸开环（= U1A 的环增益：反馈点在输出脚、β=1、负载被 R3 隔离在环外）")
    print(f"   |A|=1 穿越 = {a['fc']/1e6:.3f} MHz（宏模型锚点 GBW = 10 MHz）")
    print(f"   该点相位裕度 = {a['pm']:.1f}°   ⚠️ 宏模型只有单主极点 ⇒ **这是上界，不是真实器件的值**")
    print(f"   ⇒ U1A 环内**没有任何外部极点**（反馈点=输出脚，梯形电容经 R3 在环外）")

    print(f"\n② 环内的唯一外部极点：`R6∥R7` × 运放输入电容（在 U1B 的 −in 上）")
    fc_u1b = 10e6 * (1.0 / 7.975)              # 环穿越 ≈ GBW × β
    print(f"   U1B 环穿越 ≈ GBW×β = {fc_u1b/1e6:.2f} MHz")
    print(f"   {'R6/R7':<22}{'Cin':>9}{'R6∥R7':>10}{'极点':>10}{'穿越处相位损失':>16}")
    for tag, r6, r7 in ((f"活体 {R6_OLD/1e3:.1f}k/{R7_OLD/1e3:.0f}k（改前）", R6_OLD, R7_OLD),
                        ("活体 24.9k/3.57k", 24.9e3, 3.57e3)):
        for cd, cc, ct in ((2e-12, 2e-12, "2p/2p"), (8e-12, 4e-12, "8p/4p"),
                           (16e-12, 8e-12, "16p/8p")):
            p = inloop_pole(r6, r7, cd, cc)
            lag = phase_lag(p["fp"], fc_u1b)
            print(f"   {tag:<22}{ct:>9}{p['rth']/1e3:>9.2f}k{p['fp']/1e6:>9.2f}M{lag:>15.1f}°")
    V["Cin_diff"], V["Cin_cm"] = 8e-12, 4e-12

    print(f"\n③ 零输入长瞬态（200µs @1ns；**自激的特征是末窗 p-p 不趋于 0**）")
    print(f"   {'情形':<30}{'GOUT 50-100us':>16}{'GOUT 150-200us':>17}{'ADCP 150-200us':>17}")
    for tag, kw in (("活体（无寄生）", dict()),
                    ("输出脚寄生 22 pF", dict(cpar=22e-12)),
                    ("输出脚寄生 100 pF", dict(cpar=100e-12)),
                    ("Cin 16p/8p（最坏档）", dict(cin=(16e-12, 8e-12))),
                    ("改前 R6/R7 + Cin 16p/8p", dict(r6=R6_OLD, r7=R7_OLD, cin=(16e-12, 8e-12)))):
        d = selfosc(**kw)
        print(f"   {tag:<30}{d['GOUT']['w1']*1e6:>14.3f}uV{d['GOUT']['w2']*1e6:>15.3f}uV"
              f"{d['ADCP']['w2']*1e6:>15.3f}uV")
    V["Cin_diff"], V["Cin_cm"] = 8e-12, 4e-12

    print("\n" + "=" * 100)
    print("读法：③ 的末窗（150-200µs）p-p 若落到 µV 以下 ⇒ 无自激；")
    print("      若与首窗同量级或更大 ⇒ 自激，必须查该档（先查寄生电容与去耦）。")
    print("★ 本仿真**不能**替代上板判据：真实器件的高频相位是 datasheet 性质，宏模型盖不住。")
    print("★ 上板判据（建议加进 23 §7 作 V13）：SMA 端接 50Ω **不接信号源**，")
    print("   示波器看 TP4 / U1 输出有无**持续正弦**；并看 `+5V_A` 电流是否异常偏大（V5 已用此判据）。")
    print("=" * 100)


if __name__ == "__main__":
    main()
