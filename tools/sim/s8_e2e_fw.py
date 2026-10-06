# -*- coding: utf-8 -*-
"""S8 · 题面示例的**端到端复现**：源信号 → AFE → 12bit ADC → 固件峰检测链 → 显示值

这是全链最后一环，也是最直接的一项验证：题面**自己给了验收示例**
（u = U1·sin(2π·10.5k·t) + U3·sin(2π·31.5k·t) + U4·sin(2π·42.0k·t) mV，
装置应显示 10.5kHz/U1、31.5kHz/U3、42.0kHz/U4），
本脚本就按这个示例跑一遍，把**显示值**与**标称值**逐项对判据（5mV / 1kHz）。

固件算法**逐句照搬** `firmware/Operation/Src/op_peak.c` + `op_meas.c`：
  窗 = 固件自己的 `op_dsp_winh4[]` 表（从 .c 里抽出来的，不是我另造的 BH4）
  ① 栅格预筛 2·mag/(N·CG) ≥ 1.5mV
  ② log 域三点抛物线 → d ∈ [−0.5, 0.5]
  ③ fc0 ± Δf/8 三点 |X| 抛物线 → d2
  ④ 单点 DFT 读数 amp = 2|X|/(N·CG)，φ = atan2 + π/2（sin 载波补偿）
  ⑤ 标定修正（g、p_corr）→ 终判（门限 + 频带）
  Upp = sin 重建 max−min（抽点 max(512, 32·m_max)）；Urms = √(ΣA²/2)
"""
from __future__ import annotations

import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from afe_net import V, build_afe  # noqa: E402

N = 4096
FS = 2.0e6
DF = FS / N                     # 488.28 Hz
CG = 0.35875                    # 固件 op_dsp_winh4.h 的 OP_DSP_CG
F_LO, F_HI = 9000.0, 502000.0   # 固件 op_peak 的默认 s_p
GATE_MV = 1.5
WIN = np.load(os.path.join(os.path.dirname(os.path.abspath(__file__)), "_winh4.npy"))


# ---------------------------------------------------------------- 固件算法
def dft_at(xw, f):
    n = len(xw)
    k = np.arange(n)
    X = np.sum(xw * np.exp(-2j * np.pi * f * k / FS))
    return X


