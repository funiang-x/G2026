# -*- coding: utf-8 -*-
"""生成 BH4 窗的 Flash 常量表 → firmware/Operation/Src/op_dsp_winh4.c

系数照抄 tools/verify_dsp.py 的 BH4（数值复算的真相源）——改系数先重跑复算，
再跑本脚本重新生成表。表 4096×4B = 16KB 在 Flash（设计 docs/01 §5：RAM 留给
DMA 与 FFT 缓冲，窗表不占）。
"""
import pathlib

N = 4096
# 与 verify_dsp.py 完全一致（注意 a3 = 0.011568，不是教科书 0.01168）
BH4 = [0.35875, 0.48829, 0.14128, 0.011568]

import math
w = []
for n in range(N):
    t = 2.0 * math.pi * n / N
    w.append(BH4[0] - BH4[1] * math.cos(t) + BH4[2] * math.cos(2 * t) - BH4[3] * math.cos(3 * t))
cg = sum(w) / N

src = pathlib.Path(__file__).resolve().parent.parent / "firmware" / "Operation"
lines = []
lines.append('#include "op_dsp_winh4.h"')
lines.append("")
lines.append("/* ⚠️ 自动生成（tools/gen_winh4.py）—— 手改无效，下次生成会覆盖。")
lines.append(" * 系数与 verify_dsp.py 的 BH4 一致；CG 是本表的精确均值。 */")
lines.append(f"const float op_dsp_winh4[OP_DSP_N] = {{")
for i in range(0, N, 8):
    lines.append("    " + ", ".join(f"{v:.9g}f" for v in w[i:i + 8]) + ",")
lines.append("};")
(src / "Src" / "op_dsp_winh4.c").write_text("\n".join(lines) + "\n", encoding="utf-8")

hdr = f"""#ifndef OP_DSP_WINH4_H
#define OP_DSP_WINH4_H
/* ============================================================================
 *  op_dsp_winh4 —— BH4 窗 Flash 常量表（⚠️ 自动生成，tools/gen_winh4.py）
 *
 *  系数 = verify_dsp.py 的 BH4（数值复算真相源）；改系数 → 重跑复算 → 重跑脚本。
 *  表 4096×4B = 16KB 在 Flash：RAM 留给 DMA/FFT 缓冲（board_config CCM 切分）。
 * ==========================================================================*/
#define OP_DSP_N 4096U

/* 相干增益 CG = mean(w)。幅值口径 A = 2|X| / (N·CG)（docs/01 §5）。 */
#define OP_DSP_CG {cg:.9f}f

extern const float op_dsp_winh4[OP_DSP_N];

#endif /* OP_DSP_WINH4_H */
"""
(src / "Inc" / "op_dsp_winh4.h").write_text(hdr, encoding="utf-8")
print(f"CG = {cg:.9f}  (a3={BH4[3]})")
print("written:", src / "Src" / "op_dsp_winh4.c", "and", src / "Inc" / "op_dsp_winh4.h")
