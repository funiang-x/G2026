# -*- coding: utf-8 -*-
"""G2026 · 七项模拟仿真研究（S1~S7）

每项都回答一个**手工算不出或算不准**的硬件问题，并给出可判定的数字。
全部只用 tools/sim/mna.py + afe_net.py，不依赖任何外部 SPICE。

S1 AFE 端到端 AC（含运放非理想性）      → 通带 droop / 折叠带衰减 / 标定残差
S2 蒙特卡洛容差（含寄生）               → 折叠带最坏衰减的分布
S3 ADC 采样保持反冲（瞬态）             → 12bit 建立误差 vs R8/C10
S4 两音互调 IM3（非线性瞬态）           → 带内 IM3 是否超 1.5mV 门限
S5 噪声预算                             → TP4 噪声底 vs 0.5mVrms 判据
S6 电源纹波耦合（PSRR 路径）            → R21 该不该从 0Ω 改 4.7Ω
S7 题面三场景端到端误差                 → 对 5mV / 1kHz 判据的余量
"""
from __future__ import annotations

import math
import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from afe_net import V, build_afe, dft_amp, bh4  # noqa: E402

# ---------------- 全局配置（"改前 / 改后"一键切换） ----------------
# ⚠️ 改前基线**必须显式写值**：2026-10-03 之前这里靠 `r6=None, r7=None` 落到
#    `afe_net.V` 的默认值上，而那次 H2 改图只改了图纸、没同步 afe_net ⇒ 改前基线
#    悄悄变成了改后基线。现在两边都写死，谁都不依赖对方。
OPT = dict(r21=0.0, r6=69.8e3, r7=10e3)           # 改前（2026-10-02 图纸）
# ⚠️ 2026-10-15 按**实物**同步：`R6` = 6.8k（E24）、`R7` = 1k ⇒ 净增益 2.0 → **1.95**。
#    本项 = 活体实物，`afe_net.V["R6"]` / `indep_check.OPT_LIVE` 已同步为 6.8e3 / 1e3。
OPT_AFTER = dict(r21=4.7, r6=6.8e3, r7=1e3)        # 改后（H1+H2+实物换件，= 活体实物）


def set_config(**kw):
    OPT.update(kw)


def _mk(**kw):
    a = dict(OPT)
    a.update(kw)
    return build_afe(**a)
from mna import Circuit  # noqa: E402

K_B = 1.380649e-23
T0 = 300.0
FS = 2.0e6          # ADC 采样率
NFFT = 4096
GATE_MV = 1.5       # 固件峰门限
CRIT_MV = 5.0       # 题面幅值判据
EN_OPA = 5.5e-9     # OPA2197 输入电压噪声密度 V/√Hz


def db(x):
    return 20 * np.log10(np.maximum(np.abs(x), 1e-30))


def sweep(fmin=1e3, fmax=3e6, n=200):
    """对数 + 折叠带加密；**必须单调**，且把 1.5MHz（折叠带起点）与 0.5MHz 钉进栅格。"""
    a = np.logspace(math.log10(fmin), math.log10(1.2e6), int(n * 0.5))
    b = np.linspace(1.2e6, fmax, int(n * 0.5))
    g = np.unique(np.concatenate([a, b, [5e5, 1.5e6]]))
    return np.sort(g)


