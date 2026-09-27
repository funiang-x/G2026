# -*- coding: utf-8 -*-
"""G2026 G题 DSP 方案数值复算
四段：① 参数自检 ② fs 穷举六列表 ③ 滤波器对比 ④ 端到端误差仿真
报告写 utf-8 到 tools/report_dsp_verify.md
"""
import numpy as np

OUT = r"C:\Users\funiang\Desktop\Project\products\G2026\tools\report_dsp_verify.md"
L = []
def P(s=""):
    L.append(s)

# ---------- 公共参数（定稿候选） ----------
N = 4096
SYSCLK = 144e6          # board_config: 144 MHz
N_FFT = N
CRIT_AMP = 5.0          # mV 幅值判据
CRIT_F = 1000.0         # Hz 基频判据
F_MIN, F_MAX = 10e3, 500e3
UJ_PEAK = 100.0         # mV，u_J 200mVpp 的峰值
ADC_FS_MV = 3300.0
ADC_BITS = 12
G_FRONT = 4.0           # 前端增益
LSB_IN = ADC_FS_MV / (2**ADC_BITS) / G_FRONT  # 输入端等效 LSB (mV)

# BH4 (Blackman-Harris 4-term)
BH4 = np.array([0.35875, 0.48829, 0.14128, 0.011568])
def bh4(n):
    t = 2*np.pi*np.arange(n)/n
    return BH4[0] - BH4[1]*np.cos(t) + BH4[2]*np.cos(2*t) - BH4[3]*np.cos(3*t)
W4 = bh4(N_FFT)
CG = W4.mean()          # 相干增益

# 巴特沃斯低通（全极点，幅相都准确）
def bw_poles(n):
    # 上半平面极点（归一化 ωc=1）：s = -sin(φ) + j cos(φ), φ=(2k-1)π/2n
    ks = np.arange(1, n+1)
    phi = (2*ks - 1) * np.pi / (2*n)
    return -np.sin(phi) + 1j*np.cos(phi)

def bw_response(f, n, fc):
    """返回 |H(f)| 与 ∠H(f)（弧度），全极点精确式（s 归一化频率 = f/fc，无 2π）"""
    p = bw_poles(n)
    w = np.asarray(f, dtype=float) / fc
    H = np.ones_like(w, dtype=complex)
    for pk in p:
        H /= (1j*w - pk)
    return np.abs(H), np.angle(H)

def bw_Qmax(n):
    p = bw_poles(n)
    return np.max(np.abs(p) / (-2*np.real(p)))

def aperture_loss(f, smp_cyc):
    """ADC 采样保持孔径衰减 sinc(πf·ts)"""
    ts = smp_cyc / 36e6
    x = np.pi*np.asarray(f, dtype=float)*ts
    return np.sinc(x/np.pi)  # np.sinc(x)=sin(πx)/(πx)

P("# G2026 DSP 方案数值复算报告")
P(f"- N={N_FFT} · BH4 窗 · 相干增益 CG={CG:.6f}")
P(f"- ADCCLK=36MHz · 增益×{G_FRONT} · 输入端 LSB={LSB_IN:.4f} mV")
P(f"- 判据：幅值/Upp/Urms ≤{CRIT_AMP} mV，基频 ≤{CRIT_F:.0f} Hz")
P()

