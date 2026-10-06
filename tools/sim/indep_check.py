# -*- coding: utf-8 -*-
"""indep_check.py —— 独立验证（只复用 afe_net 的网表 + mna 内核，**不复用 studies.py 的结论口径**）

为什么另写一份：`studies.py` 报的是"设计想让你看的数字"。本文件只回答
**"文档里只有手算、或只有单点结论"**的那几件事，尽量用另一种方法重算：

  A 原型复算     : 8 阶双端接 Butterworth 的元件值**从闭式解独立推导**，与图纸实值逐颗比
  B 元件值择优   : 现状 vs E24 最近值 vs 多频点扫描 —— 是"仿真说该换"还是"仿真说不用换"
  C 买错件       : 按 BOM 的供应商编号下单会买到 69.8k / 10k / 0Ω，四种组合各仿真一遍
  D 干扰源扫频   : 0.9~2.6MHz 扫频 → 折叠到 [9k,502k] 的伪峰**最高值**（文档只测了 1.0M/1.5M 两点）
  E LCD_RST      : 串 33Ω 到底会不会拖慢复位（文档只说"会"，没给数）

跑法：python tools/sim/indep_check.py [A|B|C|D|E|all]
"""
from __future__ import annotations

import json
import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import studies as ST  # noqa: E402
from afe_net import V, build_afe  # noqa: E402

FS = 2.0e6            # ADC 采样率（冻结参数）
BAND = (9e3, 502e3)   # 固件认的峰频带（op_peak 终判）
GATE = 1.5e-3         # 固件峰门限 1.5mVpk
JAM_PP = 200e-3       # 题面 u_J 峰峰值
NET_GAIN_FW = 2.0     # 固件 BOARD_NET_GAIN（未标定时除它）

# 图纸实值（2026-10-03 活体网表 + BOM）—— **活体**：C2/C6 已按实拍择优改过
DRAWN_L = (4.7e-6, 22e-6, 22e-6, 12e-6)
DRAWN_C = (6.2e-9, 10e-9, 9.1e-9, 1.8e-9)      # 活体图纸（C2 5.6→6.2n / C6 10→9.1n）
PREV_C = (5.6e-9, 10e-9, 10e-9, 1.8e-9)        # 优化前（同日之前）—— 对照基线


def cs_of(caps):
    """把绝对电容量换算成 build_afe 要的 c_scale（基准 = 活体图纸）。"""
    return [caps[j] / DRAWN_C[j] for j in range(4)]

E24 = (1.0, 1.1, 1.2, 1.3, 1.5, 1.6, 1.8, 2.0, 2.2, 2.4, 2.7, 3.0,
       3.3, 3.6, 3.9, 4.3, 4.7, 5.1, 5.6, 6.2, 6.8, 7.5, 8.2, 9.1)
OPT_LIVE = dict(r21=4.7, r6=24.9e3, r7=3.57e3)     # 活体图纸（H1+H2 已落图）


def db(x):
    return 20 * np.log10(np.maximum(np.abs(x), 1e-30))


def e24_nearest(x):
    """给定理想值，返回最近的 E24 值（允许任意十倍程）。"""
    best, bd = None, 1e9
    for s in E24:
        for d in range(-14, 4):
            v = s * 10.0 ** d
            if v <= 0:
                continue
            err = abs(math.log(v / x))
            if err < bd:
                bd, best = err, v
    return best


def fold_alias(f, fs=FS):
    """采样折叠：返回折进 [0, fs/2] 的频率；恰在 fs/2（Nyquist bin）的按落 DC 处理
    —— 固件的峰判据只认 [9k,502k]，Nyquist bin 与 DC 都被排除。"""
    x = np.mod(f, fs)
    a = np.where(x > fs / 2, fs - x, x)
    return np.where(np.isclose(a, fs / 2), 0.0, a)