# ============================================================ S1
def s1_afe_ac(cin_diff=None, cin_cm=None, r6=None, r7=None):
    """端到端 AC：源端 EMF → ADC 输入。"""
    if cin_diff is not None:
        V["Cin_diff"] = cin_diff
    if cin_cm is not None:
        V["Cin_cm"] = cin_cm
    c, _ = _mk(src_ac=1.0)
    if r6 is not None:
        for i, (n1, n2, ohm, lb) in enumerate(c.R):
            if lb == "R6":
                c.R[i] = (n1, n2, r6, lb)
            if lb == "R7":
                c.R[i] = (n1, n2, r7, lb)
    fs = sweep()
    r = c.ac(fs)
    H = r["ADCP"]
    pb = abs(H[0])                      # 1kHz 通带基准（最接近 DC 的点）
    fold = (fs >= 1.5e6) & (fs <= 2.5e6)
    worst = float(np.max(db(H[fold])))   # 最差 = 衰减最小
    k500 = int(np.argmin(np.abs(fs - 5e5)))
    k100 = int(np.argmin(np.abs(fs - 1e5)))
    # 17 点对数标定表的插值残差（与固件同口径：log-f 线性插值）
    fcal = np.logspace(math.log10(1e4), math.log10(5e5), 17)
    hcal = np.interp(np.log(fcal), np.log(fs), db(H))
    fgrid = np.logspace(math.log10(1e4), math.log10(5e5), 2001)
    htrue = np.interp(np.log(fgrid), np.log(fs), db(H))
    hfit = np.interp(np.log(fgrid), np.log(fcal), hcal)
    resid_db = float(np.max(np.abs(htrue - hfit)))
    # 残差折算成幅值误差（在 Upp=250mV 的最坏输入上）
    resid_mv = (10 ** (resid_db / 20) - 1) * 250.0
    return dict(
        fs=fs, H=H, passband_db=db(pb),
        droop_100k=float(db(H[k100]) - db(pb)),
        droop_500k=float(db(H[k500]) - db(pb)),
        fold_worst_db=worst,
        fold_margin_db=-(worst) - 42.5,
        cal_resid_db=resid_db, cal_resid_mv=resid_mv,
        h500=float(abs(H[k500])),
    )


# ============================================================ S2
def s2_mc(nrun=800, seed=20261003, l_tol=0.10, c_tol=0.05, dcr=0.0):
    """蒙特卡洛：L±10% / C±5%，另可开电感 DCR。"""
    rng = np.random.default_rng(seed)
    fs = sweep(1.5e6, 2.5e6, 120)
    fold = (fs >= 1.5e6) & (fs <= 2.5e6)
    fpb = np.array([1e4, 1e5, 5e5])
    worsts, ripples, d500 = [], [], []
    for _ in range(nrun):
        ls = [1 + rng.uniform(-l_tol, l_tol) for _ in range(4)]
        cs = [1 + rng.uniform(-c_tol, c_tol) for _ in range(4)]
        c, _ = _mk(l_scale=ls, c_scale=cs, l_dcr=dcr, src_ac=1.0)
        r = c.ac(fs)
        h = db(r["ADCP"])
        worsts.append(float(np.max(h[fold])))
        rp = c.ac(fpb)
        hh = db(rp["ADCP"])
        ripples.append(float(hh.max() - hh.min()))
        d500.append(float(hh[-1] - hh[0]))
    worsts = np.array(worsts)
    return dict(n=nrun, fold_worst_min=float(worsts.min()),
                fold_worst_med=float(np.median(worsts)),
                fail=int(np.sum(worsts > -42.5)),
                ripple_max=float(np.max(ripples)),
                droop500_max=float(np.min(d500)),
                hist=worsts)


