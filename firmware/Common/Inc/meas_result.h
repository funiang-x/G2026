#ifndef MEAS_RESULT_H
#define MEAS_RESULT_H
/* ============================================================================
 *  meas_result —— 测量结果结构（Common 层横切：Task 与 Operation 都要用）
 *
 *  ★ 为什么放 Common：`op_meas`（Operation）生产它，`app_ui`（Task）消费它 ——
 *    Common 零依赖，两层都能 include（docs/01 §12：MeasResult_t 定义放 Common）。
 *
 *  ⚠️ **结果体 ≤64B**（docs/01 §8 的硬约束，历史教训：4144B 局部变量爆栈）。
 *     波形/谱线的渲染数据**不进**这个结构 —— 它们只在 UI 层的静态区生成。
 *     本结构不含指针（PC 单测做逐值断言，不需要堆）。
 * ==========================================================================*/
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 单帧最多检出的谱线数。题面："信号 = 基波 + 1~2 个谐波" ⇒ 3 根封顶
 * （op_peak 的主瓣合并保证不会把一根谱线的旁瓣算成多根）。
 * ⚠️ 不带 U 后缀：它大量出现在 int 循环计数里，-Wsign-compare 会逐个炸。 */
#define MEAS_MAX_PEAKS 3

/* 场景标注（题面第 1/2/3 项）。算法唯一，它只切换屏幕上的标注（docs/01 §7）。 */
#define MEAS_SCENE_UA 0U  /* u_a：Upp 100~250mV，10k~200kHz */
#define MEAS_SCENE_UB 1U  /* u_b：50~250mV，10k~500kHz */
#define MEAS_SCENE_UBJ 2U /* u_b + u_J（f_J≥1MHz 干扰）—— 抑制后仍测 u_b */

typedef struct {
    float freq_hz;  /* 精化后的频率（单点 DFT 频点，不是 FFT 栅格） */
    float amp_mv;   /* 幅值，输入端 mV（已除前端增益、已做标定修正） */
    float phase_rad;/* 该分量的相位（源端口径：标定已消前端相移） */
} meas_peak_t;

typedef struct {
    uint32_t    seq;      /* 序列号：每发布一次 +1；UI 靠它判断"有新数据" */
    uint8_t     scene;    /* MEAS_SCENE_*，仅用于显示标注 */
    uint8_t     n_peaks;  /* 1..MEAS_MAX_PEAKS */
    uint16_t    _pad;
    float       f1_hz;    /* 基频 = 最低峰的精化频率 */
    float       upp_mv;   /* Upp = sin 口径重建波形的 max−min（口径②） */
    float       urms_mv;  /* Urms = √(ΣA_k²/2)，交流频域口径（口径③） */
    meas_peak_t peak[MEAS_MAX_PEAKS];
} meas_result_t; /* 4+1+1+2+4+4+4 + 3×12 = 56B ≤ 64B ✓ */

#ifdef __cplusplus
}
#endif
#endif /* MEAS_RESULT_H */