# ---------- ① 参数自检 ----------
P("## ① 参数自检")
P(f"- BH4 主瓣（−6dB）半宽约 2.2 bin；峰间最小间隔 = 10kHz/488Hz ≈ 20 bin，互不侵占")
# 单音 δ 扫描验证读数口径（无标定、无滤波器、理想 ADC）
def read_tone(x, fs, f_lo=9e3, f_hi=502e3, gate=1.5, cal=None):
    """完整读数链：去DC→窗→FFT→找峰→抛物线→三点|X|精化→单点DFT→幅值/相位
    防御四条（MCU 同款）：
      ① 栅格预筛：2*mag[k]/(N·CG) < gate 的峰直接跳过（防地板伪峰精化后借用主瓣能量）
      ② log 地板：防 log(0) → nan 传播
      ③ 抛物线 d 限幅 |d|≤0.5（MCU 上超出即丢弃该峰）
      ④ 精化后终判 gate + 频带内
    """
    x = x - x.mean()
    xw = x * W4
    X = np.fft.rfft(xw)
    mag = np.abs(X)
    df = fs / N_FFT
    k_lo, k_hi = int(f_lo/df), min(int(f_hi/df), N_FFT//2 - 1)
    floor = max(mag.max(), 1e-30) * 1e-9
    lmag = np.log(np.maximum(mag, floor))
    peaks = []
    k = k_lo + 1
    while k < k_hi - 3:
        if mag[k] >= mag[k-1] and mag[k] > mag[k+1]:
            if 2*mag[k]/(N_FFT*CG) >= gate:          # ① 栅格预筛
                a, b, c = lmag[k-1], lmag[k], lmag[k+1]
                den = a - 2*b + c
                d = 0.5*(a - c)/den if den != 0 else 0.0
                d = min(max(d, -0.5), 0.5)            # ③ 限幅
                fc0 = (k + d) * df
                fs3 = np.array([fc0 - df/8, fc0, fc0 + df/8])
                n_ = np.arange(N_FFT)
                X3 = np.array([np.dot(xw, np.exp(-2j*np.pi*f*n_/fs)) for f in fs3])
                m3 = np.abs(X3)
                den2 = m3[0] - 2*m3[1] + m3[2]
                d2 = 0.5*(m3[0]-m3[2])/den2 if den2 != 0 else 0.0
                d2 = min(max(d2, -0.5), 0.5)          # ③ 限幅
                f_c = fc0 + d2*df/8
                Xc = np.dot(xw, np.exp(-2j*np.pi*f_c*n_/fs))
                amp = 2*abs(Xc) / (N_FFT*CG)
                ph = np.angle(Xc)
                if amp >= gate and f_lo <= f_c <= f_hi:   # ④ 终判
                    # sin 载波补偿：DFT 对 A·sin(2πft+φ0) 的读数相位 = φ0 − π/2
                    # （Σ sin(θ)e^{−jθ} = −j·N·CG/2）。出口统一补回 π/2，使返回
                    # 相位 = 源端口径（docs/01 §5 ②）；不补则重建落在 cos 口径，
                    # 多谐波 Upp 偏 +25mV（复算④ 点名的那个数）。2026-09-28 修正。
                    peaks.append((f_c, amp, ph + np.pi/2))
            k += 5
        else:
            k += 1
    # 标定修正
    if cal is not None:
        fixed = []
        for (f_c, amp, ph) in peaks:
            g, p = cal(f_c)
            fixed.append((f_c, amp/g, ph - p))
        return fixed
    return peaks

def synth(fs, comps, uj_f=None, uj_a=UJ_PEAK, n=N_FFT, lpf=None, quant=True):
    """合成采样序列（mV 口径）：Σ A sin(2πft+φ) ×前端(增益+LPF) → ADC 量化"""
    t = np.arange(n)/fs
    x = np.zeros(n)
    for (f, a, ph) in comps:
        g, p = (1.0, 0.0) if lpf is None else lpf(f)
        x += a*g*np.sin(2*np.pi*f*t + ph - p) * G_FRONT
    if uj_f is not None:
        g, p = (1.0, 0.0) if lpf is None else lpf(uj_f)
        x += uj_a*g*np.sin(2*np.pi*uj_f*t - p) * G_FRONT
    if quant:
        v = 1650.0 + x                       # 偏置 1650 mV（ADC 中点）
        v = np.clip(v, 0.0, 3300.0)          # ADC 满量程 3300 mV
        x = np.round(v*(2**ADC_BITS/3300.0)) * (3300.0/(2**ADC_BITS)) - 1650.0
        x = x / G_FRONT                      # 折回输入端量纲 (mV)
    return x

def true_params(comps):
    f1 = min(c[0] for c in comps)
    tt = np.linspace(0, 1/f1, 8192, endpoint=False)
    w = np.zeros_like(tt)
    for (f, a, ph) in comps:
        w += a*np.sin(2*np.pi*f*tt + ph)
    upp = np.ptp(w)
    urms = np.sqrt(sum(a*a/2 for (f, a, ph) in comps))
    return upp, urms, f1

P("- 单音理想链路自检：")
fs0 = 2.0e6
for (f, a) in [(25e3, 100.0), (10.5e3, 100.0), (42e3, 50.0), (317e3, 125.0), (499e3, 125.0)]:
    x = synth(fs0, [(f, a, 0.0)], quant=True)
    pk = read_tone(x, fs0)
    err = pk[0][1] - a if pk else float('nan')
    ferr = pk[0][0] - f if pk else float('nan')
    P(f"  - f={f/1e3:8.1f} kHz A={a:6.1f} mV → 读数 {pk[0][1]:8.4f} mV（Δ{err:+.4f}）· Δf={ferr:+.2f} Hz")
P()

# ---------- ② fs 穷举六列表 ----------
P("## ② 采样率穷举（fs = 144MHz/K，N=4096，约束 Δf≤500Hz）")
P()
P("| K | fs(MHz) | Δf(Hz) | 折叠带起点(MHz) | 8阶BW fc=800k@折叠带(dB) | 混叠残留(mV,u_J=100mV峰) | ADC占空(15cyc/36MHz) |")
P("|---|---|---|---|---|---|---|")
for K in range(60, 101):
    fs = SYSCLK/K
    df = fs/N_FFT
    if df > 500.0:
        continue
    fold = fs - F_MAX
    g, _ = bw_response(fold, 8, 800e3)
    att = -20*np.log10(g)
    resid = UJ_PEAK * 10**(-att/20)
    duty = fs*15/36e6*100
    star = " ★" if K == 72 else ""
    P(f"| {K} | {fs/1e6:.4f} | {df:.1f} | {fold/1e6:.3f} | {att:.1f} | {resid:.3f} | {duty:.0f}%{star} |")
P()
P("**结论：K=72 → fs=2.0MHz，Δf=488.28Hz ✓，折叠带起点 1.5MHz，混叠残留 0.65mV。**")
P()

# ---------- ③ 滤波器对比 ----------
P("## ③ 抗混叠低通对比（分析带 10k~500k，折叠带起点 fs−500k=1.5MHz）")
P()
P("| 方案 | 通带@500k(mV/100mV分量) | @1.5MHz 衰减(dB) | 混叠残留(mV) | Qmax |")
P("|---|---|---|---|---|")
for (n, fc) in [(8, 700e3), (8, 750e3), (8, 800e3), (8, 850e3), (12, 550e3), (12, 600e3)]:
    g500, _ = bw_response([500e3], n, fc)
    ripple = 100*abs(1 - g500[0])
    g15, _ = bw_response([1.5e6], n, fc)
    att = -20*np.log10(g15[0])
    resid = UJ_PEAK*10**(-att/20)
    P(f"| {n}阶 BW fc={fc/1e3:.0f}k | {ripple:.3f} | {att:.1f} | {resid:.3f} | {bw_Qmax(n):.2f} |")
P()
g500, ph500 = bw_response([500e3], 8, 800e3)
P(f"- 孔径损失（SMP=3 ADCCLK, ts=83.3ns）@500k：{(1-aperture_loss(500e3,3))*100:.3f}% = {(1-aperture_loss(500e3,3))*100:.3f} mV/100mV 分量")
P(f"- 8阶BW fc=800k 相位：∠H(100k)={bw_response([100e3],8,800e3)[1][0]:.3f} rad, ∠H(500k)={ph500[0]:.3f} rad（标定消）")
P("**结论：8 阶 BW fc=800k：通带起伏 0.03mV、Qmax=2.56、混叠残留 0.65mV，全达标。**")
P()

# ---------- ④ 端到端仿真 ----------
P("## ④ 端到端误差仿真（定稿：fs=2.0MHz · N=4096 · BH4 · 8阶BW fc=800k · ×4增益 · 12bit量化 · 标定）")
P()
fs = 2.0e6
lpf = lambda f: bw_response([f], 8, 800e3)

# 标定表：17 点对数间隔，纯音 100mV 无干扰
cal_f = np.logspace(np.log10(10e3), np.log10(500e3), 17)
cal_g = np.zeros(17); cal_p = np.zeros(17)
for i, f in enumerate(cal_f):
    x = synth(fs, [(f, 100.0, 0.0)], lpf=lpf, quant=True)
    pk = read_tone(x, fs, cal=None)
    cal_g[i] = pk[0][1]/100.0
    cal_p[i] = pk[0][2]
def cal(fq):
    lf, l0 = np.log(fq), np.log(cal_f)
    g = np.exp(np.interp(lf, l0, np.log(cal_g)))
    p = np.interp(lf, l0, cal_p)
    return g, p
# 标定残差自检（在非表点频）
res_max = 0
for f in np.logspace(np.log10(10e3), np.log10(500e3), 41):
    if np.any(np.isclose(f, cal_f, rtol=1e-3)): continue
    x = synth(fs, [(f, 100.0, 0.0)], lpf=lpf, quant=True)
    pk = read_tone(x, fs, cal=cal)
    res_max = max(res_max, abs(pk[0][1]-100.0))
P(f"- 标定残差自检（41 个非表点、100mV 纯音）：最坏 {res_max:.4f} mV")
P()

def scenario(name, comps, uj=None, verbose=True):
    x = synth(fs, comps, uj_f=uj, lpf=lpf, quant=True)
    pk = read_tone(x, fs, cal=cal)
    f1m = min(p[0] for p in pk)
    # 谐波级次与相位重组
    mks = [max(1, round(p[0]/f1m)) for p in pk]
    amps = [p[1] for p in pk]
    # 相位：把各分量相位统一到"以基波相位为参考的源端口径"
    tt = np.linspace(0, 1/f1m, 8192, endpoint=False)
    w = np.zeros_like(tt)
    for (mk, (fc_, a, ph)) in zip(mks, pk):
        w += a*np.sin(2*np.pi*mk*f1m*tt + ph)
    upp = np.ptp(w)
    urms = np.sqrt(sum(a*a/2 for a in amps))
    ut, urt, f1t = true_params(comps)
    # 各分量误差
    errs = []
    for (f, a, ph) in comps:
        best = min(pk, key=lambda p: abs(p[0]-f))
        errs.append(abs(best[1]-a))
    maxamp = max(errs) if errs else 0
    dUpp = upp - ut; dUrms = urms - urt; df1 = f1m - f1t
    nspur = len(pk) - len(comps)
    P(f"- **{name}**：|ΔA|max={maxamp:.3f} mV · ΔUpp={dUpp:+.3f} mV · ΔUrms={dUrms:+.3f} mV · Δf1={df1:+.1f} Hz · 多检谱线 {nspur}")
    if verbose:
        P(f"  - 检出 {len(pk)} 线：" + ", ".join(f"{p[0]/1e3:.1f}kHz/{p[1]:.2f}mV" for p in pk) +
          f"（真值 {len(comps)} 线：" + ", ".join(f"{c[0]/1e3:.1f}kHz/{c[1]:.1f}mV" for c in comps) + "）")
    return maxamp, dUpp, dUrms, df1

P("### 场景表")
scenario("S1 题面示例 f1=10.5k m=(1,3,4)", [(10.5e3,60,0),(31.5e3,30,0),(42e3,20,0)])
scenario("S2 最小信号 f1=10k m=(1,2,3) Upp≈50mV", [(10e3,9,0),(20e3,11,0),(30e3,11,0)])
scenario("S3 高频边缘 f1=250k m=(1,2)", [(250e3,60,0),(500e3,40,0)])
scenario("S4 全带组合 f1=10k m=(1,4,8)", [(10e3,50,0),(40e3,25,0),(80e3,25,0)])
scenario("S5 干扰 u_J=1.0MHz(Nyquist整)", [(10.5e3,60,0),(31.5e3,30,0),(42e3,20,0)], uj=1.0e6)
scenario("S6 干扰 u_J=1.2MHz", [(10.5e3,60,0),(31.5e3,30,0),(42e3,20,0)], uj=1.2e6)
scenario("S7 干扰 u_J=1.49MHz(镜像带外边缘)", [(10.5e3,60,0),(31.5e3,30,0),(42e3,20,0)], uj=1.49e6)
scenario("S8 干扰 u_J=1.5MHz(折叠带起点·最坏)", [(10.5e3,60,0),(31.5e3,30,0),(42e3,20,0)], uj=1.5e6)
scenario("S9 干扰 u_J=1.5MHz + 真有500k分量", [(10.5e3,60,0),(31.5e3,30,0),(42e3,20,0),(500e3,20,0)], uj=1.5e6, verbose=False)
scenario("S10 干扰 u_J=2.5MHz(二次折叠)", [(10.5e3,60,0),(31.5e3,30,0),(42e3,20,0)], uj=2.5e6)
scenario("S11 干扰 u_J=3.0MHz(镜像1MHz带外)", [(10.5e3,60,0),(31.5e3,30,0),(42e3,20,0)], uj=3.0e6)
scenario("S12 干扰 u_J=5.0MHz", [(10.5e3,60,0),(31.5e3,30,0),(42e3,20,0)], uj=5.0e6)
P()

P("### 无标定对照（S1 与 S3）")
def scenario_nocal(name, comps):
    x = synth(fs, comps, lpf=lpf, quant=True)
    pk = read_tone(x, fs, cal=None)
    errs = []
    for (f, a, ph) in comps:
        best = min(pk, key=lambda p: abs(p[0]-f))
        errs.append(best[1]-a)
    P(f"- {name}: ΔA = " + ", ".join(f"{e:+.2f}" for e in errs) + " mV")
scenario_nocal("无标定 S1", [(10.5e3,60,0),(31.5e3,30,0),(42e3,20,0)])
scenario_nocal("无标定 S3", [(250e3,60,0),(500e3,40,0)])
P()

P("### f1 全带最坏扫描（非对齐最坏；f1≤125k 用 m=(1,3,4)，f1>125k 用 m=(1,2)，保证全部谐波 ≤500k）")
worst = dict(a=0, u=0, r=0, f=0)
for f1 in np.arange(10e3, 250e3, 137.0):
    mks = (1, 3, 4) if f1 <= 125e3 else (1, 2)
    us = (60, 30, 25) if len(mks) == 3 else (60, 40)
    comps = [(f1*mk, u, 0) for mk, u in zip(mks, us)]
    x = synth(fs, comps, lpf=lpf, quant=True)
    pk = read_tone(x, fs, cal=cal)
    if len(pk) < 3: continue
    f1m = min(p[0] for p in pk)
    amps = [p[1] for p in pk]
    errs = []
    for (f, a, ph) in comps:
        best = min(pk, key=lambda p: abs(p[0]-f))
        errs.append(abs(best[1]-a))
    ut, urt, _ = true_params(comps)
    tt = np.linspace(0, 1/f1m, 4096, endpoint=False)
    w = np.zeros_like(tt)
    for p in pk:
        mk = max(1, round(p[0]/f1m))
        w += p[1]*np.sin(2*np.pi*mk*f1m*tt + p[2])
    worst['a'] = max(worst['a'], max(errs))
    worst['u'] = max(worst['u'], abs(np.ptp(w)-ut))
    worst['r'] = max(worst['r'], abs(np.sqrt(sum(a*a/2 for a in amps))-urt))
    worst['f'] = max(worst['f'], abs(f1m-f1))
P(f"- 最坏：|ΔA|={worst['a']:.3f} mV · |ΔUpp|={worst['u']:.3f} mV · |ΔUrms|={worst['r']:.3f} mV · |Δf1|={worst['f']:.1f} Hz")
P()

P("### Upp 口径对照（S1 真值 vs 2ΣA vs cos 约定）")
comps = [(10.5e3,60,0),(31.5e3,30,0),(42e3,20,0)]
ut, _, _ = true_params(comps)
P(f"- 真值（sin 零相位重建）：{ut:.2f} mV")
P(f"- 2ΣA 口径：{2*sum(c[1] for c in comps):.2f} mV（差 {2*sum(c[1] for c in comps)-ut:+.2f}）")
tt = np.linspace(0, 1/10.5e3, 8192, endpoint=False)
wc = sum(c[1]*np.cos(2*np.pi*c[0]*tt) for c in comps)
P(f"- cos 口径重建：{np.ptp(wc):.2f} mV（差 {np.ptp(wc)-ut:+.2f}）——禁用")
P()
P("## 总结论")
P("1. fs=2.0MHz（TIM 144MHz/72）· N=4096 · BH4 · 单点DFT精化读数 —— 全部场景幅值误差 <1mV，判据 5mV 余量 ≥5×")
P("2. 抗混叠：8 阶 BW fc=800k（Qmax=2.56），折叠带 1.5MHz 处 −44dB → 残留 0.65mV，是最大不可标定项")
P("3. 17 点对数标定消掉通带起伏+孔径+增益，残差 <0.1mV 量级")
P("4. Upp 必须 sin 口径重建（S1 实测：2ΣA 偏 +65mV、cos 口径偏 +25mV，均击穿判据）")
P("5. Urms 用 √(ΣA²/2) 交流口径；u_J≥1.0MHz 的直通镜像 ≥0.5MHz 全在分析带外，唯一进带路径 1.5MHz 折叠已压到 0.65mV")

with open(OUT, "w", encoding="utf-8") as f:
    f.write("\n".join(L))
print("done", len(L), "lines")