# ============================================================ S3
def s3_adc_kickback(r8_list=(100.0, 220.0, 470.0), c10_list=(220e-12, 470e-12, 1000e-12),
                    ts=83.3e-9, v_prev=1.65 - 0.249, v_now=1.65 + 0.249, dt=0.02e-9):
    """
    ADC 采样保持反冲：开关闭合 t_s，Cadc 从上一次的采样值开始，看本次采到的误差。
    最坏情形取 500kHz 满摆幅时相邻两点的差（ΔV = 2×0.249V）。

    ⚠️ 关键：**驱动源必须带 R8 的串联阻抗**。第一版把 ADCP 用理想源钉死，
    C10 就完全不起作用（实测 9 组结果一模一样）——那是模型错，不是电路对。
    这里把 op-amp 输出等效成"理想源 + R8"（DC 下 Zout≈0，故串联阻抗就是 R8）。
    """
    out = []
    for r8 in r8_list:
        for c10 in c10_list:
            c, d = _mk(r8=r8, c10=c10, adc_load=False, src_ac=0.0)
            AG = d["AG"]
            c.add_V("VDRV", "0", dc=v_now, label="VDRV")
            c.add_R("VDRV", "GOUT", 1e-3, "drv")      # 理想源接到 U1B 输出端
            c.add_SW("ADCP", "ADCIN", V["Rsw"], 1e12, lambda t, ts=ts: t <= ts, "SW")
            c.add_C("ADCIN", AG, V["Cadc"], "Cadc")
            ts_arr, vs = c.tran(ts * 4.0, dt, method="be",
                                ic_vnode={"ADCIN": v_prev, "ADCP": v_now, "GOUT": v_now})
            k = int(round(ts / dt))
            sampled = vs["ADCIN"][k]
            droop = (vs["ADCP"][1] - v_now) * 1e3
            out.append(dict(r8=r8, c10=c10,
                            err_mv=float((sampled - v_now) * 1e3),
                            err_lsb=float(abs(sampled - v_now) / (3.3 / 4096)),
                            droop_mv=float(droop)))
    return out


# ============================================================ S4
def s4_im3(fb=400e3, fj=1.0e6, ab_pp=125e-3, aj_pp=200e-3, nl_scale=1.0,
           tstop=25e-6, dt=1e-9, psrr_db=100.0):
    """
    两音互调：源端 u_b(fb) + u_J(fj)（题面峰峰值口径）。
    20µs 恰好是 400kHz 的 8 个整周期、1MHz 的 20 个整周期 ⇒ DFT 无泄漏。
    测 BUF_OUT（U1A 出，IM3 的产生处）与 ADCP（ADC 入，IM3 的最终落点）。
    """
    c, _ = _mk(nl_scale=nl_scale, psrr_db=psrr_db)
    # 源端 EMF = u_b + u_J（题面峰峰值口径 → 峰值 = 一半）
    ab = ab_pp / 2.0
    aj = aj_pp / 2.0
    for i, (np_, nn_, dc, lb, am, ap) in enumerate(c.V):
        if lb == "VSRC":
            c.V[i] = (np_, nn_, dc, lb, 1.0, 0.0)
            c.VWAVE[i] = (lambda t, a1=ab, a2=aj, f1=fb, f2=fj:
                          a1 * math.sin(2 * math.pi * f1 * t)
                          + a2 * math.sin(2 * math.pi * f2 * t))
    ts, vs = c.tran(tstop, dt, method="trap")
    k0 = int(len(ts) * 0.2)          # 丢掉前 20% 让电路进入稳态
    # 取整数周期窗：tstop 与窗长都取 5µs 的整数倍 ⇒ 400k/1M/200k/600k 全为整周期
    n = int(round(20e-6 / dt))
    if k0 + n > len(ts):
        k0 = len(ts) - n
    fsmp = 1.0 / dt
    idx0 = k0

    def spec(node):
        x = vs[node][idx0:idx0 + n]
        # 整周期 ⇒ 矩形窗即无泄漏（不用加窗，避免窗的相干增益误差）
        return dict(
            b=dft_amp(x, fsmp, fb) * 1e3,
            j=dft_amp(x, fsmp, fj) * 1e3,
            im3_lo=dft_amp(x, fsmp, abs(2 * fb - fj)) * 1e3,
            im3_hi=dft_amp(x, fsmp, 2 * fb + fj) * 1e3,
            im2_lo=dft_amp(x, fsmp, fj - fb) * 1e3,
        )
    return dict(nl_scale=nl_scale, fb=fb, fj=fj,
                buf=spec("BUF_OUT"), adc=spec("ADCP"),
                npts=n, dt=dt, tstop=tstop)


