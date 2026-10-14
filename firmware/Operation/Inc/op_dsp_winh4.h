#ifndef OP_DSP_WINH4_H
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
#define OP_DSP_CG 0.358750000f

extern const float op_dsp_winh4_half[(OP_DSP_N / 2U) + 1U];

/* 窗系数取值（i ∈ [0, N)）：i ≤ N/2 直取，否则镜像到 N-i。
 * 表是生成物，这个 accessor 是它唯一的合法读法 —— 别直接摸 half[]。 */
static inline float op_dsp_winh4_at(uint32_t i) {
    return op_dsp_winh4_half[(i <= (OP_DSP_N / 2U)) ? i : (OP_DSP_N - i)];
}

#endif /* OP_DSP_WINH4_H */
