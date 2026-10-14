# -*- coding: utf-8 -*-
"""生成 BH4 窗的 Flash 常量表 → firmware/Operation/Src/op_dsp_winh4.c

系数照抄 tools/verify_dsp.py 的 BH4（数值复算的真相源）——改系数先重跑复算，
再跑本脚本重新生成表。

★ 2026-10-07 起存**半表**：BH4 满足 w[i] == w[N-i]（对 4096 点全表逐点验证
  0 违例，本脚本每次生成前都重新验证），所以只存 0..N/2 共 N/2+1 点，
  取数经 op_dsp_winh4_at() 镜像 —— Flash 从 16KB 降到 8KB。
  要回到全表：把下面 `half` 改回 `w`、数组长度改回 OP_DSP_N、头文件里的
  accessor 删掉即可。
"""
import math
import pathlib

N = 4096
# 与 verify_dsp.py 完全一致（注意 a3 = 0.011568，不是教科书 0.01168）
BH4 = [0.35875, 0.48829, 0.14128, 0.011568]

w = []
for n in range(N):
    t = 2.0 * math.pi * n / N
    w.append(BH4[0] - BH4[1] * math.cos(t) + BH4[2] * math.cos(2 * t) - BH4[3] * math.cos(3 * t))
cg = sum(w) / N

# 对称性哨兵：半表的正确性前提。破坏就拒绝生成，别静默产出错表。
# ⚠️ 按**落盘精度**（:.9g，与下面的 f-string 同口径）比较：双精度下 cos 的
#    末位差异（1e-15 量级）在 9 位有效数字里必然被舍掉 —— 判据是"emit 出去
#    的半表能逐字节重建 emit 出去的全表"，这才是半表正确的定义。
for i in range(1, N // 2 + 1):
    if f"{w[i]:.9g}" != f"{w[N - i]:.9g}":
        raise SystemExit(f"对称性破坏 @i={i}: {w[i]!r} != {w[N - i]!r} —— 不能存半表")

half = w[: N // 2 + 1]

src = pathlib.Path(__file__).resolve().parent.parent / "firmware" / "Operation"
lines = []
lines.append('#include "op_dsp_winh4.h"')
lines.append("")
lines.append("/* ⚠️ 自动生成（仓库根 tools/gen_winh4.py）—— 手改无效，下次生成会覆盖。")
lines.append(" * 系数与 verify_dsp.py 的 BH4 一致；CG 是全表的精确均值。")
lines.append(" * 只存半表（0..N/2）：w[i] == w[N-i] 生成前已逐点验证，取数走 op_dsp_winh4_at()。 */")
lines.append("const float op_dsp_winh4_half[(OP_DSP_N / 2U) + 1U] = {")
for i in range(0, len(half), 8):
    lines.append("    " + ", ".join(f"{v:.9g}f" for v in half[i:i + 8]) + ",")
lines.append("};")
(src / "Src" / "op_dsp_winh4.c").write_text("\n".join(lines) + "\n", encoding="utf-8")

hdr = f"""#ifndef OP_DSP_WINH4_H
#define OP_DSP_WINH4_H
/* ============================================================================
 *  op_dsp_winh4 —— BH4 窗 Flash 常量表（⚠️ 自动生成，仓库根 tools/gen_winh4.py）
 *
 *  系数 = verify_dsp.py 的 BH4（数值复算真相源）；改系数 → 重跑复算 → 重跑脚本。
 *  ★ 存**半表**（0..N/2，2026-10-07 起）：BH4 满足 w[i] == w[N-i]（全表逐点
 *    验证 0 违例），取数经下面的镜像 accessor —— Flash 16KB → 8KB，
 *    RAM 留给 DMA/FFT 缓冲（board_config CCM 切分）。
 * ==========================================================================*/
#include <stdint.h>

#define OP_DSP_N 4096U

/* 相干增益 CG = mean(w)（全表均值，存半表不改口径）。幅值口径 A = 2|X| / (N·CG)。 */
#define OP_DSP_CG {cg:.9f}f

extern const float op_dsp_winh4_half[(OP_DSP_N / 2U) + 1U];

/* 窗系数取值（i ∈ [0, N)）：i ≤ N/2 直取，否则镜像到 N-i。
 * 表是生成物，这个 accessor 是它唯一的合法读法 —— 别直接摸 half[]。 */
static inline float op_dsp_winh4_at(uint32_t i) {{
    return op_dsp_winh4_half[(i <= (OP_DSP_N / 2U)) ? i : (OP_DSP_N - i)];
}}

#endif /* OP_DSP_WINH4_H */
"""
(src / "Inc" / "op_dsp_winh4.h").write_text(hdr, encoding="utf-8")
print(f"CG = {cg:.9f}  (a3={BH4[3]})")
print("written:", src / "Src" / "op_dsp_winh4.c", "and", src / "Inc" / "op_dsp_winh4.h")
