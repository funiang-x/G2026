# -*- coding: utf-8 -*-
"""G2026 AFE 网表 + 器件模型（按 2026-10-02 落图后的活体网表 1:1 还原）

元件值/连接**从活体网表抄**（hardware/26 §2、_review3/netlist_now.py），不另立第二真相源。
改图后本文件必须同步。

模型锚点（每条都说明"凭什么这么定"，不猜）：
  · OPA2197 宏模型：2 级、单主极点。
      GBW = 10 MHz（datasheet）        → C1 = gm1/(2π·GBW)
      Aol_dc = 1e6 (120 dB)            → R1 = Aol/gm1
      压摆率 SR = 20 V/µs（datasheet） → Vk1 = SR·C1/gm1
    差分对用 tanh（MOS 对的自然形态）⇒ 同一个 Vk1 既锚住压摆率、又给出 HD3 量级。
    输出级另用一条 **并联 tanh 跨导** 模拟 gm 压缩（IM3 的第二个源），其膝点由
    短路电流 ±20 mA 锚定。
  · 电源抑制：**模型里显式加一条 VCCS**（supply→内部节点，增益 gm1/PSRR）。
    这是必需的 —— 不建模 PSRR 就等于假设"电源纹波进不了输出"，那 R21 该不该改就成了拍脑袋。
  · SRV05-4：AC/瞬态只体现为 Cj = 1.5 pF（钳位点 ~9V，本板信号远够不到）。
  · ADC 输入：STM32F4 采样网络 Rsw 串 + Cadc 到地，Cadc = 4 pF；
    Rsw 由 t_s = 3 ADCCLK = 83.3 ns 满足 0.5LSB 建立反解 ⇒ Rsw ≈ 2.2 kΩ。
"""
from __future__ import annotations

import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mna import Circuit  # noqa: E402

# ---------------- 图纸实值（改图必须同步这里） ----------------
# ⚠️ 2026-10-03 对齐修正：H2 把 R6/R7 改成 24.9k/3.57k 后，本表一直还是旧值
#    （R6=69.8k / R7=10k）。后果很隐蔽：`build_afe()` 不传 r6/r7 时**算的是改前设计**，
#    而 `studies.py` 的 OPT 用的正是 `r6=None, r7=None` ⇒ `python tools/sim/studies.py`
#    单独跑出来的 S1/S2/S3/S5/S7 全部是改前口径。现改为与活体网表一致，
#    并把 studies.OPT（改前基线）改成**显式写值**，不再靠这里的默认值兜底。
V = dict(
    R1=49.9, R3=49.9, R4=49.9, R8=100.0,
    R2=10e3, R5=10e3, R7=3.57e3, R9=10e3, R10=10e3, R6=24.9e3,
    C1=100e-9, C9=100e-9, C14=100e-9,
    C2=6.2e-9, C4=10e-9, C6=9.1e-9, C8=1.8e-9,
    C10=470e-12, C11=22e-6, C13=10e-6, C23=10e-6,
    # 注：`C12`/`JP3`/`TP6` 已于 2026-10-03 从图纸删除（VREF 留位整条支路是死的，
    #     见 `hardware/30 §4`）。本仿真从未引用它们，故此处不再列。
    L1=4.7e-6, L3=22e-6, L5=22e-6, L7=12e-6,
    Cj_tvs=1.5e-12,
    Cin_diff=8e-12, Cin_cm=4e-12,
    V5A=5.0, V3V3=3.3,
    Cadc=4e-12, Rsw=2.2e3,
    Rsrc=50.0,
)
LNAMES = ("L1", "L3", "L5", "L7")
CNAMES = ("C2", "C4", "C6", "C8")

OPA = dict(gm1=1e-3, GBW=10e6, Aol=1e6, SR=20e6, Ro=100.0, Isc=20e-3)