# ============================================================ A 原型复算
def a_prototype():
    """8 阶双端接 Butterworth：g_k = 2·sin((2k-1)π/16)，k=1..8（闭式解，不查表）。
    奇序号 = 串联电感，偶序号 = 并联电容。R0=50Ω 双端接。
    fc 由图纸的 C2 反解（C2 = g2/(ω·R0)）。"""
    g = [2 * math.sin((2 * k - 1) * math.pi / 16) for k in range(1, 9)]
    r0 = 50.0
    # fc 不能随便锚在某一颗上（锚 C2 得 631.6k、锚 L1 得 660.6k）。
    # 用**逐颗反解出的 fc 的几何均值**当"这块板实际的设计 fc"——它是
    # 最小化全体相对偏差的那个 fc，也因此能客观排出"哪一颗最离谱"。
    drawn = [DRAWN_L[0], DRAWN_C[0], DRAWN_L[1], DRAWN_C[1],
             DRAWN_L[2], DRAWN_C[2], DRAWN_L[3], DRAWN_C[3]]
    f_imp = []
    for k, v in enumerate(drawn):                    # k 从 0 数：偶=电感，奇=电容
        f_imp.append(g[k] * r0 / (2 * math.pi * v) if k % 2 == 0
                     else g[k] / (2 * math.pi * v * r0))
    fc = math.exp(sum(math.log(x) for x in f_imp) / 8.0)
    w = 2 * math.pi * fc
    L_ideal = [g[i] * r0 / w for i in (0, 2, 4, 6)]
    C_ideal = [g[i] / (w * r0) for i in (1, 3, 5, 7)]
    rows = []
    for k, (name, ideal, dr) in enumerate(zip(
            ("L1", "C2", "L3", "C4", "L5", "C6", "L7", "C8"),
            [L_ideal[0], C_ideal[0], L_ideal[1], C_ideal[1],
             L_ideal[2], C_ideal[2], L_ideal[3], C_ideal[3]], drawn)):
        e = e24_nearest(ideal)
        rows.append(dict(name=name, ideal=ideal, drawn=dr,
                         dev_pct=(dr / ideal - 1) * 100,
                         implied_fc=f_imp[k],
                         # 该颗相对"设计 fc"的偏离：客观排序用
                         off_fc_pct=(f_imp[k] / fc - 1) * 100,
                         e24=e, e24_dev_pct=(e / ideal - 1) * 100,
                         drawn_is_best_e24=abs(math.log(dr / e)) < 1e-9))
    rows.sort(key=lambda r: -abs(r["off_fc_pct"]))
    return dict(g=g, fc=fc, rows=rows, f_implied=f_imp,
                L_ideal=L_ideal, C_ideal=C_ideal)


# ============================================================ B 元件值择优
def _metrics(l_scale, c_scale, n=200):
    """一套 AC 判据。fold_margin 是硬余量；resid 是 17 点标定后的幅值误差来源。"""
    fs = ST.sweep(1e3, 3e6, n)
    kw = {}
    if l_scale is not None:
        kw["l_scale"] = l_scale
    if c_scale is not None:
        kw["c_scale"] = c_scale
    c, _ = build_afe(src_ac=1.0, **OPT_LIVE, **kw)
    H = db(c.ac(fs)["ADCP"])
    fold = (fs >= 1.5e6) & (fs <= 2.5e6)
    k1 = int(np.argmin(np.abs(fs - 1e3)))
    k5 = int(np.argmin(np.abs(fs - 5e5)))
    fc17 = np.logspace(4, math.log10(5e5), 17)
    hc = np.interp(np.log(fc17), np.log(fs), H)
    fg = np.logspace(4, math.log10(5e5), 2001)
    resid = float(np.max(np.abs(np.interp(np.log(fg), np.log(fs), H)
                                - np.interp(np.log(fg), np.log(fc17), hc))))
    pb = H[(fs >= 1e4) & (fs <= 5e5)]
    rip = []
    for cd, cc in ((2e-12, 2e-12), (16e-12, 8e-12)):
        V["Cin_diff"], V["Cin_cm"] = cd, cc
        cv, _ = build_afe(src_ac=1.0, **OPT_LIVE, **kw)
        hh = db(cv.ac(np.array([1e4, 1e5, 5e5]))["ADCP"])
        rip.append(float(hh.max() - hh.min()))
    V["Cin_diff"], V["Cin_cm"] = 8e-12, 4e-12
    return dict(net_gain=float(abs(10 ** (H[k1] / 20))),
                droop500=float(H[k5] - H[k1]),
                ripple_pp=float(pb.max() - pb.min()),
                fold_worst=float(H[fold].max()),
                fold_margin=float(-H[fold].max() - 42.5),
                resid_db=resid, resid_mv=float((10 ** (resid / 20) - 1) * 250),
                cin_span_db=max(rip) - min(rip))


CAPS_E24_NF = (1.0, 1.1, 1.2, 1.3, 1.5, 1.6, 1.8, 2.0, 2.2, 2.4, 2.7, 3.0,
               3.3, 3.6, 3.9, 4.3, 4.7, 5.1, 5.6, 6.2, 6.8, 7.5, 8.2, 9.1,
               10.0, 11.0, 12.0, 13.0, 15.0)


def b_variants():
    """优化：**只动电容**（电感是难买件，改它要重新选型+买料）。
    先逐颗扫 E24 看响应面，再坐标下降找组合，最后把候选拿去做端到端判据。"""
    base = _metrics(None, cs_of(PREV_C))
    rows = [("优化前（C2=5.6n / C6=10n）", base, None),
            ("活体现状（C2=6.2n / C6=9.1n）", _metrics(None, None), None)]
    # ---- 阶段 1：坐标下降（两轮，从活体现状出发再搜一遍，看还有没有更好的） ----
    cur = {i: DRAWN_C[i] for i in range(4)}
    for _ in range(2):
        for i in range(4):
            best, bv = None, None
            for v in CAPS_E24_NF:
                trial = dict(cur)
                trial[i] = v * 1e-9
                cs = [trial[j] / DRAWN_C[j] for j in range(4)]
                m = _metrics(None, cs)
                # 目标：硬余量留够 10dB 后，最小化标定残差（=幅值误差来源）
                if m["fold_margin"] < 10.0:
                    continue
                score = m["resid_db"]
                if best is None or score < best:
                    best, bv = score, v
            if bv is not None and abs(bv * 1e-9 - cur[i]) > 1e-15:
                cur[i] = bv * 1e-9
    cs = cs_of(list(cur.values()))
    rows.append(("再搜一轮的最优（只动电容）", _metrics(None, cs), list(cur)))
    # ---- 阶段 2：几个有代表性的单点（供人判取舍） ----
    for tag, cs2 in (
            ("若只把 C6 换回原型值 8.2nF", cs_of([DRAWN_C[0], DRAWN_C[1], 8.2e-9, DRAWN_C[3]])),
            ("再把 C8 也换 2.0nF", cs_of([DRAWN_C[0], DRAWN_C[1], 8.2e-9, 2.0e-9])),
            ("全部电容 ×0.95（等效 fc 上移）", [0.95] * 4),
            ("全部电容 ×1.05（等效 fc 下移）", [1.05] * 4)):
        rows.append((tag, _metrics(None, cs2), None))
    return rows, cur