# ============================================================ S5
def s5_noise(fmin=1e4, fmax=5e5, npts=120):
    """噪声预算：电阻热噪声（逐个源的传递阻抗）+ 运放 en + ADC 量化。"""
    fs = np.logspace(math.log10(fmin), math.log10(fmax), npts)
    # 基准 AC 传递（信号路径与各级）
    c0, _ = _mk(src_ac=1.0)
    r0 = c0.ac(fs)
    h_lc = r0["N4"] / r0["BUF_OUT"]           # U1A 出 → LC 出
    h_gain = r0["ADCP"] / r0["GAININ"]        # U1B 的闭环增益（+in→out）
    h_u1a = r0["BUF_OUT"] / r0["BUFIN"]       # U1A 缓冲
    h_in = r0["BUFIN"] / r0["SRC"]            # 源端 → 缓冲入（含端接）
    # 电阻热噪声：在电阻两端并一个 1A 的 AC 电流源，量到 ADCP 的转移阻抗
    items = []
    for (n1, n2, ohm, lb) in c0.R:
        if lb in ("AGND_tie",) or ohm <= 0:
            continue
        c, _ = _mk(src_ac=0.0)
        c.add_I(n1, n2, dc=0.0, label="inoise", ac_mag=1.0)
        rr = c.ac(fs)
        zt = rr["ADCP"]                            # V/A
        dens = math.sqrt(4 * K_B * T0 / ohm)       # A/√Hz
        items.append((lb, ohm, zt * dens))
    # 运放 en（输入电压噪声密度，按各级闭环增益传播）
    items.append(("U1A.en", None, EN_OPA * h_u1a * h_lc * h_gain))
    items.append(("U1B.en", None, EN_OPA * h_gain))
    tot = np.zeros_like(fs, dtype=float)
    detail = {}
    for (lb, ohm, psd) in items:
        p = np.abs(psd) ** 2
        detail[lb] = float(np.sqrt(np.trapezoid(p, fs)))
        tot += p
    vn_rms = float(np.sqrt(np.trapezoid(tot, fs)))
    # 折算到源端（除以净增益 1.995）
    return dict(fs=fs, vn_rms_mv=vn_rms * 1e3,
                vn_src_mv=vn_rms * 1e3 / 1.995,
                detail={k: v * 1e3 for k, v in sorted(detail.items(), key=lambda x: -x[1])},
                quant_mv=3.3 / 4096 / math.sqrt(12) * 1e3,
                h_in=abs(h_in[-1]))


# ============================================================ S6
def s6_psrr(r21_list=(0.0, 4.7, 10.0, 22.0), psrr_list=(60.0, 80.0, 100.0, 126.0)):
    """+5V 轨纹波 → ADC 输入。R21 越大，C11/C14 的滤波越有效。"""
    fs = np.array([1e3, 1e4, 1e5, 2e5, 5e5, 1e6, 2e6])
    out = []
    for p in psrr_list:
        for r21 in r21_list:
            c, _ = _mk(r21=r21, psrr_db=p, rail_ac=1.0, src_ac=0.0)
            r = c.ac(fs)
            h = db(r["ADCP"])          # 纹波 1V → ADC 的传递（dB）
            out.append(dict(psrr=p, r21=r21, fs=fs, h=h,
                            h100k=float(h[2]), h500k=float(h[4])))
    return out


