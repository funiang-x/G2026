#ifndef OP_PEAK_H
#define OP_PEAK_H
/* ============================================================================
 *  op_peak —— 找峰 + 频率精化 + 单点 DFT 读数（Operation 层，纯逻辑）
 *
 *  数值口径与 tools/verify_dsp.py 的 read_tone() **逐行对齐**（那是复算过的
 *  真相源：最坏误差 0.031mV / 1750 帧全带扫描）。任何"优化"改动后必须重跑
 *  S1~S12 单测（Test/test_op_meas.c）确认数字不漂。
 *
 *  ---- 防御四条（verify_dsp 已踩过的坑，缺一翻车，见 docs/01 §5）----
 *  ① 栅格预筛：2·mag[k]/(N·CG) < gate 的峰跳过 —— 量化噪声地板的伪峰会
 *    "借"强峰主瓣斜坡过终判，粗频率乱跳（33mV 伪峰实测）；
 *  ② log 前加地板：防 log(0)→NaN 传播；
 *  ③ 抛物线顶点偏移 |d|≤0.5，超限弃峰 —— 主瓣边缘的三点抛物线会外推出
 *    荒谬频率；
 *  ④ 精化后终判 gate + 频带内 —— 预筛只看栅格值，精化后可能掉下门限。
 *
 *  ---- 单点 DFT 的实现 ----
 *  X(f)=Σ xw[i]·e^{−j2πf·i/fs} 用复数旋转因子递推（每步 4 次乘），**每 256 点
 *  用 sinf/cosf 重置一次锚点** —— f32 纯递推 4096 步的相位误差会累积，重置让
 *  它有界（16 次三角函数调用/频点，总耗时微秒级）。朴素逐点 sinf/cosf 是
 *  4096×2 次/频点，三条谱线就要 ~35ms，不行（docs/01 §8 时序预算 5ms）。
 * ==========================================================================*/
#include <stdint.h>

#include "meas_result.h"
#include "op_cal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 扫描带与门限（题面判据推导，docs/01 §5；Task 可注入覆盖，这里给定稿值）。
 * ① f 带取 [9k, 502k]：比题面 [10k,500k] 略宽 —— 粗频率在栅格上，边缘峰的
 *    抛物线中心可能落进 10k/500k 的内侧栅格，扫描带不放宽会切掉真峰；
 * ② gate=1.5mV：> 最坏混叠残留 0.65mV（防假线），< 最小真分量影响
 *    （<1.1mV ≪ 5mV 判据）。 */
typedef struct {
    float f_lo_hz;
    float f_hi_hz;
    float gate_mv;
} op_peak_params_t;

void op_peak_init(const op_peak_params_t *p);

/* 扫一帧谱，输出峰表（按频率升序 —— 扫描天然升序）。
 *   xw    加窗时域（mV，op_dsp_run 的 xw_out）
 *   mag   栅格幅值（op_dsp_run 的 mag_out）
 *   n     点数（= OP_DSP_N）
 *   fs_hz 采样率 —— 由参数传入（R2：时间/频率事实不进 Operation 的静态状态）
 *   cal   标定表，NULL = 不修正
 *   out   输出峰表；cap 至少 MEAS_MAX_PEAKS
 * 返回峰数（0..cap）；-1 参数非法。 */
int op_peak_run(const float *xw, const float *mag, uint32_t n, float fs_hz,
                const op_cal_table_t *cal, meas_peak_t *out, int cap);

#ifdef __cplusplus
}
#endif
#endif /* OP_PEAK_H */
