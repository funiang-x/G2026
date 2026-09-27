/* ============================================================================
 *  op_dsp.c —— 采样序列 → 频谱幅值 · 见 op_dsp.h
 * ==========================================================================*/
#include "op_dsp.h"

#include <math.h>

#ifndef OP_DSP_HOST_FFT
#include "arm_math.h" /* arm_cfft_f32 —— CMSIS-DSP（MCU 侧；host 用替身，见 .h） */
#endif

static float s_fs_hz = 0.0f;
static float s_lsb_mv = 0.0f;

void op_dsp_init(float fs_hz, float lsb_in_mv) {
    s_fs_hz = fs_hz;
    s_lsb_mv = lsb_in_mv;
}

float op_dsp_fs_hz(void) {
    return s_fs_hz;
}

/* ------------------------------------------------------------------ host 替身
 * 朴素 DFT（O(N²)）：与 arm_cfft_f32 输出同构（interleaved re/im 到 k=N/2）。
 * 只在 PC 单测编（Test/CMakeLists 定义 OP_DSP_HOST_FFT）—— 上板验证时
 * MCU 走的是 CMSIS 库路径，替身不参与。 */
#ifdef OP_DSP_HOST_FFT
static void host_dft(const float *x, uint32_t n, float *out_reim) {
    const uint32_t half = n / 2U;
    for (uint32_t k = 0; k <= half; k++) {
        double re = 0.0;
        double im = 0.0;
        const double w = 2.0 * 3.14159265358979323846 * (double)k / (double)n;
        for (uint32_t i = 0; i < n; i++) {
            re += (double)x[i] * cos(w * (double)i);
            im -= (double)x[i] * sin(w * (double)i);
        }
        out_reim[2U * k] = (float)re;
        out_reim[2U * k + 1U] = (float)im;
    }
}
#endif

int op_dsp_run(const uint16_t *raw, uint32_t n, float *xw_out, float *fft_buf,
               float *mag_out) {
    if (raw == NULL || xw_out == NULL || fft_buf == NULL || mag_out == NULL ||
        n != (uint32_t)OP_DSP_N || s_fs_hz <= 0.0f || s_lsb_mv <= 0.0f) {
        return -1;
    }

    /* ① 去 DC + 折算 mV + 加窗 —— 一遍循环（verify_dsp：x−mean 再 ×W4）。
     *    均值用整数累加（码值本来是整数，4096 项精确无舍入）。 */
    uint32_t sum = 0U;
    for (uint32_t i = 0; i < n; i++) {
        sum += raw[i];
    }
    const float mean = (float)((double)sum / (double)n);

    for (uint32_t i = 0; i < n; i++) {
        const float mv = ((float)raw[i] - mean) * s_lsb_mv;
        xw_out[i] = mv * op_dsp_winh4[i];
        fft_buf[i] = xw_out[i]; /* FFT 工作块从加窗数据出发（xw_out 保留给 op_peak） */
    }

    /* ② 实序列 FFT：虚部填 0 走复数 CFFT（见 op_dsp.h 的选型说明）。
     *    MCU：arm_cfft_f32 单缓冲 in-place，输出 interleaved re/im。
     *    host：朴素 DFT **从 xw_out 读**（与 fft_buf 内容相同）—— 朴素 DFT
     *    的输出会覆盖自己的输入，in-place 会破坏后续 bin 的求和。 */
#ifndef OP_DSP_HOST_FFT
    {
        static arm_cfft_instance_f32 S; /* init 只写 instance，无缓冲 —— static 免重复建 */
        if (arm_cfft_init_f32(&S, (uint16_t)n) != ARM_MATH_SUCCESS) {
            return -1;
        }
        arm_cfft_f32(&S, fft_buf, 0U, 1U);
    }
#else
    host_dft(xw_out, n, fft_buf);
#endif

    /* ③ mag 原地写前半：mag[k]=|X[k]|。k=0 含 DC（去 DC 后≈0）+ 混叠在第 3 项
     *    口径里随 mag[0] 丢弃（docs/01 §6）—— 统一写 0。 */
    mag_out[0] = 0.0f;
    for (uint32_t k = 1; k < n / 2U; k++) {
        const float re = fft_buf[2U * k];
        const float im = fft_buf[2U * k + 1U];
        mag_out[k] = sqrtf(re * re + im * im);
    }

    return 0;
}
