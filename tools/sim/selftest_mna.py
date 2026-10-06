# -*- coding: utf-8 -*-
"""mna 内核自检：每一条都与解析解对拍。跑不通就不许往上层建模型。"""
import sys, os, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
from mna import Circuit, mag_db

fails = []
def chk(name, got, want, tol, unit=""):
    ok = abs(got - want) <= tol
    print(f"  {'OK ' if ok else 'FAIL'}  {name:<42} got={got:.6g} want={want:.6g} {unit}")
    if not ok:
        fails.append(name)

print("=" * 78)
print("① RC 低通 —— AC 与解析 |H|=1/√(1+(ωRC)²) 对拍")
print("=" * 78)
c = Circuit()
c.add_V("in", "0", ac_mag=1.0)
c.add_R("in", "mid", 1e3)
c.add_C("mid", "0", 159.15494309189535e-9)   # fc=1kHz
r = c.ac([100.0, 1000.0, 10000.0])
RC = 1e3 * 159.15494309189535e-9
for idx, f in enumerate((100.0, 1000.0, 10000.0)):
    chk(f"|H| @{f:.0f}Hz", abs(r["mid"][idx]), 1/math.sqrt(1+(2*math.pi*f*RC)**2), 1e-9)

print("=" * 78)
print("② RC 充电 —— 瞬态与解析 v=V(1-e^{-t/RC}) 对拍")
print("=" * 78)
c = Circuit()
c.add_V("in", "0", dc=1.0)
c.add_R("in", "mid", 1e3)
c.add_C("mid", "0", 1e-6)          # τ = 1ms
ts, vs = c.tran(3e-3, 1e-6, method="be", ic_vnode={"mid": 0.0})
for t in (0.5e-3, 1e-3, 2e-3):
    k = int(round(t/1e-6))
    chk(f"v(t={t*1e3:.1f}ms) BE", vs["mid"][k], 1-math.exp(-t/1e-3), 2e-3, "V")
ts, vs = c.tran(3e-3, 1e-6, method="trap", ic_vnode={"mid": 0.0})
for t in (0.5e-3, 1e-3, 2e-3):
    k = int(round(t/1e-6))
    chk(f"v(t={t*1e3:.1f}ms) trap", vs["mid"][k], 1-math.exp(-t/1e-3), 2e-3, "V")

print("=" * 78)
print("③ RLC 串联 —— 谐振频率与阻尼对拍")
print("=" * 78)
# f0 = 1/(2π√(LC)), Q = (1/R)√(L/C)  欠阻尼
Lv, Cv, Rv = 4.7e-6, 5.6e-9, 49.9
c = Circuit()
c.add_V("in", "0", dc=0.0, ac_mag=1.0)
c.add_R("in", "a", Rv)
c.add_L("a", "b", Lv)
c.add_C("b", "0", Cv)
f0 = 1/(2*math.pi*math.sqrt(Lv*Cv))
Q = (1/Rv)*math.sqrt(Lv/Cv)
print(f"  解析 f0={f0/1e3:.3f} kHz  Q={Q:.4f}")
# AC 峰值频率扫描
fs = np.linspace(0.5*f0, 2.0*f0, 4001)
r = c.ac(fs)
v = np.abs(r["in"] - r["a"]) / Rv     # 电流口径 |I| = |V_R|/R —— 谐振点才出峰
fpk = fs[int(np.argmax(v))]
chk("AC 谐振频率（电流峰）", fpk/1e3, f0/1e3, f0/1e3*0.02, "kHz")
chk("AC 谐振处 |I| = Vs/R", np.max(v), 1.0/Rv, 1.0/Rv*0.02, "A")
# Q<0.707 ⇒ C 两端无峰；改验 f0 处 |V_C| = Q·Vs（Q<1 也成立）
b0 = np.abs(r["b"][int(np.argmin(np.abs(fs-f0)))])
chk("f0 处 |V_C| = Q·Vs", b0, Q, Q*0.02, "")

print("=" * 78)
print("④ 电感直流短路 / 电容直流开路")
print("=" * 78)
c = Circuit()
c.add_V("in", "0", dc=5.0)
c.add_R("in", "a", 100.0)
c.add_L("a", "b", 1e-3)
c.add_R("b", "0", 100.0)
d = c.dc()
chk("分压中点 (L 短路)", d["b"], 2.5, 1e-6, "V")

print("=" * 78)
print("⑤ 二极管：串联 1k 从 5V 到 D，电流 ≈ (5-0.7)/1k")
print("=" * 78)
c = Circuit()
c.add_V("in", "0", dc=5.0)
c.add_R("in", "a", 1000.0)
c.add_D("a", "0", Is=1e-14, n=1.0, Rs=0.0, label="D1")
d = c.dc()
chk("二极管压降", d["a"], 0.7, 0.06, "V")

print("=" * 78)
print("⑥ 时变开关：t<1ms 断开 / t≥1ms 闭合，观察阶跃响应")
print("=" * 78)
c = Circuit()
c.add_V("in", "0", dc=1.0)
c.add_R("in", "a", 1000.0)
c.add_SW("a", "b", 1.0, 1e9, lambda t: t >= 1e-3, "SW")
c.add_R("b", "0", 1000.0)
c.add_C("b", "0", 1e-6)
ts, vs = c.tran(3e-3, 1e-6, method="trap")
chk("t=0.5ms 开关断开", vs["b"][500], 0.0, 1e-3, "V")
chk("t=3ms  已闭合（4τ 后）", vs["b"][-1], 0.5, 2e-2, "V")

print("=" * 78)
if fails:
    print(f"❌ {len(fails)} 项未过：{fails}")
    sys.exit(1)
print("✅ mna 内核自检全部通过")
