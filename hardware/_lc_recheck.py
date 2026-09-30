# -*- coding: utf-8 -*-
"""G2026 AFE · LC 梯形滤波器数值复算：设计值 vs 画布实际值。
   双端接 50Ω 8 阶巴特沃斯，级联 ABCD 矩阵求 |S21|。"""
import cmath, math

R = 50.0
FC = 800e3

def ladder_resp(Ls, Cs, f):
    """从源到负载：Rs -> L1(串) -> C2(并) -> L3 -> C4 -> L5 -> C6 -> L7 -> C8 -> RL"""
    w = 2 * math.pi * f
    M = [[1, 0], [0, 1]]
    def mul(A, B):
        return [[A[0][0]*B[0][0] + A[0][1]*B[1][0], A[0][0]*B[0][1] + A[0][1]*B[1][1]],
                [A[1][0]*B[0][0] + A[1][1]*B[1][0], A[1][0]*B[0][1] + A[1][1]*B[1][1]]]
    for i in range(4):
        z = 1j * w * Ls[i]
        M = mul(M, [[1, z], [0, 1]])
        y = 1j * w * Cs[i]
        M = mul(M, [[1, 0], [y, 1]])
    A, B, C, D = M[0][0], M[0][1], M[1][0], M[1][1]
    Zs, Zl = R, R
    denom = A * Zl + B + C * Zs * Zl + D * Zs
    return 2 * math.sqrt(Zs * Zl) / denom

def db(x):
    return 20 * math.log10(abs(x))

DESIGN_L = [3.9e-6, 16e-6, 20e-6, 11e-6]
DESIGN_C = [4.3e-9, 7.5e-9, 6.8e-9, 1.6e-9]
DRAWN_L = [3.9e-6, 15e-6, 18e-6, 12e-6]
DRAWN_C = [4.3e-9, 7.5e-9, 6.8e-9, 1.6e-9]

# 理论值（E24 之前的连续解）
G = [0.39018, 1.11114, 1.66294, 1.96157]
IDEAL_L = [g * R / (2 * math.pi * FC) for g in G]
IDEAL_C = [g / (2 * math.pi * FC * R) for g in G]

print("理想 8 阶 BW @fc=800k, 50Ω 双端接：")
print("  L =", [round(x * 1e6, 3) for x in IDEAL_L], "uH")
print("  C =", [round(x * 1e9, 3) for x in IDEAL_C], "nF")
print("  E24 取整: L = 3.9/16/20/11 uH, C = 4.3/7.5/6.8/1.6 nF  (= 03-components.md)")
print()

freqs = [10e3, 100e3, 500e3, 800e3, 1.0e6, 1.5e6, 2.0e6, 2.5e6]
print(f"{'f':>9} | {'设计值 dB':>10} | {'画布值 dB':>10} | {'差值 dB':>8}")
print("-" * 50)
for f in freqs:
    a = db(ladder_resp(DESIGN_L, DESIGN_C, f))
    b = db(ladder_resp(DRAWN_L, DRAWN_C, f))
    print(f"{f/1e3:>7.0f}k | {a:>10.3f} | {b:>10.3f} | {b-a:>+8.3f}")

print()
print("关键判据（折叠带起点 1.5MHz 需 <= -42.5dB）：")
a = db(ladder_resp(DESIGN_L, DESIGN_C, 1.5e6))
b = db(ladder_resp(DRAWN_L, DRAWN_C, 1.5e6))
print(f"  设计值 @1.5M = {a:.2f} dB   画布值 @1.5M = {b:.2f} dB")
print(f"  -> 画布值{'满足' if b <= -42.5 else '不满足'} -42.5dB 门槛，余量 {abs(b)-42.5:+.2f} dB")

print()
print("通带 droop（相对 10kHz）：")
for f in [100e3, 500e3]:
    a = db(ladder_resp(DESIGN_L, DESIGN_C, f)) - db(ladder_resp(DESIGN_L, DESIGN_C, 10e3))
    b = db(ladder_resp(DRAWN_L, DRAWN_C, f)) - db(ladder_resp(DRAWN_L, DRAWN_C, 10e3))
    print(f"  @{f/1e3:.0f}k: 设计 {a:+.3f} dB / 画布 {b:+.3f} dB")