def opa2197(c: Circuit, tag, inp, inn, out, vcc, vee, nl_scale=1.0,
            psrr_db=100.0, vcc_node=None, p=None):
    """挂一个 OPA2197 单元。nl_scale<1 = 更非线性（敏感性分析用）。
    psrr_db 给定时，在内部节点注入与 vcc 纹波成正比的电流（电源抑制路径）。"""
    q = dict(OPA)
    if p:
        q.update(p)
    gm1, GBW, Aol, SR, Ro, Isc = (q["gm1"], q["GBW"], q["Aol"], q["SR"],
                                  q["Ro"], q["Isc"])
    C1 = gm1 / (2 * math.pi * GBW)
    R1 = Aol / gm1
    vk1 = SR * C1 / gm1 * nl_scale          # 输入级软饱和电压（锚：压摆率）
    vk2 = Isc / gm1 * nl_scale              # 输出级膝点（锚：短路电流）
    n1 = f"{tag}~n1"
    c.add_C(inp, inn, V["Cin_diff"], f"{tag}.cd")
    c.add_C(inp, "0", V["Cin_cm"], f"{tag}.cm+")
    c.add_C(inn, "0", V["Cin_cm"], f"{tag}.cm-")
    # 差分对：**电流从 V+ 灌进 n1**（极性必须让 v(inp)>v(inn) 时 v(n1) 上升）。
    # ⚠️ 第一版写反了（从 n1 抽走），后果是：交流闭环增益看起来仍对（DC 下
    #    1/β 与极性无关），但环路是**右半平面极点** ⇒ 瞬态里输出线性发散。
    #    这类错只有瞬态仿真能抓到 —— 又一次印证"必须仿真"。
    c.add_Gtanh("0", n1, inp, inn, gm1, vk1, f"{tag}.gm1")
    c.add_R(n1, "0", R1, f"{tag}.R1")
    c.add_C(n1, "0", C1, f"{tag}.C1")
    # 输出级：软饱和跨导（小信号输出阻抗 = 1/gm2 = Ro；限流 = gm2·vk2 = Isc）
    gm2 = 1.0 / Ro
    vk2 = Isc / gm2
    c.add_Gtanh(out, "0", out, n1, gm2, vk2, f"{tag}.gm2")
    if vcc_node is not None and psrr_db is not None:
        c.add_G(n1, "0", vcc_node, "0", gm1 / (10 ** (psrr_db / 20.0)),
                f"{tag}.psrr")
    return dict(n1=n1, C1=C1, R1=R1, vk1=vk1, vk2=vk2, Ro=Ro, gm1=gm1, gm2=gm2)