def b_robust(caps, nrun=150, seed=20261003, ref=None,
             tag_ref="活体图纸", tag_caps="对照"):
    """关键一问：搜索出来的电容组是**真优势**还是**拟合到了那 17 个点上**？
    给 L±10% / C±5% 的容差做蒙特卡洛，比"标定残差"的分布。
    固件 op_cal_lookup 就是 **log-log 线性插值**（op_cal.c:47 `expf(logf(g_k)+u·Δ)`），
    所以这个残差口径与固件一字不差，不是自造指标。"""
    rng = np.random.default_rng(seed)
    fs = ST.sweep(1e3, 3e6, 160)
    fc17 = np.logspace(4, math.log10(5e5), 17)
    fg = np.logspace(4, math.log10(5e5), 1200)
    out = {}
    ref = list(DRAWN_C if ref is None else ref)
    for tag, base in ((tag_ref, ref), (tag_caps, list(caps))):
        res = []
        for _ in range(nrun):
            cs = [base[j] * (1 + rng.uniform(-0.05, 0.05)) / DRAWN_C[j] for j in range(4)]
            ls = [1 + rng.uniform(-0.10, 0.10) for _ in range(4)]
            c, _ = build_afe(src_ac=1.0, **OPT_LIVE, l_scale=ls, c_scale=cs)
            H = db(c.ac(fs)["ADCP"])
            hc = np.interp(np.log(fc17), np.log(fs), H)
            res.append(float(np.max(np.abs(np.interp(np.log(fg), np.log(fs), H)
                                           - np.interp(np.log(fg), np.log(fc17), hc)))))
        res = np.array(res)
        mv = lambda x: float((10 ** (x / 20) - 1) * 250)  # noqa: E731
        out[tag] = dict(median_db=float(np.median(res)), p90_db=float(np.percentile(res, 90)),
                        worst_db=float(res.max()),
                        median_mv=mv(np.median(res)), p90_mv=mv(np.percentile(res, 90)),
                        worst_mv=mv(res.max()))
    return out


def b_worst_tone(caps, nrun=8, seed=424242):
    """把"标定残差"这个 dB 指标拿去做**直接测量**（不能只信自己的指标）：
    先把响应扫出来找准"最坏频点" f*（17 点 log-log 插值偏离真值最大的地方），
    再在 f* 注入单音，走固件同口径（17 点标定 + 12bit 量化 + 频域 sin 重建），
    读 Upp 误差。标称值一组 + 带容差的若干组（L±10% / C±5%）。"""
    rng = np.random.default_rng(seed)
    fs = ST.sweep(1e3, 3e6, 400)
    fc17 = np.logspace(4, math.log10(5e5), 17)
    fg = np.logspace(4, math.log10(5e5), 1500)
    res = []
    for k in range(nrun + 1):
        if k == 0:
            cs = [caps[j] / DRAWN_C[j] for j in range(4)]
            ls = [1.0] * 4
            tag = "标称"
        else:
            cs = [caps[j] * (1 + rng.uniform(-0.05, 0.05)) / DRAWN_C[j] for j in range(4)]
            ls = [1 + rng.uniform(-0.10, 0.10) for _ in range(4)]
            tag = f"容差#{k}"
        c, _ = build_afe(src_ac=1.0, **OPT_LIVE, l_scale=ls, c_scale=cs)
        H = c.ac(fg)["ADCP"]
        Hb = db(H)
        hc = np.interp(np.log(fc17), np.log(fg), Hb)
        fit = np.interp(np.log(fg), np.log(fc17), hc)
        i = int(np.argmax(np.abs(Hb - fit)))
        fstar = float(fg[i])
        # --- 直接测量：f* 单音，源端 Upp=250mVpp，走固件口径 ---
        hc_c = np.interp(fc17, fg, H)
        g = abs(np.interp(fstar, fc17, np.abs(hc_c)))
        ph_cal = np.interp(fstar, fc17, np.unwrap(np.angle(hc_c)))
        h = complex(np.interp(fstar, fg, H.real), np.interp(fstar, fg, H.imag))
        t = np.arange(20000) / FS
        x0 = np.sin(2 * math.pi * fstar * t)
        scale = 250e-3 / (x0.max() - x0.min())
        y = scale * abs(h) / g * np.sin(2 * math.pi * fstar * t + (np.angle(h) - ph_cal))
        code = np.clip(np.round((y + 1.65) / 3.3 * 4096), 0, 4095)
        yq = code / 4096 * 3.3 - 1.65
        res.append(dict(tag=tag, fstar=fstar, resid_db=float(Hb[i] - fit[i]),
                        err_mv=float((yq.max() - yq.min()) * 1e3 - 250.0)))
    return res


