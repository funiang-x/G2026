# -*- coding: utf-8 -*-
"""把 afe_net 里的电路导出成 ngspice / LTspice 可跑的 .cir 网表（供外部 SPICE 独立复核）。

用法：python tools/sim/emit_spice.py
产物：tools/sim/spice/{afe_ac.cir, adc_kickback.cir}
说明：导出的是**同一份拓扑与同一批参数**，但运放用 SPICE 惯用的
      「VCVS + RC 主极点 + 输出电阻」子电路表达（与 Python 侧的 tanh 模型等价于小信号）。
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from afe_net import V, OPA, build_afe  # noqa: E402

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "spice")
os.makedirs(OUT, exist_ok=True)


def opa_subckt(name="OPA2197"):
    gm1, GBW, Aol, SR, Ro, Isc = (OPA["gm1"], OPA["GBW"], OPA["Aol"],
                                  OPA["SR"], OPA["Ro"], OPA["Isc"])
    C1 = gm1 / (2 * math.pi * GBW)
    R1 = Aol / gm1
    return f"""* ---- {name} 宏模型（单极点 + 开环输出电阻）----
* GBW={GBW/1e6:.0f}MHz  Aol={Aol:.0f}({20*math.log10(Aol):.0f}dB)  SR={SR/1e6:.0f}V/us  Ro={Ro:.0f}ohm
* 节点: 1=+in 2=-in 3=out 4=V+ 5=V-
.SUBCKT {name} 1 2 3 4 5
RCM1 1 0 1e12
RCM2 2 0 1e12
CDIF 1 2 {V['Cin_diff']*1e12:.4g}p
CCM1 1 0 {V['Cin_cm']*1e12:.4g}p
CCM2 2 0 {V['Cin_cm']*1e12:.4g}p
* 差分跨导 gm1 = {gm1*1e3:.4g} mS；主极点由 R1/C1 决定
G1 0 N1 1 2 {gm1:.6g}
R1 N1 0 {R1:.6g}
C1 N1 0 {C1*1e12:.6g}p
* 输出级：单位增益缓冲 + 开环输出电阻
E1 N2 0 N1 0 1.0
RO 3 N2 {Ro:.6g}
.ENDS {name}
"""


def emit_afe_ac():
    c, _ = build_afe(src_ac=1.0)
    L = []
    A = L.append
    A("* G2026 AFE 端到端 AC —— 与 tools/sim/afe_net.py 同拓扑同参数")
    A("* 用途：独立 SPICE 复核通带 droop / 折叠带衰减。改图后请同步 afe_net.py 再重跑本脚本。")
    A("")
    A(opa_subckt())
    A("")
    A("* ---- 电源 ----")
    A(f"V5V   5V    0    DC {V['V5A']} AC 0")
    A(f"R21   5V    5VA  {4.7:.6g}")          # ★ 改后值（H1）
    A(f"V3V3  3V3   0    DC {V['V3V3']} AC 0")
    A(f"C11   5VA   0    {V['C11']*1e6:.6g}u")
    A(f"C14   5VA   0    {V['C14']*1e9:.6g}n")
    A("")
    A("* ---- ① 输入保护与端接 ----")
    A("VSRC  SRC   0    DC 0 AC 1")           # AC 1V 作为扫频激励
    A(f"RSRC  SRC   IN   {V['Rsrc']:.6g}")
    A(f"R1    IN    AGND {V['R1']:.6g}")
    A(f"CJ    IN    AGND {V['Cj_tvs']*1e12:.6g}p")
    A(f"C1    IN    BUFIN {V['C1']*1e9:.6g}n")
    A(f"R2    BUFIN VBIAS {V['R2']:.6g}")
    A("")
    A("* ---- ② 一级缓冲 U1A（G=1）----")
    A("XU1A  BUFIN BUF_OUT 5VA AGND OPA2197")
    A("")
    A("* ---- ④ 8 阶 LC 梯形 ----")
    A(f"R3    BUF_OUT LCIN {V['R3']:.6g}")
    names = (("L1", 4.7e-6, "C2", 5.6e-9), ("L3", 22e-6, "C4", 10e-9),
             ("L5", 22e-6, "C6", 10e-9), ("L7", 12e-6, "C8", 1.8e-9))
    prev = "LCIN"
    for i, (ln, lv, cn, cv) in enumerate(names, start=1):
        A(f"{ln}   {prev} N{i}   {lv*1e6:.6g}u")
        A(f"{cn}   N{i}   AGND {cv*1e9:.6g}n")
        prev = f"N{i}"
    A(f"C23   N4    R4T  {V['C23']*1e6:.6g}u")
    A(f"R4    R4T   AGND {V['R4']:.6g}")
    A("")
    A(f"* ---- ⑤ 二级增益 U1B（×{1 + V['R6'] / V['R7']:.2f}）----")
    A(f"C9    N4    GAININ {V['C9']*1e9:.6g}n")
    A(f"R5    GAININ VBIAS {V['R5']:.6g}")
    A("XU1B  GAININ FB 5VA AGND OPA2197")
    A(f"R6    GOUT  FB  {V['R6']:.6g}")
    A(f"R7    FB    VBIAS {V['R7']:.6g}")
    A(f"R8    GOUT  ADCP {V['R8']:.6g}")
    A(f"C10   ADCP  AGND {V['C10']*1e12:.6g}p")
    A("")
    A("* ---- ③ 基准与偏置（R9.1 已改接 +3V3）----")
    A(f"R9    +3V3  VBIAS {V['R9']:.6g}")
    A(f"R10   VBIAS AGND  {V['R10']:.6g}")
    A(f"C13   VBIAS AGND  {V['C13']*1e6:.6g}u")
    A("")
    A("* ---- 模拟地汇到 0（本网表里 AGND 即参考）----")
    A("RAG   AGND  0    1m")
    A("")
    A("* ---- 分析 ----")
    A(".ac dec 200 1k 3Meg")
    A(".print ac vdb(ADCP) vp(ADCP)")
    A(".end")
    return "\n".join(L) + "\n"


def emit_kickback():
    c, d = build_afe(r8=V["R8"], c10=V["C10"], adc_load=False)
    ts = 83.3e-9
    L = []
    A = L.append
    A("* G2026 ADC 采样保持反冲 —— 与 tools/sim/studies.py::s3_adc_kickback 同拓扑")
    A("* 场景：500kHz 满摆幅时相邻两采样点差 ΔV=0.498V；Cadc 从上一次采样值起算")
    A("* 判读：看 t=83.3ns 时 ADCIN 相对 1.899V 的残差（目标 <1 LSB = 0.806mV）")
    A("")
    A(f"VDRV  DRV  0   DC 1.899")
    A(f"RD    DRV  GOUT 1m")
    A(f"R8    GOUT ADCP {V['R8']:.6g}")
    A(f"C10   ADCP AGND {V['C10']*1e12:.6g}p")
    A(f"RSW   ADCP ADCIN 0.001")
    A(f"CADC  ADCIN AGND {V['Cadc']*1e12:.6g}p IC=1.401")
    A("RAG   AGND 0 1m")
    A("")
    A(".tran 0.02n 333.2n UIC")
    A(".print tran v(ADCIN) v(ADCP)")
    A(".end")
    return "\n".join(L) + "\n"


if __name__ == "__main__":
    p1 = os.path.join(OUT, "afe_ac.cir")
    p2 = os.path.join(OUT, "adc_kickback.cir")
    open(p1, "w", encoding="utf-8", newline="\n").write(emit_afe_ac())
    open(p2, "w", encoding="utf-8", newline="\n").write(emit_kickback())
    print("→", p1)
    print("→", p2)
