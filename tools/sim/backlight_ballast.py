# -*- coding: utf-8 -*-
"""背光镇流分析：3V3 直驱 / 3V3 串阻 / 5V 串阻 —— 哪个能把电流压进规格。

**已知（规格书，TK035F3296 与 TK035F9629 同一张表）**
  · 4 颗 LED **并联**；要求 **恒流 60 mA ±10%**（`If=60mA(±10%)`，constant current driving）
  · `Vf` @60mA：Min **2.9** / Typ **3.1** / Max **3.6** V
    —— 注意这是**颗与颗之间**的离散，**不是**同一颗的斜率

**已知（用户实测 2026-10-03）**：**3.3 V 能点亮**。这条信息量比看起来大：
  它说明"本机"的 `Vf + I·R_串` 在 3.3 V 下恰好落在 LED 有电流的区间，
  **但它同时说明：电流是由"余量 ÷ 寄生电阻"定的，不是设计定的。**

**未知（显式假设 + 敏感性）**：同一颗 LED 上"电流每十倍程抬多少电压"——
白色 LED 并联组典型 **0.03~0.12 V/decade**，三个值都跑。

**模型的边界（必须写清楚）**：本模型只用 `Vf@60mA` 这一个锚点 + 一个斜率假设，
**不能反推"现在实际是多少 mA"** —— 那需要实测。本文件只回答**结构性问题**：
"给定驱动方式，电流会被压住吗？"

用法：python tools/sim/backlight_ballast.py
"""
import math

VF60_SPEC = (2.9, 3.1, 3.6)             # 规格书的颗间离散（换批次就可能遇上）
VF60_LIT = (2.9, 3.0, 3.1, 3.2, 3.25)   # "3.3V 能亮"夹出来的可用区间
SLOPES = (0.03, 0.06, 0.12)             # V/decade（假设）
RSW = 0.06                              # Q1(SI2302) Rds(on) @Vgs≈3V，量级
I_NOM = 0.060
SPEC_LO, SPEC_HI = 0.054, 0.066
RS = (0.0, 3.3, 5.1, 10.0, 15.0, 22.0, 27.0, 30.0, 33.0, 36.0, 39.0, 43.0, 47.0)
I_CAP = 0.5                             # 超过 500 mA 就报">0.5A"（再往上没工程意义）


def current(vrail, rs, vf60, slope, rsw=RSW):
    """解 Vrail = I·(rsw+rs) + Vf60 + slope·log10(I/60mA)。"""
    lo, hi = 1e-7, I_CAP
    for _ in range(200):
        mid = 0.5 * (lo + hi)
        v = mid * (rsw + rs) + vf60 + slope * math.log10(mid / I_NOM)
        if v < vrail:
            lo = mid
        else:
            hi = mid
    return 0.5 * (lo + hi)


def show(cs):
    out = []
    for c in cs:
        out.append(f"{'>0.5A' if c >= I_CAP * 0.999 else f'{c*1e3:.1f}'}")
    return out


def row(vrail, rs, vfs=VF60_LIT, slope=0.06):
    cs = [current(vrail, rs, v, slope) for v in vfs]
    span = (max(cs) - min(cs)) * 1e3
    ok = all(SPEC_LO <= c <= SPEC_HI for c in cs)
    near = all(0.9 * SPEC_LO <= c <= 1.1 * SPEC_HI for c in cs)
    return cs, span, ("✅ 全部合规" if ok else ("🟡 基本合规（边缘差 ≤10%）" if near else "❌ 不合规"))


def main():
    print("=" * 100)
    print("背光镇流分析  判据 54~66 mA（= 60mA±10%）  Q1 Rds(on)≈0.06Ω 并入串阻")
    print("=" * 100)

    print("\n① 现状 3V3 直驱（Rs=0）—— 电流由**寄生电阻**定，不由设计定")
    for s in SLOPES:
        lo = current(3.3, 0.0, VF60_SPEC[0], s)
        hi = current(3.3, 0.0, VF60_SPEC[2], s)
        print(f"   slope={s:.2f} V/dec :  Vf=3.6（上限）→ {hi*1e3:7.1f} mA（偏暗）"
              f"   Vf=2.9（下限）→ {'>0.5A（烧）' if lo >= I_CAP*0.999 else f'{lo*1e3:.0f} mA'}")
    print("   ⇒ 换到 `Vf`=2.9 那一批就是**过流到安培级**；本机能亮只说明本机 `Vf` 靠近 3.3V 侧。")

    print("\n② 3V3 档能不能靠串阻救？先算「要多大才能居中到 60mA」，再看那个阻值下的跨度")
    for s in SLOPES:
        # 以 typ Vf=3.1 居中
        rs_center = (3.3 - 3.1) / I_NOM
        cs, span, verdict = row(3.3, rs_center, slope=s)
        print(f"   slope={s:.2f} : 居中需 Rs≈{rs_center:.1f}Ω → I = "
              f"{' ~ '.join(show(cs))} mA   跨度 {span:.0f} mA   {verdict}")
    print("   ⇒ 3V3 的余量（≈0.2 V）**小于** `Vf` 的颗间离散（0.35 V）⇒ 居中与窄跨度不可兼得。")
    print("      （串大阻值确实能把跨度压小，但电流也随之掉到 60 mA 以下 —— 那不是「救」，是「关掉」。）")

    print("\n③ 5V 档（余量 ≈1.8 V ≫ 离散 0.35 V）—— 逐档看")
    print(f"   {'Rs(Ω)':>7}{'Vf=2.9':>10}{'Vf=3.0':>10}{'Vf=3.1':>10}"
          f"{'Vf=3.2':>10}{'Vf=3.25':>11}{'跨度(mA)':>11}   判")
    for r in RS:
        cs, span, verdict = row(5.0, r)
        if r == 0.0:
            print(f"   {r:>7.1f}{'':>10}{'':>10}{'':>10}{'':>10}{'':>11}{'—':>11}   ❌ 恒压直驱，安培级")
            continue
        print(f"   {r:>7.1f}" + "".join(f"{v:>10}" for v in show(cs))
              + f"{span:>11.0f}   {verdict}")

    print("\n④ 选定档：**+5V 串 33Ω**")
    for s in SLOPES:
        cs, span, verdict = row(5.0, 33.0, slope=s)
        print(f"   slope={s:.2f} V/dec :  I = {min(cs)*1e3:.1f} ~ {max(cs)*1e3:.1f} mA   跨度 {span:.1f} mA   {verdict}")
    i_max = max(current(5.0, 33.0, v, 0.12) for v in VF60_LIT)
    print(f"   电阻耗散（最大电流 {i_max*1e3:.1f} mA）：{i_max**2*33:.3f} W ⇒ 用 **1206 / 0.25W**")
    print(f"   顺带把 60 mA 从 +3V3 挪到 +5V：核心板 LDO 耗散 "
          f"{(5-3.3)*0.082:.3f} W → {(5-3.3)*0.022:.3f} W")
    print("\n   注：Vf=3.25 那一端会到 53.0 mA（规格下沿 54 mA 差 1 mA）——"
          "\n   即「基本合规」。要真正严格合规只能上**恒流驱动**（适配板加恒流沉或换带驱动的模组）。")


if __name__ == '__main__':
    main()
