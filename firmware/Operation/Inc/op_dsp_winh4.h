#ifndef OP_DSP_WINH4_H
#define OP_DSP_WINH4_H
/* ============================================================================
 *  op_dsp_winh4 —— BH4 窗 Flash 常量表（⚠️ 自动生成，tools/gen_winh4.py）
 *
 *  系数 = verify_dsp.py 的 BH4（数值复算真相源）；改系数 → 重跑复算 → 重跑脚本。
 *  表 4096×4B = 16KB 在 Flash：RAM 留给 DMA/FFT 缓冲（board_config CCM 切分）。
 * ==========================================================================*/
#define OP_DSP_N 4096U

/* 相干增益 CG = mean(w)。幅值口径 A = 2|X| / (N·CG)（docs/01 §5）。 */
#define OP_DSP_CG 0.358750000f

extern const float op_dsp_winh4[OP_DSP_N];

#endif /* OP_DSP_WINH4_H */