def build_afe(r21=0.0, r3=None, r8=None, c10=None, r9_from="3v3",
              l_scale=1.0, c_scale=1.0, l_dcr=0.0, nl_scale=1.0,
              psrr_db=100.0, adc_load=False, tvs=True, src_ac=0.0,
              rail_ac=0.0, r6=None, r7=None, cin_diff=None, cin_cm=None,
              rail3v3_ac=0.0):
    """还原 AFE 页。返回 (Circuit, 诊断字典)。"""
    c = Circuit("G2026 AFE")
    Ls = list(l_scale) if isinstance(l_scale, (list, tuple)) else [l_scale] * 4
    Cs = list(c_scale) if isinstance(c_scale, (list, tuple)) else [c_scale] * 4

    # ---------------- 电源 ----------------
    c.add_V("+5V", "0", dc=V["V5A"], ac_mag=rail_ac, label="V5V")
    c.add_R("+5V", "+5V_A", max(r21, 1e-6), "R21")
    c.add_V("+3V3", "0", dc=V["V3V3"], ac_mag=rail3v3_ac, label="V3V3")
    c.add_C("+5V_A", "0", V["C11"], "C11")
    c.add_C("+5V_A", "0", V["C14"], "C14")
    AG = "AGND"
    c.add_R(AG, "0", 1e-6, "AGND_tie")

    # ---------------- ① 输入保护与端接 ----------------
    c.add_V("SRC", "0", dc=0.0, ac_mag=src_ac, label="VSRC")
    c.add_R("SRC", "IN", V["Rsrc"], "Rsrc")
    c.add_R("IN", AG, V["R1"], "R1")
    if tvs:
        c.add_C("IN", AG, V["Cj_tvs"], "D1.Cj")
    c.add_C("IN", "BUFIN", V["C1"], "C1")
    c.add_R("BUFIN", "VBIAS", V["R2"], "R2")

    # ---------------- ② 一级缓冲（U1A, G=1） ----------------
    if cin_diff is not None:
        V["Cin_diff"] = cin_diff
    if cin_cm is not None:
        V["Cin_cm"] = cin_cm
    oa = opa2197(c, "U1A", "BUFIN", "BUF_OUT", "BUF_OUT", "+5V_A", AG,
                 nl_scale=nl_scale, psrr_db=psrr_db, vcc_node="+5V_A")

    # ---------------- ④ 8 阶 LC 梯形 ----------------
    r3v = V["R3"] if r3 is None else r3
    c.add_R("BUF_OUT", "LCIN", r3v, "R3")
    prev = "LCIN"
    for i in range(4):
        nxt = f"N{i + 1}"
        lv = V[LNAMES[i]] * Ls[i]
        if l_dcr > 0:
            mid = f"{LNAMES[i]}d"
            c.add_L(prev, mid, lv, LNAMES[i])
            c.add_R(mid, nxt, l_dcr, LNAMES[i] + ".dcr")
        else:
            c.add_L(prev, nxt, lv, LNAMES[i])
        c.add_C(nxt, AG, V[CNAMES[i]] * Cs[i], CNAMES[i])
        prev = nxt
    LCOUT = prev
    c.add_C(LCOUT, "R4T", V["C23"], "C23")
    c.add_R("R4T", AG, V["R4"], "R4")

    # ---------------- ⑤ 二级增益（U1B, ×7.98） ----------------
    c.add_C(LCOUT, "GAININ", V["C9"], "C9")
    c.add_R("GAININ", "VBIAS", V["R5"], "R5")
    ob = opa2197(c, "U1B", "GAININ", "FB", "GOUT", "+5V_A", AG,
                 nl_scale=nl_scale, psrr_db=psrr_db, vcc_node="+5V_A")
    c.add_R("GOUT", "FB", V["R6"] if r6 is None else r6, "R6")
    c.add_R("FB", "VBIAS", V["R7"] if r7 is None else r7, "R7")
    r8v = V["R8"] if r8 is None else r8
    c10v = V["C10"] if c10 is None else c10
    c.add_R("GOUT", "ADCP", r8v, "R8")
    c.add_C("ADCP", AG, c10v, "C10")

    # ---------------- ③ 基准与偏置 ----------------
    src9 = "+3V3" if r9_from == "3v3" else "VREF_ADC"
    if r9_from != "3v3":
        c.add_R("+3V3", "VREF_ADC", 1.0, "Rlink_ref")
    c.add_R(src9, "VBIAS", V["R9"], "R9")
    c.add_R("VBIAS", AG, V["R10"], "R10")
    c.add_C("VBIAS", AG, V["C13"], "C13")

    # ---------------- ADC 采样网络（可选） ----------------
    if adc_load:
        c.add_R("ADCP", "ADCIN", V["Rsw"], "Rsw")
        c.add_C("ADCIN", AG, V["Cadc"], "Cadc")

    return c, dict(oa=oa, ob=ob, LCOUT=LCOUT, AG=AG)


# ---------------------------------------------------------------- 口径工具
def bh4(n):
    """Blackman-Harris 4 项窗（与固件 op_dsp_winh4 同族）。"""
    import numpy as np
    a = (0.35875, 0.48829, 0.14128, 0.01168)
    k = np.arange(n)
    return (a[0] - a[1] * np.cos(2 * np.pi * k / (n - 1))
            + a[2] * np.cos(4 * np.pi * k / (n - 1))
            - a[3] * np.cos(6 * np.pi * k / (n - 1)))


def dft_amp(x, fs, f0, win=None):
    """单点 DFT 幅值（与固件同口径：单点 DFT + 相干增益归一）。"""
    import numpy as np
    n = len(x)
    w = np.ones(n) if win is None else win
    k = np.arange(n)
    X = np.sum(x * w * np.exp(-2j * np.pi * f0 * k / fs))
    return 2 * abs(X) / (n * w.mean())