# ============================================================ S7
def s7_scenarios():
    """题面三场景端到端误差。前端是 LTI（S1 已证）⇒ 逐谐波施加复数传递即可精确算，
    不需要 FFT（也就避开 10.5kHz 在 N=4096 下 21.5 个周期的泄漏）。

    同时给出**两列**：
      · 裸误差（只除一个常数净增益）—— 说明"为什么必须有频率标定"
      · 标定后误差（17 点对数表，与固件 op_cal 同口径）—— 这才是判据下的真实余量
    """
    s1 = s1_afe_ac()
    fs = s1["fs"]; H = s1["H"]
    # 标定表：17 点 log 均匀铺 10k~500k（固件口径）——**同时含幅频与相频**
    # （固件 op_peak.c：`op_cal_lookup(cal, f_c, &g, &p_corr)` 之后 `amp /= g; phi -= p_corr;`）
    fc = np.logspace(4, math.log10(5e5), 17)
    hc = np.interp(fc, fs, H)
    hc_mag = np.abs(hc)
    hc_ph = np.unwrap(np.angle(hc))
    out = []
    scenarios = [
        ("u_a（10k~200k）", [10.5e3, 31.5e3, 42.0e3], [1.0, 0.5, 0.25], 100e-3),
        ("u_b（10k~500k）", [10.5e3, 105e3, 420e3], [1.0, 0.5, 0.25], 250e-3),
        ("u_b 最坏（含 500k）", [500e3, 100e3, 10e3], [1.0, 0.5, 0.25], 50e-3),
    ]
    G = 1.995

    def interp(f):
        return np.interp(f, fs, H)

    def cal_gain(f):
        return np.interp(f, fc, hc_mag)

    def cal_ph(f):
        return np.interp(f, fc, hc_ph)

    for name, freqs, rel, upp_pp in scenarios:
        t = np.arange(20000) / FS
        x0 = np.zeros_like(t)
        for f, a in zip(freqs, rel):
            x0 += a * np.sin(2 * math.pi * f * t + 0.3 * f / 1e3)
        scale = upp_pp / (x0.max() - x0.min())
        ideal_upp = (x0.max() - x0.min()) * scale * 1e3
        ideal_rms = float(np.sqrt(np.mean((x0 - x0.mean()) ** 2))) * scale * 1e3
        res = dict(name=name, upp_pp_mv=upp_pp * 1e3, ideal_upp=ideal_upp, ideal_rms=ideal_rms)
        for tag, cal in (("raw", False), ("cal", True)):
            y = np.zeros_like(t)
            for f, a in zip(freqs, rel):
                h = interp(f)
                if cal:
                    g = cal_gain(f)
                    ph = np.angle(h) - cal_ph(f)
                else:
                    g = G
                    ph = np.angle(h)
                y += scale * a * abs(h) / g * np.sin(
                    2 * math.pi * f * t + 0.3 * f / 1e3 + ph)
            code = np.clip(np.round((y + 1.65) / 3.3 * 4096), 0, 4095)
            yq = code / 4096 * 3.3 - 1.65
            res[f"upp_{tag}"] = (yq.max() - yq.min()) * 1e3
            res[f"rms_{tag}"] = float(np.sqrt(np.mean((yq - yq.mean()) ** 2))) * 1e3
            res[f"err_upp_{tag}"] = res[f"upp_{tag}"] - ideal_upp
            res[f"err_rms_{tag}"] = res[f"rms_{tag}"] - ideal_rms
        out.append(res)
    return out