def b_marginal(cand=None, nrun=100, seed=99):
    """最小改动分析：{C2, C6, C8} 三处改动的 2³ 种组合（起点 = 优化前），
    每种跑一次容差 MC，看收益到底由哪一颗承担 —— 别为了 1% 的收益去动三颗料。"""
    cand = DRAWN_C if cand is None else cand
    rng = np.random.default_rng(seed)
    fs = ST.sweep(1e3, 3e6, 140)
    fc17 = np.logspace(4, math.log10(5e5), 17)
    fg = np.logspace(4, math.log10(5e5), 1000)
    idx = (0, 2, 3)                       # C2 / C6 / C8 的位次
    out = []
    for mask in range(8):
        base = list(PREV_C)
        for b, j in enumerate(idx):
            if mask >> b & 1:
                base[j] = cand[j]
        res = []
        for _ in range(nrun):
            cs = [base[j] * (1 + rng.uniform(-0.05, 0.05)) / DRAWN_C[j] for j in range(4)]
            ls = [1 + rng.uniform(-0.10, 0.10) for _ in range(4)]
            c, _ = build_afe(src_ac=1.0, **OPT_LIVE, l_scale=ls, c_scale=cs)
            H = db(c.ac(fs)["ADCP"])
            hc = np.interp(np.log(fc17), np.log(fs), H)
            res.append(float(np.max(np.abs(np.interp(np.log(fg), np.log(fs), H)
                                           - np.interp(np.log(fg), np.log(fc17), hc)))))
        res = np.array(res)
        mv = lambda x: float((10 ** (x / 20) - 1) * 250)  # noqa: E731
        names = "+".join(n for b, n in enumerate(("C2", "C6", "C8")) if mask >> b & 1) or "都不改"
        out.append(dict(changed=names, median_mv=mv(np.median(res)),
                        p90_mv=mv(np.percentile(res, 90)), worst_mv=mv(res.max())))
    return out


def b_endtoend(caps):
    """把候选取值送进端到端题面复现（判据上的真实误差 = 最终验收量）。
    口径：固件 17 点标定表是**在本板上实测建的**，所以它测到的就是实际 |H| ——
    标定列能吸收静态增益偏差，吸收不了的是"标定快照之外"的东西。"""
    cs = cs_of(caps)
    out = []
    for tag, kw in (("活体(优化后)", dict(c_scale=[1.0] * 4)),
                    ("优化前", dict(c_scale=cs))):
        ST.set_config(r6=24.9e3, r7=3.57e3, r21=4.7, **kw)
        for d in ST.s7_scenarios():
            out.append(dict(tag=tag, name=d["name"],
                            err_raw=d["err_upp_raw"], err_cal=d["err_upp_cal"],
                            err_rms_cal=d["err_rms_cal"]))
    ST.set_config(c_scale=[1.0] * 4)
    return out


# ============================================================ C 买错件
def c_buy():
    """按供应商编号下单会买到的四种阻力组合。r21 保持活体的 4.7Ω。"""
    cases = [
        ("OK  24.9k / 3.57k（图纸值）", 24.9e3, 3.57e3),
        ("ERR 69.8k / 3.57k（R6 买成 C706249）", 69.8e3, 3.57e3),
        ("ERR 24.9k / 10k  （R7 买成 C2912582）", 24.9e3, 10e3),
        ("ERR 69.8k / 10k  （两颗都买错）", 69.8e3, 10e3),
    ]
    out = []
    for tag, r6, r7 in cases:
        ST.set_config(r6=r6, r7=r7, r21=4.7)
        s1 = ST.s1_afe_ac()
        fs, H = s1["fs"], s1["H"]
        net = float(abs(H[0]))
        k5 = int(np.argmin(np.abs(fs - 5e5)))
        pb = db(H)[(fs >= 1e4) & (fs <= 5e5)]
        rip = []
        for cd, cc in ((2e-12, 2e-12), (16e-12, 8e-12)):
            V["Cin_diff"], V["Cin_cm"] = cd, cc
            cv, _ = build_afe(src_ac=1.0, **OPT_LIVE)
            hh = db(cv.ac(np.array([1e4, 1e5, 5e5]))["ADCP"])
            rip.append(float(hh.max() - hh.min()))
        V["Cin_diff"], V["Cin_cm"] = 8e-12, 4e-12
        s5 = ST.s5_noise()
        s7 = ST.s7_scenarios()
        worst = min(s7, key=lambda d: d["err_upp_cal"])
        out.append(dict(
            tag=tag, r6=r6, r7=r7,
            gain=1 + r6 / r7, net_gain=net,
            gain_vs_ok=net / 1.995, droop500=float(db(H[k5]) - db(H[0])),
            ripple=float(pb.max() - pb.min()), cin_span=max(rip) - min(rip),
            headroom_pk_mv=float(1.60 / net * 1e3),
            noise_mv=s5["vn_rms_mv"],
            raw_upp_err={d["name"]: d["err_upp_raw"] for d in s7},
            cal_upp_err={d["name"]: d["err_upp_cal"] for d in s7},
            worst_cal_err=worst["err_upp_cal"]))
    ST.set_config(**OPT_LIVE)
    return out


