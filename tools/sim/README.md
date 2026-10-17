# tools/sim —— G2026 模拟电路仿真链

> **为什么自己写内核**：本机没有 ngspice / LTspice，外网（SourceForge / GitHub）在沙箱内不可达。
> 而且本工程需要的是**可复现、可审计、能被门禁跑**的仿真，不是一次性的 GUI 操作。
> 另出 `.cir` 网表（`spice/`）供**外部 SPICE 独立复核**。

## 一键跑

```bash
PY=python

$PY tools/sim/selftest_mna.py    # ① 内核自检：6 项与解析解对拍（必须全绿）
$PY tools/sim/studies.py         # ② 七项研究 S1~S7 → out/studies.json（约 30s）
$PY tools/sim/s8_e2e_fw.py       # ③ ★ 题面示例端到端复现（固件算法逐句照搬，退出码 0 = 全过）
$PY tools/sim/verify_opt.py      # ④ 改前/改后对比 → out/verify_opt.json
$PY tools/sim/emit_spice.py      # ⑤ 生成 spice/*.cir

# 外部 SPICE 复核（装了 ngspice 的话）
ngspice -b tools/sim/spice/afe_ac.cir
ngspice -b tools/sim/spice/adc_kickback.cir
```

## 文件

| 文件 | 作用 |
|---|---|
| `mna.py` | **MNA 内核**。元件：R / L / C / V / I / E(VCVS) / G(VCCS) / D(二极管，pnjlim 软限幅) / 时变开关 / **tanh 软饱和跨导**。分析：DC / 复数 AC / 瞬态（梯形 + 步首与开关处后向欧拉 + 牛顿）。 |
| `selftest_mna.py` | 内核的 6 项解析对拍：RC 频响、RC 充电（BE 与梯形各一遍）、RLC 谐振、L 直流短路、二极管压降、时变开关。**跑不通就不许往上层建模型。** |
| `afe_net.py` | AFE 网表（按活体网表 1:1）+ **OPA2197 宏模型** + ADC 采样网络 + `bh4`/`dft_amp` 口径工具。 |
| `studies.py` | S1 端到端 AC · S2 蒙特卡洛 · S3 ADC 采样反冲 · S4 两音 IM3 · S5 噪声预算 · S6 电源耦合/PSRR · S7 前端 LTI 频域视角。 |
| **`s8_e2e_fw.py`** | **★ 题面示例端到端复现**：源 → AFE → 12bit ADC → **固件的峰检测链（逐句照搬）** → 显示值，逐分量对判据。`_winh4.npy` 是从 `firmware/Operation/Src/op_dsp_winh4.c` 抽出来的**固件自己的窗表**。 |
| `verify_opt.py` | 改前/改后一键对比（H1/H2 的验收证据）。 |
| `emit_spice.py` | 导出 `spice/*.cir`。 |
| `out/*.json` | 机器可读的结果，供报告引用。 |

## OPA2197 宏模型的三条锚点（不猜）

| 参数 | 值 | 锚在哪 |
|---|---|---|
| GBW | 10 MHz | datasheet |
| Aol | 1e6（120 dB） | 只影响 DC 精度，不影响带宽 |
| **SR** | **20 V/µs** | datasheet —— 它**同时**决定差分对的软饱和电压 `Vk1 = SR·C1/gm1` |
| Ro（开环输出阻抗） | 100 Ω | 量级 |
| Isc（短路电流） | ±20 mA | datasheet |
| Cin_diff / Cin_cm | 8 pF / 4 pF | **工程假设**（沿用 `hardware/17` 的 Cin−=8pF）；**必须做敏感性分析** |
| PSRR | 可设（默认 100 dB） | **显式建模** —— 不建模 PSRR 就等于假设"纹波进不了输出"，那 R21 该不该改就成了拍脑袋 |

宏模型实测：GBW **9.885 MHz** · SR **≈19 V/µs** · 单位增益阶跃稳定收敛。

## 三个"只有仿真能抓"的坑（本工程实际踩到的）

1. **差分对极性写反**：AC 增益、DC 工作点全部正确，**只有瞬态会发散**（右半平面极点）。
2. **通带形状押在未知的 Cin 上**：Cin 2p→16p 时通带起伏 0.03→2.36 dB。
3. **电源纹波耦合**：R21=0Ω 时 PSRR 60dB 下 500kHz 耦合 −36.8 dB（1V 纹波 → 14mV 到 ADC）。

## 改图后必须做的事

`afe_net.py` 里的元件值是**从活体网表抄的**。改图后：

```bash
python _review3/netlist_now.py AFE INT PWR     # 重读活体网表
# 人工把 afe_net.py 的 V[...] 对齐，然后重跑 selftest + studies + verify_opt
```

## 边界（别越界）

- 内核**没有**建模：输出级交叉失真（只用输入对 tanh + 输出级软饱和近似）、温漂、1/f 噪声、CMRR、走线寄生。
  凡这些项影响的结论，都在报告里标了"未建模/敏感性分析覆盖"。
- 仿真结论一律是**设计验证**，不是**上板验证**。上板判据见 `hardware/27 §7.2`。
