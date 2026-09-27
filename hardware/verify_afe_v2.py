# -*- coding: utf-8 -*-
"""G2026 前端 v2 复算：无源 LC 8阶巴特沃斯 fc=800k @50Ω + OPA2197 ×4（滤波在前）
判据链：u_J 按题面 200mVpp 不打折；折叠带起点 = fs − f_max；所有 dB/mV 脚本算，不外推。
报告写 utf-8 文件再读（skill 环境事实）。"""
import numpy as np

fc = 800e3
fs = 2.0e6
N  = 4096
R  = 50.0
n  = 8
GAIN = 4.0
DIV  = 0.5          # 50Ω 源 ÷ 50Ω 端接
GBW_OP = 10e6       # OPA2197
SR_OP  = 20.0       # V/µs
UJ_PP  = 200e-3     # 题面：u_J 200mVpp
FMIN, FMAX = 9e3, 502e3   # 分析带（软件文档 §5 gate）

out = []
w = out.append
w("# G2026 前端 v2 数值复算报告（LC 无源 + OPA2197）")
w("")
w("- 生成：2026-09-28 · 脚本 verify_afe_v2.py · numpy %s" % np.__version__)
w("- 链路：BNC→TVS→LC8(BW fc=800k, 50Ω)→50Ω负载→AC耦合(100nF/10k)→×4(OPA2197)→100Ω/470pF→ADC1")
w("- fs=2.0MHz / N=4096 / BH4 不变（与软件文档一致）")
w("")

# ---------- 1. 巴特沃斯精确响应（解析式，非渐近外推） ----------
def bw8(f):
    return 1.0/np.sqrt(1.0 + (np.asarray(f, dtype=float)/fc)**(2*n))

w("## 1. 8 阶巴特沃斯 fc=800kHz 精确衰减（|H|=1/√(1+(f/fc)^16)）")
w("")
w("| 频率 | 用途 | |H| | dB |")
w("|---|---|---|---|")
for f, tag in [(10e3,"带内底"),(100e3,"带内"),(200e3,"带内(u_a上限)"),(500e3,"带内顶"),
               (800e3,"-3dB点"),(1.0e6,"Nyquist/u_J合法域底"),(1.5e6,"折叠带起点"),
               (2.5e6,"二次折叠"),(3.5e6,"三次折叠")]:
    h = float(bw8(f)); w("| %.3f MHz | %s | %.4g | %.1f |" % (f/1e6, tag, h, 20*np.log10(h)))
w("")

# ---------- 2. LC 梯形元件值（Butterworth 原型闭式 g_k=2sin((2k-1)π/2n)） ----------
g = [2*np.sin((2*k-1)*np.pi/(2*n)) for k in range(1, n+1)]
Lb = R/(2*np.pi*fc)      # 每单位 g 的电感
Cb = 1/(2*np.pi*fc*R)    # 每单位 g 的电容
E24 = [1.0,1.1,1.2,1.3,1.5,1.6,1.8,2.0,2.2,2.4,2.7,3.0,3.3,3.6,3.9,4.3,4.7,5.1,5.6,6.2,6.8,7.5,8.2,9.1]
def e24(x):
    m = x/ (10**np.floor(np.log10(x)))
    best = min(E24, key=lambda e: abs(e-m))
    return best * 10**np.floor(np.log10(x))
