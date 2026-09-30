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
 *  工作块，**2N**）、mag_out（N/2 个 float）。engine 把 fft_buf 放 CCM
 *  （board_config 的 32KB 切分），xw_out 放 SRAM1（2026-09-28：fft_buf 要 2N，
 *  CCM 装不下两块），mag 复用 fft_buf 前半即可（原地，见 op_dsp_run）。
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

/* 把**实序列**装配成 CFFT 需要的**交织复数**缓冲：
 *      cfft_buf[2i] = xw[i]（实部）    cfft_buf[2i+1] = 0（虚部）
 *
 * ★ 为什么单独抽成一个函数（2026-09-28）：这一步原先内联在 `op_dsp_run` 里，
 *   属于"只有 MCU 才跑到"的代码 —— 结果它错了（写成连续存放 ⇒ 偶数样本当实部、
 *   奇数样本当虚部），而 host 走朴素 DFT 替身，**全绿也照旧错**。
 *   抽出来放进 Operation 层后，它就能被 PC 单测直接验证（见 S15）。
 *   **本工程"方便 AI 开发"的核心手法：把 MCU 独有的步骤尽量搬进 Operation，
 *   让 PC 单测覆盖它；MCU 侧只留真正无法搬走的那一下（arm_cfft_f32 调用）。**
 *
 * ⚠️ `cfft_buf` 必须能放 **2n** 个 float（CFFT 的官方约定 "size 2*fftLen"）。 */
void op_dsp_pack_real_to_cfft(const float *xw, float *cfft_buf, uint32_t n);

/* 跑一帧频谱。
 *   raw     ADC 原始码（0..4095，含 ~2048 直流偏置）
 *   xw_out  [N]   输出：加窗后时域（mV）—— op_peak 的单点 DFT 点积输入
 *   fft_buf [**2N**] 工作块：CFFT 的交织复数缓冲（p[2i]=实部 / p[2i+1]=虚部）
 *                 ⚠️ 必须是 **2*N** 个 float！`arm_cfft_f32` 的官方约定就是
 *                 "size 2*fftLen"（蝶形索引到 pSrc[2*(fftLen-1)+1]）。给 N 会
 *                 写穿缓冲 —— 2026-09-28 修过一次（原先只给了 N）。
 *   mag_out [N/2] 输出：栅格幅值（可传 fft_buf —— 原地写前半）
 * 返回 0 成功；-1 参数非法（n 不等于 OP_DSP_N / 任一缓冲 NULL）。 */
int op_dsp_run(const uint16_t *raw, uint32_t n, float *xw_out, float *fft_buf,
               float *mag_out);

#ifdef __cplusplus
}
#endif
#endif /* OP_DSP_H */
