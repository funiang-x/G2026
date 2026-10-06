# -*- coding: utf-8 -*-
"""dc_bias_check.py —— VBIAS 的直流到底怎么传到 U1B 输出？

要判的三句话：
  A. 我先写在图上/文档里的："R7 回 VBIAS 时直流增益为 **0**"       → 待判
  B. 我的推导："偏置传到输出是 **×1**"（V− = V+ = VBIAS ⇒ Vout = VBIAS） → 待判
  C. `27 §H1` 的说法："偏置误差被 ×7.975 放大 ⇒ 6.8V 饱和"        → 待判

**推导**：同相放大，R6 由 Vout 到 −in、R7 由 −in 到 VBIAS。
  V− = VBIAS + (Vout − VBIAS)·R7/(R6+R7)；直流下 V− = V+ = VBIAS ⇒ **Vout = VBIAS（×1）**
  若 R7 接 AGND：V− = Vout·R7/(R6+R7) = VBIAS ⇒ Vout = VBIAS·(1+R6/R7) = 1.65×7.975 = **13.16V** ⇒ 贴轨

⚠️ `r9_from` 只认 `"3v3"`，传别的会落到 `VREF_ADC`（不是"改成 +5V_A"）——所以本脚本
   用**直接改分压比**（`V["R9"]`）来制造偏置误差，不再依赖 `r9_from`。

用法：python tools/sim/dc_bias_check.py
"""
from __future__ import annotations

import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from afe_net import V, build_afe  # noqa: E402

OPT = dict(r21=4.7, r6=24.9e3, r7=3.57e3)
KEYS = ("VBIAS", "BUFIN", "N4", "GOUT", "ADCP")


def dc_of(r9_ohm=None, r7_to_agnd=False):
    old = V["R9"]
    if r9_ohm is not None:
        V["R9"] = r9_ohm
    try:
        c, _ = build_afe(**OPT)
        c.add_R("GOUT", "AGND", 1e6, "Rleak")   # 真实 ADC 输入有直流回路；不加会矩阵奇异
        if r7_to_agnd:
            for i, (n1, n2, ohm, lb) in enumerate(c.R):
                if lb == "R7":
                    c.R[i] = (n1, "AGND", ohm, lb)
        r = c.dc()
    finally:
        V["R9"] = old
    return {k: float(np.real(r[k])) for k in KEYS}


print('=' * 98)
print('VBIAS 直流传递：偏置传到 U1B 输出是 ×1、×0、还是 ×7.975？')
print('=' * 98)
rows = [
    ('① 活体：R7→VBIAS，R9=R10=10k ⇒ VBIAS=1.650V', dict()),
    ('② VBIAS 抬到 ~2.48V（R9=3.3k；偏置误差 +0.83V）', dict(r9_ohm=3.3e3)),
    ('③ VBIAS 抬到 ~2.75V（R9=2.0k；偏置误差 +1.10V）', dict(r9_ohm=2.0e3)),
    ('④ 反例：R7 改接 AGND，VBIAS 仍 1.650V', dict(r7_to_agnd=True)),
]
print(f"\n{'情形':<48}{'VBIAS':>9}{'U1A出':>9}{'梯形出':>9}{'U1B出':>9}{'ADC入':>9}")
base = None
for tag, kw in rows:
    m = dc_of(**kw)
    if base is None:
        base = m
    print(f"{tag:<48}" + ''.join(f'{m[k]:>9.4f}' for k in KEYS))

print('\n' + '=' * 98)
print('读法：')
print('  · ②③ 相对 ① 的 (U1B出 − VBIAS) 若 ≈ 0 ⇒ 偏置 **×1** 通过 ⇒ 图上「直流增益为 0」要改成「×1」，')
print('    且 §H1 的「×7.975 ⇒ 6.8V 饱和」不成立；')
print('  · ④ 的 U1B出 = 13.16V ⇒ 证明「R7 必须回 VBIAS 而不是 AGND」成立。')
print('=' * 98)
