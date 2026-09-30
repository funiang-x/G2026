# -*- coding: utf-8 -*-
"""G2026 · 抗混叠方案对比（ABCD 级联，与 _lc_recheck.py 同一内核）。
   判据：折叠带 [1.5,2.5]MHz 内**最差**衰减 >= 42.5dB；带内 10k~500k 尽量平。"""
import numpy as np
from scipy import signal

R = 50.0
BAND = np.linspace(1.5e6, 2.5e6, 4001)
INBAND = np.linspace(10e3, 500e3, 2001)

G8 = [0.39018, 1.11114, 1.66294, 1.96157, 1.96157, 1.66294, 1.11114, 0.39018]
E24 = np.array([1.0, 1.1, 1.2, 1.3, 1.5, 1.6, 1.8, 2.0, 2.2, 2.4, 2.7, 3.0,
                3.3, 3.6, 3.9, 4.3, 4.7, 5.1, 5.6, 6.2, 6.8, 7.5, 8.2, 9.1])

def e24(x):
    if x <= 0: return x
    dec = 10 ** np.floor(np.log10(x))
    return min(E24, key=lambda e: abs(e - x / dec)) * dec

def ladder_db(Ls, Cs, f):
    """ABCD 级联：Rs -> L1 串 -> C2 并 -> L3 串 -> C4 并 -> ... -> RL"""
    f = np.atleast_1d(np.asarray(f, dtype=float))
    w = 2 * np.pi * f
    A = np.ones_like(w, dtype=complex); B = np.zeros_like(A)
    C = np.zeros_like(A); D = np.ones_like(A)
    def mul(A, B, C, D, a, b, c, d):
        return (A*a + B*c, A*b + B*d, C*a + D*c, C*b + D*d)
    for i in range(len(Ls)):
        A, B, C, D = mul(A, B, C, D, 1, 1j*w*Ls[i], 0, 1)      # 串 L
        A, B, C, D = mul(A, B, C, D, 1, 0, 1j*w*Cs[i], 1)      # 并 C
    denom = A*R + B + C*R*R + D*R
    return 20*np.log10(np.abs(2*np.sqrt(R*R)/denom))

def zpk_db(z, p, k, f):
    s = 1j*2*np.pi*np.atleast_1d(np.asarray(f, float))
    num = np.ones_like(s); den = np.ones_like(s)
    for zz in z: num *= (s - zz)
    for pp in p: den *= (s - pp)
    return 20*np.log10(np.abs(k*num/den))

print("="*100)
print("A. 巴特沃斯 n=8 · E24 取整后的实际响应")
print("="*100)
print(f"{'fc':>6} | {'元件值 (L uH / C nF)':<44} | {'折叠带最差':>10} | {'@500k':>8} | {'@1.5M':>8} | {'带内起伏':>8}")
for fc in [650e3, 700e3, 750e3, 800e3]:
    wc = 2*np.pi*fc
    Ls = [e24(g*R/wc) for g in G8[0::2]]
    Cs = [e24(g/(wc*R)) for g in G8[1::2]]
    worst = ladder_db(Ls, Cs, BAND).max()
    d500 = ladder_db(Ls, Cs, [500e3])[0]
    d15 = ladder_db(Ls, Cs, [1.5e6])[0]
    rip = ladder_db(Ls, Cs, INBAND)
    txt = "L=" + "/".join(f"{x*1e6:g}" for x in Ls) + "  C=" + "/".join(f"{x*1e9:g}" for x in Cs)
    print(f"{fc/1e3:>4.0f}k | {txt:<44} | {worst:>9.2f}  | {d500:>7.3f}  | {d15:>7.2f}  | {rip.max()-rip.min():>7.3f}")

print()
print("="*100)
print("B. 椭圆（Cauer）· 理想响应（上限；梯形提取另算）")
print("="*100)
print(f"{'设计':<32} | {'折叠带最差':>10} | {'@500k':>8} | {'带内起伏':>8} | {'零点 kHz':<30}")
for n, fp, amax, amin in [(4,500e3,0.1,50),(4,500e3,0.1,42.5),(4,520e3,0.1,45),
                          (5,500e3,0.1,50),(6,500e3,0.1,50),(6,500e3,0.05,42.5)]:
    z,p,k = signal.ellip(n, amax, amin, 2*np.pi*fp, btype='low', analog=True, output='zpk')
    worst = zpk_db(z,p,k,BAND).max()
    d500 = zpk_db(z,p,k,[500e3])[0]
    ib = zpk_db(z,p,k,INBAND)
    zf = sorted({round(abs(zz)/2/np.pi/1e3,1) for zz in z if abs(zz)>1})
    print(f"n={n} fp={fp/1e3:.0f}k Amax={amax} Amin={amin:<4} | {worst:>9.2f}  | {d500:>7.3f}  | {ib.max()-ib.min():>7.3f}  | {str(zf):<30}")