# ============================================================ 入口
def main(which="all"):
    t0 = time.time()
    res = {}
    print("=" * 78)
    print("S1 · AFE 端到端 AC（含运放非理想性）")
    print("=" * 78)
    s1 = s1_afe_ac()
    res["s1"] = {k: v for k, v in s1.items() if k not in ("fs", "H")}
    print(f"  通带基准(1kHz)        : {s1['passband_db']:.3f} dB   （理想 2.0 → +6.02 dB）")
    print(f"  droop @100kHz         : {s1['droop_100k']:+.3f} dB")
    print(f"  droop @500kHz         : {s1['droop_500k']:+.3f} dB   （纯 GBW 极限 -0.643 dB）")
    print(f"  折叠带[1.5,2.5]M 最差 : {s1['fold_worst_db']:.2f} dB   余量 {s1['fold_margin_db']:.1f} dB")
    print(f"  17 点标定插值残差     : {s1['cal_resid_db']:.4f} dB → 幅值 {s1['cal_resid_mv']:.4f} mV")

    print("=" * 78)
    print("S2 · 蒙特卡洛容差（L±10% / C±5%）")
    print("=" * 78)
    s2 = s2_mc(nrun=600)
    res["s2"] = {k: v for k, v in s2.items() if k != "hist"}
    print(f"  n={s2['n']}  折叠带最差衰减：中位 {s2['fold_worst_med']:.2f} dB / 最坏 {s2['fold_worst_min']:.2f} dB")
    print(f"  跌破 -42.5dB 的次数    : {s2['fail']} / {s2['n']}")
    print(f"  通带起伏最大           : {s2['ripple_max']:.3f} dB   @500k droop 最坏 {s2['droop500_max']:+.3f} dB")

    print("=" * 78)
    print("S3 · ADC 采样保持反冲（瞬态）")
    print("=" * 78)
    s3 = s3_adc_kickback()
    res["s3"] = s3
    for d in s3:
        print(f"  R8={d['r8']:>5.0f}Ω C10={d['c10']*1e12:>5.0f}pF  采样误差 {d['err_mv']:+.4f} mV "
              f"({d['err_lsb']:.3f} LSB)")

    print("=" * 78)
    print("S4 · 两音互调 IM3（f_b=400k / f_J=1M）")
    print("=" * 78)
    s4 = s4_im3()
    res["s4"] = s4
    print(f"  U1A 出(BUF_OUT)：u_b={s4['buf']['b']:.2f} mV  u_J={s4['buf']['j']:.2f} mV  "
          f"IM3@{abs(2*400e3-1e6)/1e3:.0f}k={s4['buf']['im3_lo']*1e3:.3f} µV")
    print(f"  ADC 入(ADCP)   ：u_b={s4['adc']['b']:.2f} mV  u_J={s4['adc']['j']:.4f} mV  "
          f"IM3@200k={s4['adc']['im3_lo']*1e3:.4f} µV   IM2@600k={s4['adc']['im2_lo']*1e3:.4f} µV")
    print(f"  → ADC 端 IM3 = {s4['adc']['im3_lo']:.4f} mV，门限 1.5mV ⇒ 余量 "
          f"{1.5/max(s4['adc']['im3_lo'],1e-12):.1f} 倍")

    print("=" * 78)
    print("S5 · 噪声预算（10k~500k）")
    print("=" * 78)
    s5 = s5_noise()
    res["s5"] = s5
    print(f"  ADC 端噪声底 : {s5['vn_rms_mv']:.4f} mVrms   （判据 <0.5 mVrms @TP4）")
    print(f"  折到源端     : {s5['vn_src_mv']:.4f} mVrms")
    print(f"  量化噪声     : {s5['quant_mv']:.4f} mVrms")
    print("  主要贡献（ADC 端 mVrms）:")
    for k, v in list(s5["detail"].items())[:8]:
        print(f"    {k:<14} {v:.5f}")

    print("=" * 78)
    print("S6 · 电源纹波耦合（+5V → ADC）")
    print("=" * 78)
    s6 = s6_psrr()
    res["s6"] = s6
    print(f"  {'PSRR':>6} {'R21':>7} {'@100kHz':>10} {'@500kHz':>10}   1V 纹波在 500k 的贡献")
    for d in s6:
        print(f"  {d['psrr']:>5.0f}dB {d['r21']:>6.1f}Ω {d['h100k']:>9.1f}dB {d['h500k']:>9.1f}dB")

    print("=" * 78)
    print("S7 · 题面三场景端到端误差")
    print("=" * 78)
    s7 = s7_scenarios()
    res["s7"] = s7
    for d in s7:
        print(f"  {d['name']:<20} Upp 误差 {d['err_upp']:+.4f} mV   Urms 误差 {d['err_rms']:+.4f} mV"
              f"   （判据 ±5 mV）")

    print(f"\n总用时 {time.time()-t0:.1f}s")
    return res


if __name__ == "__main__":
    import json
    r = main()
    os.makedirs("tools/sim/out", exist_ok=True)
    def enc(o):
        if isinstance(o, (np.floating, np.integer)):
            return float(o)
        if isinstance(o, np.ndarray):
            return o.tolist()
        raise TypeError(str(type(o)))
    with open("tools/sim/out/studies.json", "w", encoding="utf-8") as f:
        json.dump(r, f, ensure_ascii=False, indent=1, default=enc)
    print("→ tools/sim/out/studies.json")