w("## 2. LC 元件值（fc=800k、Rs=RL=50Ω；串 L 起头，L/C 交替）")
w("")
w("g 序列（闭式）：[" + ", ".join("%.4f"%x for x in g) + "]")
w("")
w("| 节 | 类型 | 理想值 | 最近 E24 | 偏差 |")
w("|---|---|---|---|---|")
for k in range(1, n+1):
    if k % 2 == 1:
        v = g[k-1]*Lb; e = e24(v)
        w("| %d | 串联 L%d | %.2f µH | %.1f µH | %+.1f%% |" % (k, (k+1)//2, v*1e6, e*1e6, (e/v-1)*100))
    else:
        v = g[k-1]*Cb; e = e24(v)
        w("| %d | 并联 C%d | %.3f nF | %.2f nF | %+.1f%% |" % (k, k//2, v*1e9, e*1e9, (e/v-1)*100))
w("")
w("电感规格要求：绕线型 SMD（CD54/CD75 或 SWPA 类），SRF ≥ 12 MHz，DCR ≤ 0.5 Ω，Isat ≥ 50 mA（实际信号电流 ≈ 4.5 mA pk）。")
w("全部为 0805/1206 + 通孔 BNC，手焊 ✓。电容全部 C0G/NP0。")
w("")

# ---------- 3. 折叠残留（u_J 按题面 200mVpp，逐频扫折叠落点） ----------
w("## 3. u_J 折叠残留 @ADC（链路总增益 = DIV×|H(f_J)|×GAIN，理想 LC）")
w("")
w("u_J 源端 200mVpp = 100 mVpk → 端接后 50 mVpk → ×|H| → ×4。")
w("")
w("| f_J | 折叠落点 | 落点在分析带? | ADC 处残留 pk | 超过 1.5mV 峰门限? |")
w("|---|---|---|---|---|")
worst = 0.0
for fj in np.arange(1.0e6, 2.501e6, 10e3):
    alias = abs(fj - fs*round(fj/fs))
    if FMIN <= alias <= FMAX:
        spur = 100e-3 * DIV * float(bw8(fj)) * GAIN   # V, pk
        worst = max(worst, spur)
if True:
    # 列出关键代表点
    for fj in [1.0e6, 1.01e6, 1.25e6, 1.49e6, 1.5e6, 1.51e6, 1.75e6, 2.0e6, 2.25e6, 2.49e6]:
        alias = abs(fj - fs*round(fj/fs))
        inb = FMIN <= alias <= FMAX
        spur = 100e-3 * DIV * float(bw8(fj)) * GAIN
        w("| %.2f MHz | %.3f MHz | %s | %.3f mV | %s |" % (fj/1e6, alias/1e6, "是" if inb else "否",
          spur*1e3, "是" if (inb and spur>1.5e-3) else "否"))
w("")
w("**全带扫描（10kHz 步进，f_J∈[1.0,2.5]MHz，落点在带内者）最坏残留 = %.3f mV pk**" % (worst*1e3))
w("")
w("结论：最坏点 f_J=1.5MHz 恰折叠 → %.3f mV < 1.5 mV 峰检测门限（独立伪峰会被门限拦下）；" % (100e-3*DIV*float(bw8(1.5e6))*GAIN*1e3))
w("若恰落在真分量上，幅值误差 ≤ %.3f mV < 5 mV 判据 ✓。" % (100e-3*DIV*float(bw8(1.5e6))*GAIN*1e3))
w("f_J∈[1.0,1.5)MHz 的镜像落 [0.5,1.0)MHz 带外（软件文档 §6 层②机制不变）；f_J=1.0M 整落 Nyquist bin 随 mag[0] 丢弃。")
w("")

# ---------- 4. 与软件文档 §6 残留数字的对照 ----------
w("## 4. 文档数字对照（发现的问题）")
w("")
h15 = float(bw8(1.5e6))
w("- 软件文档 §6 写「@1.5MHz −43.7dB → 残留 0.655mV」。精确值 −%.1f dB ✓（衰减本身对）。" % abs(20*np.log10(h15)))
w("- 但按其 §3 链路（端接×0.5 → ×4 → 滤波）推残留 = 100mVpk×0.5×4×%.4g = **%.3f mV pk**，" % (h15, 100e-3*DIV*GAIN*h15*1e3))
w("  与 0.655mV 差 **2 倍**——文档该行少算了端接分压与增益的其中一项（LTI 链上增益与滤波可交换，顺序不影响结果）。")
w("- 修正后预算：混叠残留 1.31 mV（原 0.655）→ RSS ≈ 1.31 mV，对 5mV 判据余量 ≈ 3.8×（原称 7.5×）。仍达标，但 §6/§10 两行数字须回改；")
w("  **改动任何参数 → 按文档 §14 要求重跑 tools/verify_dsp.py 后回填**（本报告只裁定解析界，统计最坏以重跑为准）。")
w("")

# ---------- 5. 通带特性与标定（LTI 全部可标） ----------
w("## 5. 通带（标称口径 vs 标定后）")
w("")
w("增益级闭环带宽（×4，一阶近似）= GBW/4 = %.1f MHz；与 LC 级联的通带响应：" % (GBW_OP/GAIN/1e6))
w("")
w("| 频率 | LC |H| dB | 增益级 dB | 级联偏差/100mV 分量 |")
w("|---|---|---|---|")
for f in [10e3, 50e3, 100e3, 200e3, 315e3, 500e3]:
    hlc = 20*np.log10(float(bw8(f)))
    hga = 20*np.log10(1/np.sqrt(1+(f/(GBW_OP/GAIN))**2))
    tot = hlc + hga
    w("| %.0f kHz | %+.3f | %+.3f | %+.2f mV |" % (f/1e3, hlc, hga, ((10**(tot/20))-1)*100e-3*1e3))
w("")
w("- AC 耦合角（100nF/10k）= %.0f Hz，10 kHz 处 droop ≈ %.4f dB → 可忽略；" % (1/(2*np.pi*10e3*100e-9), 20*np.log10(1/np.sqrt(1+(1/(2*np.pi*10e3*100e-9)/10e3)**2))))
w("- 全部 LTI → 17 点标定表吸收后残差 ~0.023mV（软件文档 §10/§11 机制不变）。**OPA2197 方案下标定为必做项**（10MHz 增益级的 -0.17dB@500k 靠标定消）。")
w("")

# ---------- 6. OPA2197 动态范围核算 ----------
w("## 6. OPA2197 压摆率/摆幅/噪声核算")
w("")
vin_max = 62.5e-3   # 滤波后运放输入最坏（u_b 250mVpp→端接 62.5mVpk；u_J 已被 LC 压掉）
vout_max = vin_max * GAIN
sr_need = 2*np.pi*500e3*vout_max/1e6
w("- 滤波在前：运放只见 ≤%.0f mVpk 带内信号（u_J 残留 %.2f mVpk@输入），输出 ≤%.0f mVpk @≤500kHz；" % (vin_max*1e3, 100e-3*DIV*h15*1e3, vout_max*1e3))
w("- 需求 SR = 2π×500k×%.0fmV = %.2f V/µs ≪ 20 V/µs ✓（对照：若增益在前，u_J=200mVpk@1.5M 需 %.1f V/µs，也 < 20，但 IM3 风险高——仍选滤波在前）" % (vout_max*1e3, sr_need, 2*np.pi*1.5e6*200e-3/1e6))
w("- 输出摆幅：1.65V ± %.0f mV = %.2f~%.2f V，RRIO 5V 供电 ✓（对 1.65V 半量程余量 %.2f V）" % (vout_max*1e3, 1.65-vout_max, 1.65+vout_max, 1.65-vout_max))
w("- 噪声：运放 ~2.9nV/√Hz×4×√(500k×π/2) ≈ %.1f µV rms；50Ω 源热噪 ~%.1f µV rms —— 距 5mV 判据均 >50× ✓" % (2.9e-9*4*np.sqrt(500e3*np.pi/2)*1e6, np.sqrt(4e-21*50*500e3*np.pi/2)*1e6))
w("- 半程 B 半管 = 1.65V 基准缓冲（VREF3033÷2 分压），兼作上电自检已知电平（软件文档 §9 自检要求）✓")
w("")

# ---------- 7. 架构对照 ----------
w("## 7. 有源方案 vs LC 方案（本阶段定案依据）")
w("")
w("| 项 | 原案：有源 8 阶（SK/MFB） | **v2：无源 LC 8 阶 + OPA2197** |")
w("|---|---|---|")
w("| 运放 GBW 门槛 | SK 规则 ≥100×f0×Qmax ≈ 205MHz（TLV3544 贴线） | 无要求（滤波器无源，OPA2197 10MHz 只做 ×4） |")
w("| Q=2.56 稳定性风险（R1） | 存在，依赖扫频验证 | **消失**（双端接 LC 梯形天生低灵敏度） |")
w("| 器件成本 | TLV3544+TLV3542 ≈ ¥30+ | OPA2197×1（手头已有）+ L/C ≈ ¥5 |")
w("| 手焊 | SOIC-14/8 ✓ | 0805/1206 + SOIC-8 ✓ |")
w("| DSP 侧影响 | — | 理想传递函数与原案完全相同（BW8 fc=800k），§4 采样/§5 口径全部不动；仅 §6/§10 残留行按本报告 §4 修正 |")
w("")
w("**结论：v2 为给定约束下的最优解** —— 用手头器件、消掉最大风险项（R1）、保住 DSP 复算结论、BOM 省 ¥30+。")

with open(__file__.rsplit(".", 1)[0] + "_report.md", "w", encoding="utf-8") as f:
    f.write("\n".join(out) + "\n")
print("report written:", len(out), "lines")
