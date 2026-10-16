#ifndef MEAS_RESULT_H
#define MEAS_RESULT_H
/* ============================================================================
 *  meas_result —— 测量结果结构（Common 层横切：Task 与 Operation 都要用）
 *
 *  ★ 为什么放 Common：`op_meas`（Operation）生产它，`app_ui`（Task）消费它 ——
 *    Common 零依赖，两层都能 include（docs/01 §12：MeasResult_t 定义放 Common）。
 *
 *  ⚠️ **结果体 ≤84B**（docs/01 §8 的硬约束，历史教训：4144B 局部变量爆栈；
 *     原 64B 上限随 2026-10-13 的 5 峰 + THD 改造放宽到 84B，见文件尾 _Static_assert）。
 *     波形/谱线的渲染数据**不进**这个结构 —— 它们只在 UI 层的静态区生成。
 *     本结构不含指针（PC 单测做逐值断言，不需要堆）。
 * ⚠️ **Period 不进结构体**：`1e6/f1_hz`（µs）是 UI 层一行派生（页 2 DETAILS 用），
 *     省 4B、避免口径分裂。
 * ==========================================================================*/
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 单帧最多检出的谱线数。题面信号 = "基波 + 1~2 个谐波" ⇒ **5 根足够**。
 * ⚠️ 2026-10-15：曾试 5→16（想让方波的显示重建多几根谐波），**被本文件末尾的
 *    `_Static_assert(sizeof ≤ 84B)` 红线挡回**（16 根 = 216B）。要再提，先动 docs/01 §8
 *    那条红线并想清楚谁在按值拷贝这个结构体。
 * ⚠️ 不带 U 后缀：它大量出现在 int 循环计数里，-Wsign-compare 会逐个炸。 */
#define MEAS_MAX_PEAKS 5

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
    float       thd;      /* THD = √(Σ_{k≥2}A_k²)/A1（0..∞；n_peaks<2 或 A1=0 时 0）。
                           * 口径见 op_meas.c；题面第 4 项要求显示 THD ⇒ 落在这里。 */
    meas_peak_t peak[MEAS_MAX_PEAKS];
} meas_result_t; /* 4+1+1+2 + 4×4 + 5×12 = 84B */

/* ⚠️ 尺寸红线（docs/01 §8）：原为 ≤64B，2026-10-13 随 MEAS_MAX_PEAKS 3→5 与 thd
 *     放宽到 **≤84B**（路线 A，最小改动）：5 峰 + thd 后算术就是 84B。
 *     代价 = 1 深 mailbox 多占 32B RAM（CCM/SRAM 均无压力）；
 *     波形/谱线的渲染数据仍不进这个结构。 */
_Static_assert(sizeof(meas_result_t) <= 84, "meas_result_t 超过 84B 红线（docs/01 §8）");

#ifdef __cplusplus
}
#endif
#endif /* MEAS_RESULT_H */
