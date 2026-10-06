# -*- coding: utf-8 -*-
"""改前 / 改后 对比（H1 = R21 0Ω→4.7Ω；H2 = R6/R7 69.8k/10k → 24.9k/3.57k）

跑 S1(通带+Cin 敏感性) / S2(MC) / S3(采样) / S5(噪声) / S6(PSRR) / S7(场景)，
两边都跑，输出一张判定表。**这是 H1/H2 落图后的验收证据。**
"""
import json
import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import studies as ST  # noqa: E402
from afe_net import build_afe  # noqa: E402


def db(x):
    return 20 * np.log10(np.maximum(np.abs(x), 1e-30))


def passband(r6, r7, cin_d, cin_c, fs=None):
    if fs is None:
        fs = np.logspace(4, math.log10(5e5), 400)
    c, _ = build_afe(src_ac=1.0, r6=r6, r7=r7, cin_diff=cin_d, cin_cm=cin_c)
    h = db(c.ac(fs)["ADCP"])
    i500 = int(np.argmin(np.abs(fs - 5e5)))
    fc = np.logspace(4, math.log10(5e5), 17)
    hc = np.interp(np.log(fc), np.log(fs), h)
    fit = np.interp(np.log(fs), np.log(fc), hc)
    res = float(np.max(np.abs(h - fit)))
    return dict(peak=float(h.max() - h[0]), at500=float(h[i500] - h[0]),
                resid_db=res, resid_mv=(10 ** (res / 20) - 1) * 250)


def main():
    out = {}
    print("=" * 92)
    print("① 通带形状 + 17 点标定残差（Cin 取三档；残差只在 10k~500k 内评）")
    print("=" * 92)
    print(f"  {'配置':<22}{'Cin':>8}{'峰值起伏':>11}{'@500k':>10}{'标定残差':>11}{'→幅值mV':>10}")
    for tag, r6, r7 in (("改前 69.8k/10k", 69.8e3, 10e3),
                        ("改后 24.9k/3.57k", 24.9e3, 3.57e3)):
        for cd, cc, ct in ((2e-12, 2e-12, '2p/2p'), (8e-12, 4e-12, '8p/4p'),
                           (16e-12, 8e-12, '16p/8p')):
            r = passband(r6, r7, cd, cc)
            print(f"  {tag:<22}{ct:>8}{r['peak']:+10.3f}dB{r['at500']:+9.3f}dB"
                  f"{r['resid_db']:>10.4f}dB{r['resid_mv']:>10.3f}")
            out.setdefault(tag, {})[ct] = r
        # Cin 敏感性跨度
        p2 = passband(r6, r7, 2e-12, 2e-12)['peak']
        p16 = passband(r6, r7, 16e-12, 8e-12)['peak']
        print(f"  → {tag} 的 Cin 敏感性跨度 = {p16 - p2:.3f} dB")
        out.setdefault(tag, {})['cin_span_db'] = p16 - p2

    print()
    print("=" * 92)
    print("② 电源纹波耦合 +5V→ADC（改前 R21=0Ω / 改后 4.7Ω）")
    print("=" * 92)
    fs = np.array([1e5, 5e5])
    for tag, r21 in (("改前 R21=0Ω", 0.0), ("改后 R21=4.7Ω", 4.7)):
        for p in (60.0, 100.0):
            c, _ = build_afe(r21=r21, psrr_db=p, rail_ac=1.0, src_ac=0.0)
            h = db(c.ac(fs)["ADCP"])
            print(f"  {tag:<16} PSRR={p:>5.0f}dB   @100k {h[0]:+8.1f}dB   @500k {h[1]:+8.1f}dB")
            out.setdefault('psrr', {})[f"{tag}_{p:.0f}"] = dict(h100k=float(h[0]), h500k=float(h[1]))

    print()
    print("=" * 92)
    print("③ 噪声预算（改前/改后）")
    print("=" * 92)
    for tag, cfg in (("改前", dict(r21=0.0, r6=69.8e3, r7=10e3)),
                     ("改后", ST.OPT_AFTER)):
        ST.set_config(**cfg)
        s5 = ST.s5_noise()
        print(f"  {tag}  ADC 端噪声底 = {s5['vn_rms_mv']:.4f} mVrms   "
              f"（判据 <0.5）  前三大："
              + ", ".join(f"{k} {v:.4f}" for k, v in list(s5['detail'].items())[:3]))
        out.setdefault('noise', {})[tag] = dict(vn=s5['vn_rms_mv'],
                                                top={k: v for k, v in list(s5['detail'].items())[:5]})

    print()
    print("=" * 92)
    print("④ 题面三场景端到端误差（改前/改后；含 17 点标定）")
    print("=" * 92)
    for tag, cfg in (("改前", dict(r21=0.0, r6=69.8e3, r7=10e3)),
                     ("改后", ST.OPT_AFTER)):
        ST.set_config(**cfg)
        s7 = ST.s7_scenarios()
        for d in s7:
            print(f"  {tag}  {d['name']:<20} 裸 Upp {d['err_upp_raw']:+8.4f} mV | "
                  f"标定后 Upp {d['err_upp_cal']:+7.4f} mV  Urms {d['err_rms_cal']:+7.4f} mV"
                  f"   (判据 ±5 mV)")
        out.setdefault('scenario', {})[tag] = s7

    print()
    print("=" * 92)
    print("⑤ 折叠带 + IM3（改后配置复跑）")
    print("=" * 92)
    ST.set_config(**ST.OPT_AFTER)
    s1 = ST.s1_afe_ac()
    print(f"  折叠带最差 = {s1['fold_worst_db']:.2f} dB   余量 {s1['fold_margin_db']:.1f} dB")
    s4 = ST.s4_im3()
    print(f"  ADC 端 IM3@200k = {s4['adc']['im3_lo']:.4f} mV   门限 1.5mV ⇒ "
          f"余量 {1.5/max(s4['adc']['im3_lo'],1e-12):.0f}×")
    out['fold_db'] = s1['fold_worst_db']
    out['im3_mv'] = s4['adc']['im3_lo']

    os.makedirs('tools/sim/out', exist_ok=True)
    with open('tools/sim/out/verify_opt.json', 'w', encoding='utf-8') as f:
        json.dump(out, f, ensure_ascii=False, indent=1, default=float)
    print("\n→ tools/sim/out/verify_opt.json")


if __name__ == "__main__":
    main()
