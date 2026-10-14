/* ============================================================================
 *  op_dsp.c —— 采样序列 → 频谱幅值 · 见 op_dsp.h
 * ==========================================================================*/
#include "op_dsp.h"

#include <math.h>

#ifndef OP_DSP_HOST_FFT
#include "arm_math.h" /* arm_cfft_f32 —— CMSIS-DSP（MCU 侧；host 用替身，见 .h） */

/* 定长 FFT 实例（CMSIS 的 arm_const_structs.o 提供）。
 * ⚠️ 本工程**没有 vendor `arm_const_structs.h`**（`Inc/` 下只有 `arm_math.h`），
 *    但符号确实在 `libarm_cortexM4lf_math.a` 里（`arm-none-eabi-nm` 证实）——
 *    所以在这里显式声明，而不是去猜一个头文件名。 */
extern const arm_cfft_instance_f32 arm_cfft_sR_f32_len4096;

/* 这个 const 实例**只对应 4096 点** —— 改 N 必须同步换实例（否则拿错表，静默算错）。 */
_Static_assert(OP_DSP_N == 4096, "定长 const FFT 实例只对应 4096 点：改 OP_DSP_N 要同步换实例");
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

void op_dsp_pack_real_to_cfft(const float *xw, float *cfft_buf, uint32_t n) {
    if (xw == NULL || cfft_buf == NULL) {
        return;
    }
    /* 逐点交织 + 虚部清零。
     * ⚠️ 绝不能写成 `cfft_buf[i] = xw[i]` —— 那是把样本连续放，等价于
     *    "偶数样本当实部、奇数样本当虚部"，算的根本不是实序列谱。
     *    （2026-09-28 的真实缺陷，PC 侧曾因替身而全绿。） */
    for (uint32_t i = 0; i < n; i++) {
        cfft_buf[2U * i] = xw[i];
        cfft_buf[2U * i + 1U] = 0.0f;
    }
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
        xw_out[i] = mv * op_dsp_winh4_at(i); /* op_peak 的单点 DFT 点积要用，保留 */
    }

    /* ② 装配 CFFT 输入（实序列 → 交织复数 + 虚部清零）。
     * ★ 抽成 `op_dsp_pack_real_to_cfft()` 而不是内联 —— 这样它就能进 PC 单测。
     *   原先内联时，它属于"只有 MCU 跑到"的代码，写错了 PC 也全绿（见 docs/01 §5.1
     *   修正③）。**本工程方便 AI 开发的核心手法：把 MCU 独有的步骤搬进 Operation。** */
    op_dsp_pack_real_to_cfft(xw_out, fft_buf, n);

    /* ③ FFT：实序列走复数 CFFT（虚部已由上面装配成 0，见 op_dsp.h 的选型说明）。
     *    MCU：arm_cfft_f32 单缓冲 in-place，输出 interleaved re/im。
     *    host：朴素 DFT 替身 —— **它读 xw_out 而不是 fft_buf**，因为朴素 DFT 的输出
     *    会覆盖自己的输入（in-place 会破坏后续 bin 的求和），而 fft_buf 正是输出目标。
     *    ⚠️ 所以 host 这条链**不经过 fft_buf 的交织装配** —— 那一步由单测 **S15**
     *       直接验证（测的就是上面那个 `op_dsp_pack_real_to_cfft`）。
     *
     *  ★ 用**定长 const 实例**，不走 `arm_cfft_init_f32()`（2026-09-28 改）：
     *    init 是个 `switch(fftLen)`，为支持 9 种尺寸而 `U` 引用了全部 9 个
     *    `arm_cfft_sR_f32_len*` ⇒ 链接器把 16~2048 点的 twiddle/bitrev 表
     *    全拉进固件。实测**多带 45KB**（ELF 里 FFT 表共 85.3KB，4096 点只需 40.4KB）。
     *    ⚠️ 等价性已由**反汇编证实**：`arm_cfft_init_f32` 对 4096 就是把这个
     *       const 结构体的 4 个字段逐字拷过去（`ldr/ldrd/strh` 三条），无其它副作用；
     *       顺带省掉每帧一次的 init 调用。 */
#ifndef OP_DSP_HOST_FFT
    arm_cfft_f32(&arm_cfft_sR_f32_len4096, fft_buf, 0U, 1U);
#else
    host_dft(xw_out, n, fft_buf);
#endif

    /* ④ mag 原地写前半：mag[k]=|X[k]|。k=0 含 DC（去 DC 后≈0）+ 混叠在第 3 项
     *    口径里随 mag[0] 丢弃（docs/01 §6）—— 统一写 0。 */
    mag_out[0] = 0.0f;
    for (uint32_t k = 1; k < n / 2U; k++) {
        const float re = fft_buf[2U * k];
        const float im = fft_buf[2U * k + 1U];
        mag_out[k] = sqrtf(re * re + im * im);
    }

    return 0;
}