# ============================================================ D 干扰源扫频
def d_jammer():
    """u_J（200mVpp）从 0.9MHz 扫到 2.6MHz，看折叠到 [9k,502k] 的伪峰能到多大。
    文档只算了 1.0MHz 与 1.5MHz 两个点，这里**整个区间扫**。"""
    c, _ = build_afe(src_ac=1.0, **OPT_LIVE)
    f = np.linspace(0.90e6, 2.60e6, 8501)          # 200Hz 步长
    H = np.abs(c.ac(f)["ADCP"])
    alias = fold_alias(f)
    inb = (alias >= BAND[0]) & (alias <= BAND[1])
    res = {}
    for amp_pk, tag in ((JAM_PP / 2, "峰值=100mV(200mVpp 的正解)"),
                        (JAM_PP, "峰值=200mV(按题面不打折的保守口径)")):
        a = amp_pk * H
        viol = inb & (a >= GATE)
        i_w = int(np.argmax(np.where(inb, a, 0)))
        res[tag] = dict(worst_mv=float(a[i_w] * 1e3), worst_f=float(f[i_w]),
                        worst_alias=float(alias[i_w]),
                        margin=float(GATE / max(a[i_w], 1e-12)),
                        n_viol=int(viol.sum()),
                        viol_span=(float(f[viol].min()), float(f[viol].max())) if viol.any() else None)
        if not viol.any():
            i_next = int(np.argmax(a))          # 全带最大（含折到带外的）
            res[tag]["allband_peak_mv"] = float(a[i_next] * 1e3)
            res[tag]["allband_f"] = float(f[i_next])
    # 带内映射的最低干扰频率（手算说 1.5MHz，实际由 fw 带边 502k 决定）
    f_lo = float(f[inb].min())
    res["_edge"] = dict(f_lowest_inband_map=f_lo,
                        h_at_edge_db=float(db(c.ac(np.array([f_lo]))["ADCP"])[0]),
                        h_at_1p5M_db=float(db(c.ac(np.array([1.5e6]))["ADCP"])[0]))
    # 几个点名频点
    pts = [1.0e6, 1.1e6, 1.4e6, 1.49e6, f_lo, 1.5e6, 1.6e6, 1.99e6, 2.0e6, 2.01e6, 2.5e6]
    res["_points"] = [dict(f=float(x), alias=float(fold_alias(np.array([x]))[0]),
                           h_db=float(db(c.ac(np.array([x]))["ADCP"])[0]),
                           amp_mv=float(0.1 * abs(c.ac(np.array([x]))["ADCP"][0]) * 1e3),
                           in_band=bool(BAND[0] <= fold_alias(np.array([x]))[0] <= BAND[1]))
                      for x in pts]
    return res


# ============================================================ E LCD_RST 边沿
def e_rst():
    """适配板上 LCD_RST 挂 18k 上拉 + 100nF 到地（hardware/13 §5.4③ 记 τ=1.8ms），
    MCU 侧 PB1（输出阻抗按 30Ω 估）经 R42=33Ω 驱动。判据：**复位低电平脉冲 ≥5ms**。

    问题：33Ω 会不会把复位沿拖慢到出问题？—— `22 §2` 说"加了反而拖慢复位沿"因此不加，
    但 `26 §N3`/`27 §H3` 说要加，活体 BOM 里 R42 已经在了。文档两边都没给数，这里给数。"""
    out = []
    for r42, tag in ((0.0, "无串阻（等效直连）"), (33.0, "R42=33Ω（活体）"),
                     (100.0, "R42=100Ω（夸张对照）")):
        c = ST.Circuit("rst")
        c.add_V("V3V3", "0", dc=3.3, label="V3V3")
        c.add_R("V3V3", "RST", 18e3, "Rup18k")       # 适配板 18k 上拉
        c.add_C("RST", "0", 100e-9, "C100n")         # 适配板 100nF
        c.add_R("DRV", "RST", max(r42, 1e-3), "R42")
        c.add_V("VPIN", "0", dc=3.3, label="VPIN")   # MCU 引脚，空闲高
        c.add_R("VPIN", "DRV", 30.0, "Rpin")         # MCU 输出阻抗
        for i, (np_, nn_, dc, lb, am, ap) in enumerate(c.V):
            if lb == "VPIN":
                c.VWAVE[i] = lambda t: 0.0 if t <= 5e-3 else 3.3
        ts, vs = c.tran(12e-3, 0.25e-6, method="trap",
                        ic_vnode={"RST": 3.3, "DRV": 3.3})
        v = vs["RST"]
        t10 = float(ts[np.argmax(v <= 0.33)]) * 1e3 if v.min() <= 0.33 else float("nan")
        k = np.argmax((ts > 5e-3) & (v >= 0.9 * 3.3))
        t90 = (float(ts[k]) - 5e-3) * 1e3 if v.max() >= 0.9 * 3.3 else float("nan")
        out.append(dict(tag=tag, r42=r42,
                        dv_low_mv=float(v.min() * 1e3),
                        t_fall_10_ms=t10, t_rise_90_ms=t90,
                        slack_ms=5.0, note="判据 hardware/13 §5.4③：低电平脉冲 ≥5ms"))
    return out


