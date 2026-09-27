#ifndef OP_DSP_H
#define OP_DSP_H
/* ============================================================================
 *  op_dsp —— 采样序列 → 频谱幅值（Operation 层，纯逻辑）
 *
 *  处理链（docs/01 §5，数值口径与 tools/verify_dsp.py 的 read_tone 前半逐行对齐）：
 *      uint16 ADC 码 → 去 DC（减均值）→ 折算输入端 mV → ×BH4 窗
 *        → FFT → mag[k]（k=1..N/2−1；mag[0] 含 DC+Nyquist，写 0 丢弃）
 *
 *  ---- 量纲 ----
 *  全链 mV（输入端口径）：lsb_in_mv = 3.3V/4096/前端增益（verify_dsp 的 LSB_IN），
 *  由 Task 注入（Operation 不读 board_config —— R2，PC 上要能单测）。
 *
 *  ---- 缓冲归调用方 ----
 *  本层**无静态缓冲**：xw_out（加窗时域，op_peak 的点积要用）、fft_buf（FFT
 *  工作块）、mag_out（N/2 个 float）。engine 把前两块放 CCM（board_config 的
 *  32KB 切分），mag 复用 fft_buf 前半即可（原地，见 op_dsp_run）。
 *
 *  ---- FFT 的选择 ----
 *  MCU 侧用 arm_cfft_f32（radix-4，4096=4⁶）：签名就是单缓冲（天生 in-place，
 *  无 rfft_fast 双缓冲语义的纠结），实序列虚部填 0 —— 计算量翻倍（~6ms），
 *  对 2s 判据（余量 130×）无感。host 单测下（OP_DSP_HOST_FFT）换成朴素 DFT
 *  替身 —— CMSIS 的预编译 .a 是 Cortex-M4F 机器码，PC 链不了（顶层 CMakeLists
 *  §CMSIS-DSP 的说明）；窗/归一/mag 组装仍是同一份代码。
 * ==========================================================================*/
#include <stdint.h>

#include "op_dsp_winh4.h" /* OP_DSP_N / OP_DSP_CG / op_dsp_winh4[]（生成表） */

#ifdef __cplusplus
extern "C" {
#endif

/* mag_out 的长度 = N/2（k=1..N/2−1 有效，mag_out[0] 写 0）。 */
#define OP_DSP_MAG_LEN (OP_DSP_N / 2U)

/* 注入采样率与码值折算系数。fs 只用于 op_peak 算 Δf（本层不存它也行，
 * 但"一次注入"比"每次传参"少一条错误传播路径 —— 由 engine 统一 init）。 */
void op_dsp_init(float fs_hz, float lsb_in_mv);

float op_dsp_fs_hz(void);

/* 跑一帧频谱。
 *   raw     ADC 原始码（0..4095，含 ~2048 直流偏置）
 *   xw_out  [N]  输出：加窗后时域（mV）—— op_peak 的单点 DFT 点积输入
 *   fft_buf [N]  工作块：进出皆它（in-place CFFT）
 *   mag_out [N/2] 输出：栅格幅值（可传 fft_buf —— 原地写前半）
 * 返回 0 成功；-1 参数非法（n 不等于 OP_DSP_N / 任一缓冲 NULL）。 */
int op_dsp_run(const uint16_t *raw, uint32_t n, float *xw_out, float *fft_buf,
               float *mag_out);

#ifdef __cplusplus
}
#endif
#endif /* OP_DSP_H */
