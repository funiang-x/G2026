# -*- coding: utf-8 -*-
"""G2026 前端优化项量化复算：fs 微调 K 扫描 / 椭圆 PlanB / 增益上调 / 双帧识别"""
import numpy as np

fc = 800e3; n = 8; N = 4096; TIMCLK = 144e6
def bw8(f): return 1.0/np.sqrt(1.0 + (np.asarray(f, float)/fc)**(2*n))
out = []; w = out.append
w("# G2026 优化项量化复算（2026-09-28）"); w("")

# ---- 1. fs 微调（TIM 整数分频 K=64..76，Δf≤500Hz 才合法）----
w("## 1. fs 微调扫描（TIMCLK=144MHz，K 整数分频；折叠残留 = 100mVpk×0.5×|H(f_fold)|×4）")
w("")
w("| K | fs (MHz) | Δf (Hz) | 合法? | 折叠带起点 (MHz) | @起点衰减 dB | 残留 mVpk | 对比 K=72 |")
w("|---|---|---|---|---|---|---|---|")
for K in range(64, 77):
    fs = TIMCLK/K; df = fs/N; fold = fs - 500e3
    h = float(bw8(fold)); res = 100e-3*0.5*h*4*1e3
    legal = "✓" if df <= 500.0 else "✗"
    cmp_ = "—" if K == 72 else "%+.2f mV" % (res - 1.309)
    w("| %d | %.4f | %.1f | %s | %.4f | %.1f | %.3f | %s |" % (K, fs/1e6, df, legal, fold/1e6, 20*np.log10(h), res, cmp_))
w("")
w("结论：合法 K 中最优 K=71（fs=2.0282M）残留 1.31→1.13mV，**收益仅 0.18mV**，代价是软件文档全部")
w("DSP 复算（S1~S12、1750 帧扫描、DMA/时序）重跑回填 → **拒绝**。K=73 反而更差（1.52mV）。")
w("")

# ---- 2. 椭圆 PlanB（Amin 地板决定最坏残留，算术换算）----
w("## 2. 椭圆（Cauer）滤波 PlanB 量化：残留 = 1.309mV × 10^((−43.7 + Amin_dB)/20)")
w("")
for amin in [50, 60]:
    res = 1.309 * 10**((43.7 - amin)/20)
    w("- Amin=%ddB（零点@1.5M）→ 最坏残留 %.3f mV，RSS ≈ %.2f mV，余量 %.1f×" % (amin, res, np.sqrt(res**2+0.11**2), 5/np.sqrt(res**2+0.11**2)))
w("")
w("n=6 椭圆（3L/3~4C）比巴特沃斯 n=8（4L/4C）**还少一个电感**，但元件值非 E24、零点随容差漂移、")
w("DSP 文档滤波器模型要改 → **现在不做，作为实测残留超标的 Plan B 挂起**。")
w("")

# ---- 3. 增益上调 ×4→×6 ----
lsb4 = 3.3/4096/4; lsb6 = 3.3/4096/6
q4 = lsb4/np.sqrt(12)/np.sqrt(N/2)*1e3; q6 = lsb6/np.sqrt(12)/np.sqrt(N/2)*1e3
w("## 3. 增益 ×4→×6 的真实收益")
w("")
w("- LSB(输入折算): %.3f→%.3f mV；量化项(√N 增益后): %.4f→%.4f mV；**收益 %.4f mV → 拒绝**" % (lsb4*1e3, lsb6*1e3, q4, q6, q4-q6))
w("")

# ---- 4. 双帧变频识别 u_J（机制估算，非全仿真）----
w("## 4. 双帧变频识别 u_J（fs1=2.0282M/K71 + fs2=1.9726M/K73，各一帧）")
w("")
w("- 机制：同一 f_J 在两帧折叠到不同频率，真分量频率不动 → 谱线对帧配对，移动者判为 u_J 剔除，")
w("  残留项 → ~0.1mV 量级，RSS → ~0.15mV，余量 → ~30×。")
w("- 代价：软件文档 §5/§6 重写 + verify_dsp 全重跑 + 双帧时序；当前余量 3.8× 已够 → **挂「精度拉满」可选项，不做**。")
w("")

# ---- 5. 端接 vs 不端接（已在 v3 论证，此处固化数字）----
w("## 5. 端接 ×0.5 的价值固化")
w("")
w("- 不端接（BUF1 直读，链路 ×4）：残留 = 100mVpk×0.006546×4 = %.3f mV **> 1.5mV 峰门限**（伪峰风险，须抬门限）；" % (100e-3*float(bw8(1.5e6))*4*1e3))
w("- 端接（现状）：%.3f mV < 门限 ✓ → 端接不是损耗，是链路里唯一免费无失真的抗混叠增益。" % (100e-3*0.5*float(bw8(1.5e6))*4*1e3))
w("")
w("**采纳/拒绝裁决汇总见 02-architecture.md §6。**")
with open(__file__.rsplit(".", 1)[0].replace("verify_opt", "report_opt") + ".md", "w", encoding="utf-8") as f:
    f.write("\n".join(out) + "\n")
print("ok", len(out))