# ============================================================ 入口
# ============================================================ F 串阻贴哪一侧
def f_damping(z0=120.0, r_out=30.0, cl=10e-12, nsec=8, dt=2e-12, tstop=60e-9, tr=5e-9):
    """8080 一根线的边沿振铃：33Ω 到底该贴 MCU 侧还是连接器侧？
    文档 `28 §5.2` 自己标注过它的数字"是上界估计、不是实测"；`18 §R-21` 说图纸画在 J5 侧
    是 EDA 工具限制、PCB 必须贴 MCU 侧。这里用**分布 LC 阶梯 + 两端两种贴法**直接量过冲。

    线模型：Z0=120Ω（2 层板、无参考平面，扁平排线/飞线的量级），N 段 LC，
    每段 L=L_tot/N、C=L_tot/N/Z0²；末端 CMOS 输入按 10pF 电容 + 高阻。
    驱动：MCU 推挽，输出阻抗 30Ω，边沿用 3.3V 阶跃（阶跃是最坏情况）。"""
    cases = []
    for l_tot, tag in ((60e-9, "成品形态（直插转接板，L≈60nH）"),
                       (215e-9, "台面形态（100mm 杜邦，L≈215nH）")):
        for r_d, place in ((0.0, "不串"), (33.0, "MCU 侧"), (33.0, "连接器侧")):
            c = ST.Circuit("si")
            c.add_V("VDRV", "0", dc=0.0, label="VDRV")
            for i, (np_, nn_, dc, lb, am, ap) in enumerate(c.V):
                if lb == "VDRV":
                    # 有限边沿（斜坡），不是理想阶跃 —— 阶跃会把过冲算得偏高
                    c.VWAVE[i] = lambda t: 3.3 * min(max(t / tr, 0.0), 1.0)
            ls, cs = l_tot / nsec, l_tot / nsec / (z0 ** 2)
            if place == "MCU 侧" and r_d > 0:
                c.add_R("VDRV", "N1", r_d, "Rd")
                c.add_R("N1", "N1d", r_out, "Rout")
                c.add_L("N1d", "N2", ls, "L1")
                first = "N2"
            else:
                c.add_R("VDRV", "N1", r_out, "Rout")
                c.add_L("N1", "N2", ls, "L1")
                first = "N2"
            prev = first
            for k in range(1, nsec):
                nxt = f"NL{k + 1}"
                c.add_L(prev, nxt, ls, f"L{k + 1}")
                c.add_C(prev, "0", cs, f"C{k}")
                prev = nxt
            if place == "连接器侧" and r_d > 0:
                c.add_R(prev, "LOAD", r_d, "Rd")
            else:
                c.add_R(prev, "LOAD", 1e-3, "tie")
            c.add_C("LOAD", "0", cl, "CL")
            ts, vs = c.tran(tstop, dt, method="trap")
            v = vs["LOAD"]
            vmax = float(v.max())
            # 边沿时间（10%~90% 首次穿越）
            k10 = np.argmax(v >= 0.33)
            k90 = np.argmax(v >= 2.97)
            cases.append(dict(tag=tag, l_nh=l_tot * 1e9, place=place, r_d=r_d,
                              z0=z0, vmax=vmax, overshoot_pct=(vmax - 3.3) / 3.3 * 100,
                              edge_ns=float((ts[k90] - ts[k10]) * 1e9)))
    return cases


