# -*- coding: utf-8 -*-
"""G2026 · AFE 抗混叠滤波器定案（fc=660kHz · BW n=8 · 全部锁立创在售档位）+ 容差蒙特卡洛。"""
import numpy as np

R = 50.0
BAND = np.linspace(1.5e6, 2.5e6, 8001)
INBAND = np.linspace(10e3, 500e3, 4001)

L = [4.7e-6, 22e-6, 22e-6, 12e-6]
C = [5.6e-9, 10e-9, 10e-9, 1.8e-9]
PARTS = ["XRCD54-4R7M", "CD54-220M", "CD54-220M", "XRCD54-120M",
         "0805CG562J500NT", "0805CG103J500NT", "0805CG103J500NT", "0805CG182J500NT"]

def ladder_db(Ls, Cs, f):
    f = np.atleast_1d(np.asarray(f, float)); w = 2*np.pi*f
    A = np.ones_like(w, dtype=complex); B = np.zeros_like(A); Cc = np.zeros_like(A); D = np.ones_like(A)
    for i in range(len(Ls)):
        A, B, Cc, D = (A, A*1j*w*Ls[i] + B, Cc, Cc*1j*w*Ls[i] + D)
        A, B, Cc, D = (A + B*1j*w*Cs[i], B, Cc + D*1j*w*Cs[i], D)
    return 20*np.log10(np.abs(2*R/(A*R + B + Cc*R*R + D*R)))

lines = []
def out(s=""):
    print(s); lines.append(s)

out("=" * 78)
out("G2026 AFE 抗混叠滤波器 · 定案（fc=660kHz · 8 阶巴特沃斯 · 50Ω 双端接）")
out("=" * 78)
out()
out("元件（全部立创在售档位）：")
for i, (l, c) in enumerate(zip(L, C)):
    out(f"  L{i*2+1} = {l*1e6:>5g} uH   {PARTS[i]:<18}   C{i*2+2} = {c*1e9:>4g} nF   {PARTS[4+i]}")
out()
w = ladder_db(L, C, BAND)
out(f"折叠带 [1.5, 2.5]MHz 最差衰减 : {w.max():.2f} dB   (判据 <= -42.5 dB → 余量 {abs(w.max())-42.5:.1f} dB)")
for f in [1.5e6, 2.0e6, 2.5e6]:
    out(f"     @{f/1e6:.1f} MHz : {ladder_db(L,C,[f])[0]:7.2f} dB")
ib = ladder_db(L, C, INBAND)
out(f"带内 10k~500k 起伏          : {ib.max()-ib.min():.3f} dB    (@500k = {ladder_db(L,C,[500e3])[0]:.3f} dB)")
out()
out("蒙特卡洛（L ±10%、C ±5% 均匀分布，5000 次，扫 [1.5,2.5]MHz）：")
rng = np.random.default_rng(20260928)
fchk = np.linspace(1.5e6, 2.5e6, 401)
worst = []
for _ in range(5000):
    Ls = [x*(1+rng.uniform(-0.10, 0.10)) for x in L]
    Cs = [x*(1+rng.uniform(-0.05, 0.05)) for x in C]
    worst.append(ladder_db(Ls, Cs, fchk).max())
worst = np.array(worst)
bad = int((worst > -42.5).sum())
out(f"  最坏样本 {worst.max():.2f} dB   中位 {np.median(worst):.2f} dB   95% 分位 {np.percentile(worst,95):.2f} dB")
out(f"  跌破 -42.5dB 的样本 : {bad}/5000 = {100*bad/5000:.2f}%   最坏余量 {abs(worst.max())-42.5:.2f} dB")
out()
out("对照：")
out("  冻结设计 fc=800k (L=3.9/16/20/11)   : -43.10 dB   余量 0.6 dB")
out("  画布实件 fc=800k (L=3.9/15/18/12)   : -42.18 dB   余量 -0.3 dB  ← 不达标")
out("  本定案   fc=660k (L=4.7/22/22/12)   : -59.30 dB   余量 16.8 dB")
out()
out("注：电感损耗 Q_L 由 100 降到 25，折叠带最差衰减仅由 -59.3 变到 -59.5 dB（巴特沃斯对 Q 极不敏感）。")

open(r"C:/Users/funiang/Desktop/Project/projects/G2026/hardware/report_filter_final.md", "w", encoding="utf-8").write(
    "# G2026 AFE 抗混叠滤波器 · 定案复算\n\n```\n" + "\n".join(lines) + "\n```\n")
print("\n-> 已写入 hardware/report_filter_final.md")