def op_peak_run(xw, cal_mag=None, cal_ph=None):
    """返回峰表 [(f_hz, amp_mv, phi_rad), ...] —— 逐句照搬 op_peak_run()。"""
    n = len(xw)
    mag = np.abs(np.fft.rfft(xw))
    k_lo = int(F_LO / DF)
    k_hi = int(min(F_HI / DF, n // 2 - 1))
    floor_v = max(mag[k_lo:k_hi + 1].max(), 1e-30) * 1e-9
    out = []
    k = k_lo + 1
    while k < k_hi - 3 and len(out) < 16:
        if mag[k] >= mag[k - 1] and mag[k] > mag[k + 1]:
            if 2.0 * mag[k] / (n * CG) >= GATE_MV:
                a, b, c = (math.log(max(mag[k - 1], floor_v)),
                           math.log(max(mag[k], floor_v)),
                           math.log(max(mag[k + 1], floor_v)))
                den = a - 2 * b + c
                d = 0.5 * (a - c) / den if den != 0 else 0.0
                d = min(0.5, max(-0.5, d))
                fc0 = (k + d) * DF
                m3 = [abs(dft_at(xw, fc0 + (j - 1) * DF / 8.0)) for j in (0, 1, 2)]
                den2 = m3[0] - 2 * m3[1] + m3[2]
                d2 = 0.5 * (m3[0] - m3[2]) / den2 if den2 != 0 else 0.0
                d2 = min(0.5, max(-0.5, d2))
                f_c = fc0 + d2 * DF / 8.0
                X = dft_at(xw, f_c)
                amp = 2.0 * abs(X) / (n * CG)
                phi = math.atan2(X.imag, X.real) + math.pi / 2.0   # sin 载波补偿
                if cal_mag is not None:
                    g = float(np.interp(f_c, cal_mag[0], cal_mag[1]))
                    pc = float(np.interp(f_c, cal_ph[0], cal_ph[1]))
                    if g > 0:
                        amp /= g
                    phi -= pc
                if amp >= GATE_MV and F_LO <= f_c <= F_HI:
                    out.append((f_c, amp, phi))
            k += 5
        else:
            k += 1
    return out


def op_meas(peaks):
    """照搬 op_meas_build()：Upp = sin 重建 max−min；Urms = √(ΣA²/2)。"""
    if not peaks:
        return None
    f1 = min(p[0] for p in peaks)
    urms = math.sqrt(sum(p[1] ** 2 for p in peaks) / 2.0)
    m = [max(1, int(p[0] / f1 + 0.5)) for p in peaks]
    m_max = max(m)
    pts = min(4096, max(512, 32 * m_max))
    t = np.arange(pts) / (f1 * pts)
    w = np.zeros(pts)
    for (f, a, ph), mk in zip(peaks, m):
        w += a * np.sin(2 * math.pi * mk * f1 * t + ph)
    return dict(f1=f1, urms=urms, upp=w.max() - w.min(), n=len(peaks), pts=pts)


# ---------------------------------------------------------------- 场景
def run_case(name, freqs, amps_mv, cal=True, uj=None, gnet=1.995):
    """freqs/amps_mv = 各分量频率与**峰值**（mV）；uj = (f, 峰值mV) 或 None。

    ⚠️ 标称 Upp 必须用**细网格**算：fs=2MHz 下 500kHz 分量每周期只有 4 个采样点，
       用采样点取 max−min 会**系统性低估**（本脚本第一版就踩了这个坑，把固件的
       正确结果误判成 +14.6mV 超差）。固件用频域 sin 重建，恰恰是为了避开这一点。
    """
    # 1) 源端波形（EMF），峰值口径 —— 标称值在**细网格**上算
    fine = 200000
    tf = np.arange(fine) / (FS * fine / 4000.0)      # 4000 个 ADC 周期，dt = 0.5ns
    xf = np.zeros(fine)
    for f, a in zip(freqs, amps_mv):
        xf += a * np.sin(2 * math.pi * f * tf + 0.3 * f / 1e3)
    if uj:
        xf += uj[1] * np.sin(2 * math.pi * uj[0] * tf)
    upp_nom = (xf.max() - xf.min())          # mV（源端 EMF 的真峰峰值，含 u_J）
    urms_nom = float(np.sqrt(np.mean((xf - xf.mean()) ** 2)))
    # 题面在场景 3 要的是 **u_b 的** Upp/Urms ⇒ 另算一个"只有 u_b"的标称
    xb = np.zeros(fine)
    for f, a in zip(freqs, amps_mv):
        xb += a * np.sin(2 * math.pi * f * tf + 0.3 * f / 1e3)
    upp_nom_b = (xb.max() - xb.min())
    urms_nom_b = float(np.sqrt(np.mean((xb - xb.mean()) ** 2)))

    t = np.arange(N) / FS
    x = np.zeros(N)
    for f, a in zip(freqs, amps_mv):
        x += a * np.sin(2 * math.pi * f * t + 0.3 * f / 1e3)
    if uj:
        x += uj[1] * np.sin(2 * math.pi * uj[0] * t)

    # 2) 前端（LTI，逐谐波复数传递）
    c, _ = build_afe(src_ac=1.0, r21=4.7, r6=24.9e3, r7=3.57e3)
    fs_grid = np.logspace(3, math.log10(3e6), 900)
    H = c.ac(fs_grid)["ADCP"]
    fc17 = np.logspace(4, math.log10(5e5), 17)
    Hc = np.interp(fc17, fs_grid, H)
    cal_mag = (fc17, np.abs(Hc)) if cal else None
    cal_ph = (fc17, np.unwrap(np.angle(Hc))) if cal else None

    y = np.zeros(N)
    allf = list(zip(freqs, amps_mv)) + ([(uj[0], uj[1])] if uj else [])
    for f, a in allf:
        h = np.interp(f, fs_grid, H)
        y += a * abs(h) * np.sin(2 * math.pi * f * t + 0.3 * f / 1e3 + np.angle(h))
    if not cal:
        y = y / gnet                       # 裸：只除一个常数净增益

    # 3) 12bit 量化（VREF 3.3V，直流点 VBIAS 1.65V）
    code = np.clip(np.round((y + 1650.0) / 3300.0 * 4096), 0, 4095)
    yq = code / 4096 * 3300.0 - 1650.0     # mV

    # 4) 固件链
    xw = yq * WIN
    peaks = op_peak_run(xw, cal_mag, cal_ph)
    r = op_meas(peaks)
    if r is not None:
        r["peaks"] = peaks
    return dict(name=name, cal=cal, upp_nom=upp_nom, urms_nom=urms_nom,
                upp_nom_b=upp_nom_b, urms_nom_b=urms_nom_b,
                peaks=peaks, meas=r)


def main():
    print("=" * 96)
    print("S8 · 题面示例端到端复现（源 → AFE → 12bit ADC → 固件峰检测链 → 显示值）")
    print("=" * 96)
    # 题面原例：u = U1·sin(2π·10.5k·t) + U3·sin(2π·31.5k·t) + U4·sin(2π·42.0k·t) mV
    ex_f = [10.5e3, 31.5e3, 42.0e3]
    cases = [
        ("题面原例（U1=80,U3=40,U4=20 mV 峰值）", ex_f, [80.0, 40.0, 20.0], None),
        ("场景 1：u_a 含 200kHz 边界（10k/50k/200k）", [10e3, 50e3, 200e3],
         [80.0, 40.0, 20.0], None),
        ("场景 2：u_b **最大信号** 250mVpp（500k/100k/10k）", [500e3, 100e3, 10e3],
         [71.5, 35.75, 17.875], None),
        ("场景 2：u_b **最小信号** 50mVpp（500k/100k/10k）", [500e3, 100e3, 10e3],
         [14.3, 7.15, 3.575], None),
        ("场景 3：u_b(10.5k/105k/420k) ⊕ u_J(1.5MHz)", [10.5e3, 105e3, 420e3],
         [80.0, 40.0, 20.0], (1.5e6, 100.0)),
        ("场景 3：u_b(10.5k/105k/420k) ⊕ u_J(1.0MHz)", [10.5e3, 105e3, 420e3],
         [80.0, 40.0, 20.0], (1.0e6, 100.0)),
        ("场景 3：u_b 小信号 ⊕ u_J(1.2MHz)", [10.5e3, 105e3, 420e3],
         [18.0, 9.0, 4.5], (1.2e6, 100.0)),
        ("500kHz 场景（500k/100k/10k）", [500e3, 100e3, 10e3], [25.0, 12.5, 6.25], None),
    ]
    allok = True
    for tag, freqs, amps_mv, uj in cases:
        for cal in (False, True):
            r = run_case(tag, freqs, amps_mv, cal=cal, uj=uj)
            m = r["meas"]
            if m is None:
                print(f"\n[{tag}] 标定={cal}  → **未检出任何峰** ❌")
                allok = False
                continue
            print(f"\n[{tag}]  标定={'开' if cal else '关'}")
            ref_upp = r['upp_nom_b'] if uj else r['upp_nom']
            ref_rms = r['urms_nom_b'] if uj else r['urms_nom']
            tag_ref = 'u_b 的' if uj else ''
            print(f"  标称 {tag_ref}Upp = {ref_upp:8.3f} mV   实测 Upp = {m['upp']:8.3f} mV   "
                  f"误差 {m['upp']-ref_upp:+7.3f} mV")
            print(f"  标称 {tag_ref}Urms= {ref_rms:8.3f} mV   实测 Urms= {m['urms']:8.3f} mV   "
                  f"误差 {m['urms']-ref_rms:+7.3f} mV"
                  + (f"   （u 含 u_J 的 Upp 标称 {r['upp_nom']:.3f} mV）" if uj else ""))
            if abs(m['upp'] - ref_upp) > 5.0 or abs(m['urms'] - ref_rms) > 5.0:
                allok = False
            print(f"  基频实测 = {m['f1']/1e3:.4f} kHz")
            print(f"  {'标称 f(kHz)':>12}{'标称 A(mV)':>12}{'实测 f(kHz)':>13}{'实测 A(mV)':>12}"
                  f"{'Δf(Hz)':>10}{'ΔA(mV)':>10}")
            # 逐分量比对：u_b/u_a 的分量**必须**检出；u_J **必须检不出**（这是设计目标）
            for f0, a0 in zip(freqs, amps_mv):
                best = min(m["peaks"], key=lambda p: abs(p[0] - f0))
                df = best[0] - f0
                da = best[1] - a0
                flag = "✅" if (abs(df) <= 1000 and abs(da) <= 5.0) else "❌"
                print(f"  {f0/1e3:>12.4f}{a0:>12.3f}{best[0]/1e3:>13.4f}{best[1]:>12.3f}"
                      f"{df:>10.1f}{da:>10.3f}  {flag}")
                if abs(df) > 1000 or abs(da) > 5.0:
                    allok = False
            if uj:
                # u_J 应被抑制：峰表里不应有 f_J ± 5kHz 内的峰
                near = [p for p in m["peaks"] if abs(p[0] - uj[0]) < 5e3]
                ok_uj = not near
                print(f"  u_J 抑制：峰表里 {'无' if ok_uj else '有'} {uj[0]/1e6:.1f}MHz 附近的峰"
                      f"  {'✅ 抑制达标' if ok_uj else '❌ 漏进来了'}")
                if not ok_uj:
                    allok = False
                # 再查带内是否出现伪峰（除已知分量外的峰）
                spurious = [p for p in m["peaks"]
                            if min(abs(p[0] - f) for f in freqs) > 5e3]
                print(f"  带内伪峰：{len(spurious)} 个"
                      + ("  ✅" if not spurious else
                         "  ❌ " + ", ".join(f"{p[0]/1e3:.1f}kHz/{p[1]:.3f}mV" for p in spurious)))
                if spurious:
                    allok = False
    print("\n" + "=" * 96)
    print("✅ S8 全部通过（逐分量 |Δf| ≤ 1kHz 且 |ΔA| ≤ 5mV）" if allok
          else "❌ S8 有分量超出判据，见上面的 ❌ 行")
    return allok


if __name__ == "__main__":
    ok = main()
    sys.exit(0 if ok else 1)