def main(which="all"):
    out = {}
    if which in ("all", "A"):
        a = a_prototype()
        print("=" * 104)
        print(f"A · 原型复算（独立闭式解 g_k=2sin((2k-1)π/16)）：8 阶双端接 Butterworth，R0=50Ω，"
              f"设计 fc={a['fc']/1e3:.1f} kHz（= 逐颗反解 fc 的几何均值）")
        print("=" * 104)
        print(f"  {'件':<4}{'理想值':>11}{'图纸实值':>11}{'偏差':>9}{'反解fc':>10}{'偏离设计fc':>11}"
              f"{'E24最近':>11}{'换后偏差':>10}   判")
        for r in a["rows"]:
            isL = r["name"].startswith("L")
            unit, su = (1e6, "µH") if isL else (1e9, "nF")
            flag = "✅ 已是 E24 最近" if r["drawn_is_best_e24"] else "⚠️ 有更近的 E24 值"
            print(f"  {r['name']:<4}{r['ideal']*unit:>9.2f}{su}{r['drawn']*unit:>9.2f}{su}"
                  f"{r['dev_pct']:>+8.1f}%{r['implied_fc']/1e3:>9.1f}k{r['off_fc_pct']:>+10.1f}%"
                  f"{r['e24']*unit:>9.2f}{su}{r['e24_dev_pct']:>+9.1f}%   {flag}")
        print("  （按『偏离设计 fc』降序 —— 排第一的就是把通带拽歪最多的那一颗）")
        out["A"] = a

    if which in ("all", "B"):
        rows, cur = b_variants()
        print()
        print("=" * 104)
        print("B · 电容取值择优（电感不动 —— 难买件；目标：折叠余量 ≥10dB 下最小化标定残差）")
        print("=" * 104)
        print(f"  {'方案':<34}{'净增益':>8}{'@500k':>9}{'通带峰谷':>10}{'折叠最差':>10}{'余量':>8}"
              f"{'标定残差':>11}{'→幅值':>9}{'Cin跨度':>9}")
        for tag, m, caps in rows:
            print(f"  {tag:<34}{m['net_gain']:>8.4f}{m['droop500']:>+8.3f}dB{m['ripple_pp']:>9.3f}dB"
                  f"{m['fold_worst']:>9.2f}dB{m['fold_margin']:>7.1f}dB{m['resid_db']:>10.4f}dB"
                  f"{m['resid_mv']:>8.3f}m{m['cin_span_db']:>8.3f}dB")
        print("  搜索出的最优电容组： " +
              " ".join(f"{n}={v*1e9:.1f}nF" for n, v in zip(("C2", "C4", "C6", "C8"), cur.values())))
        rb = b_robust(PREV_C, ref=DRAWN_C,
                      tag_ref="活体（优化后）", tag_caps="优化前")
        print()
        print("  容差鲁棒性（L±10% / C±5%，各 150 次跑标定残差）—— 真优势还是拟合到点上：")
        for tag, d in rb.items():
            print(f"    {tag}  中位 {d['median_mv']:.3f} mV · 90 分位 {d['p90_mv']:.3f} mV · "
                  f"最坏 {d['worst_mv']:.3f} mV")
        wt = []
        for wtag, wcaps in (("活体(优化后)", list(DRAWN_C)), ("优化前", list(PREV_C))):
            for d in b_worst_tone(wcaps):
                wt.append(dict(wtag=wtag, **d))
        print()
        print("  把该指标拿去做直接测量（最坏频点注入单音 → 17 点标定 → 12bit → sin 重建）：")
        for d in wt:
            print(f"    {d['wtag']} {d['tag']:<8} f*={d['fstar']/1e3:>7.2f}kHz  "
                  f"残差 {d['resid_db']:+.4f}dB  Upp 误差 {d['err_mv']:+7.3f} mV   （判据 ±5mV）")
        e2e = b_endtoend(PREV_C)
        mg = b_marginal()
        print()
        print("  最小改动分析（{C2,C6,C8} 的 2³ 组合，各 100 次容差 MC 的标定残差）：")
        for d in sorted(mg, key=lambda x: x["median_mv"]):
            print(f"    改 {d['changed']:<12} 中位 {d['median_mv']:.3f} mV · "
                  f"90 分位 {d['p90_mv']:.3f} mV · 最坏 {d['worst_mv']:.3f} mV")
        print()
        print("  端到端题面复现（判据 ±5mV）——标定列能不能把差异吸收掉：")
        for d in e2e:
            print(f"    {d['tag']}  {d['name']:<20} 裸 {d['err_raw']:+8.3f} mV   "
                  f"标定后 Upp {d['err_cal']:+7.3f} mV  Urms {d['err_rms_cal']:+7.3f} mV")
        out["B"] = dict(rows=[dict(tag=t, caps=c, **m) for t, m, c in rows],
                        best_caps=list(cur.values()), e2e=e2e)

    if which in ("all", "C"):
        cb = c_buy()
        print()
        print("=" * 96)
        print("C · 买错件（按 BOM 供应商编号下单的实际后果）")
        print("=" * 96)
        print(f"  {'情形':<38}{'净增益':>8}{'vs图纸':>8}{'@500k':>9}{'噪声':>9}{'余量pk':>9}")
        for d in cb:
            print(f"  {d['tag']:<38}{d['net_gain']:>8.4f}{d['gain_vs_ok']:>7.2f}x"
                  f"{d['droop500']:>+8.3f}dB{d['noise_mv']:>8.4f}{d['headroom_pk_mv']:>8.0f}m")
            for k in d["raw_upp_err"]:
                print(f"      {k:<22} 裸 Upp 误差 {d['raw_upp_err'][k]:+9.3f} mV   "
                      f"标定后 {d['cal_upp_err'][k]:+8.3f} mV   （判据 ±5mV）")
        out["C"] = cb

    if which in ("all", "D"):
        dj = d_jammer()
        print()
        print("=" * 96)
        print("D · 干扰源扫频 0.90~2.60 MHz（200mVpp）→ 折进 [9k,502k] 的伪峰")
        print("=" * 96)
        for tag, r in dj.items():
            if tag.startswith("_"):
                continue
            print(f"  {tag}")
            print(f"    带内伪峰最高 {r['worst_mv']:.4f} mVpk @ u_J={r['worst_f']/1e6:.4f}MHz"
                  f"（折到 {r['worst_alias']/1e3:.1f}kHz）  门限 1.5mV ⇒ 余量 {r['margin']:.1f}×")
            print(f"    违规频点数 {r['n_viol']} / 8501" +
                  (f"   区间 {r['viol_span'][0]/1e6:.4f}~{r['viol_span'][1]/1e6:.4f}MHz"
                   if r["viol_span"] else "   （无一违规）"))
            if "allband_peak_mv" in r:
                print(f"    参考：不折叠时全带最大 {r['allband_peak_mv']:.3f} mVpk "
                      f"@ {r['allband_f']/1e6:.4f}MHz（落在带外，固件不认）")
        e = dj["_edge"]
        print(f"  带内映射的最低干扰频率 = {e['f_lowest_inband_map']/1e6:.4f} MHz "
              f"（|H|={e['h_at_edge_db']:.2f}dB）；1.5MHz 处 |H|={e['h_at_1p5M_db']:.2f}dB"
              f" ⇒ 文档用 1.5MHz 作判据偏保守 {e['h_at_1p5M_db']-e['h_at_edge_db']:.2f} dB")
        print("  点名频点：")
        for p in dj["_points"]:
            print(f"    {p['f']/1e6:>6.3f}MHz  折到 {p['alias']/1e3:>7.1f}kHz  "
                  f"|H|={p['h_db']:>7.2f}dB  ADC 端 {p['amp_mv']:>7.4f}mVpk  "
                  f"{'带内' if p['in_band'] else '带外'}")
        out["D"] = dj

    if which in ("all", "F"):
        fc = f_damping()
        print()
        print("=" * 104)
        print("F · 8080 单线边沿振铃：33Ω 串在 MCU 侧 vs 连接器侧（分布 LC 阶梯，Z0=120Ω，")
        print("    驱动 30Ω，末端 10pF；5ns 斜坡边沿）")
        print("=" * 104)
        print(f"  {'形态':<32}{'串在哪':<12}{'峰值':>9}{'过冲':>9}{'边沿(10-90%)':>14}")
        for d in fc:
            print(f"  {d['tag']:<32}{d['place']:<12}{d['vmax']:>8.3f}V"
                  f"{d['overshoot_pct']:>8.1f}%{d['edge_ns']:>12.1f}ns")
        out["F"] = fc

    if which in ("all", "E"):
        er = e_rst()
        print()
        print("E · LCD_RST：串 33Ω 对复位沿的实际影响（适配板 18k↑ + 100nF↓，PB1 拉低 5ms 后释放）")
        print("=" * 96)
        for d in er:
            print(f"  {d['tag']:<20} 低电平 {d['dv_low_mv']:>7.2f} mV   "
                  f"下降到 10% 用 {d['t_fall_10_ms']:>7.4f} ms   "
                  f"释放到 90% 用 {d['t_rise_90_ms']:>7.4f} ms")
        f0 = next(d for d in er if d["r42"] == 0.0)
        f33 = next(d for d in er if d["r42"] == 33.0)
        print(f"  ⇒ 33Ω 让下降沿多花 {f33['t_fall_10_ms']-f0['t_fall_10_ms']:+.4f} ms，"
              f"占 5ms 判据的 {(f33['t_fall_10_ms']-f0['t_fall_10_ms'])/5.0*100:+.3f}%")
        out["E"] = er

    if which == "R":
        print("=" * 104)
        print("R · 大样本容差 MC（n=400，L±10% / C±5%）—— 优化前 vs 活体图纸")
        print("=" * 104)
        for tag, caps in (("优化前（C2=5.6n C6=10n）", PREV_C),
                          ("活体  （C2=6.2n C6=9.1n）", DRAWN_C)):
            d = b_robust(caps, nrun=400, ref=caps, tag_ref=tag, tag_caps=tag)[tag]
            print(f"  {tag:<26} 中位 {d['median_mv']:.3f} mV · 90% {d['p90_mv']:.3f} mV · "
                  f"最坏 {d['worst_mv']:.3f} mV   （判据 5mV）")
            out.setdefault("R", {})[tag] = d

    if which == "S":
        ST.set_config(**ST.OPT_AFTER)
        d = ST.s2_mc(nrun=300)
        print("=" * 104)
        print(f"S · 折叠带容差 MC（活体图纸，L±10% / C±5%，n={d['n']}）")
        print("=" * 104)
        print(f"  折叠带最差衰减：中位 {d['fold_worst_med']:.2f} dB / 最坏 {d['fold_worst_min']:.2f} dB"
              f"   跌破 −42.5dB：{d['fail']} / {d['n']}")
        print(f"  通带起伏最大 {d['ripple_max']:.3f} dB   @500k droop 最坏 {d['droop500_max']:+.3f} dB")
        out["S"] = {k: v for k, v in d.items() if k != 'hist'}

    os.makedirs("tools/sim/out", exist_ok=True)
    with open("tools/sim/out/indep_check.json", "w", encoding="utf-8") as f:
        json.dump(out, f, ensure_ascii=False, indent=1, default=float)
    print("\n→ tools/sim/out/indep_check.json")
    return out


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "all")
