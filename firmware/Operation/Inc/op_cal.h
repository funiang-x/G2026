#ifndef OP_CAL_H
#define OP_CAL_H
/* ============================================================================
 *  op_cal —— 标定表：建表数据结构、log 域插值、序列化 + CRC（Operation 层）
 *
 *  依据 docs/01 §11：题面允许"携带自用信号发生器 + 标称值" ⇒ 全部 LTI 误差
 *  （增益/分压/通带/孔径/相移）可被赛前一次标定吃掉（复算：残差 0.023mV）。
 *
 *  表 = 17 个对数频点（10k→500k）上的 G(f)（幅值修正系数）与 P(f)（相位修正，
 *  rad）。使用：A/=G(f)，φ−=P(f)（verify_dsp 的 cal()）。
 *  ---- 分工 ----
 *  本层只管"表长什么样、怎么插值、怎么进出 Flash"；
 *  "怎么测出表"是 engine+UI 的流程（逐频点测一次读数），"写 Flash"是组合根
 *  经 op_flash 拆段 + bsp_spi 落盘 —— 本层不碰硬件（R2）。
 * ==========================================================================*/
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 17 点对数间隔 10k→500k：f_k = 10k·(50)^(k/16)（verify_dsp 的 logspace）。
 * 频段是题面定死的（第 2/3 项 10k~500kHz），不做参数。 */
#define OP_CAL_POINTS 17
#define OP_CAL_F_LO 10000.0f
#define OP_CAL_F_HI 500000.0f

/* 未标定/缺表时 UI 仍要能显示测量值 ⇒ 调用方传 NULL 即"不修正"。 */
typedef struct {
    float f[OP_CAL_POINTS]; /* 频点（Hz），严格递增 */
    float g[OP_CAL_POINTS]; /* 幅值修正：读数 = 真值×g ⇒ 修正 A/=g */
    float p[OP_CAL_POINTS]; /* 相位修正（rad）：φ_true = φ_measured − p */
} op_cal_table_t;

/* 填默认频点表（对数间隔）——建表流程开始前调，g/p 留给流程填。 */
void op_cal_default_freqs(op_cal_table_t *t);

/* ★ 写入第 idx 个标定点（g = 幅值修正，p = 相位修正）。**必须用它，不要直接写
 * t->p[idx]** —— 相位要在这里展开（unwrap），否则查表会插出完全错误的值。
 *
 * 为什么必须展开（2026-10-07 实测，证据 `_work/hwsw-chain/cal_phase_wrap.py`）：
 *   逐点测出来的相位是 `atan2` 的主值（再 +π/2 ⇒ 落在 (-π/2, 3π/2]）。而 AFE 的
 *   相移是 8 阶 LC 梯形（fc≈644 kHz）⇒ **带内会穿过 ±180°**：本板实测在
 *   306.6 kHz 存 -75.3°，到 391.6 kHz 存 +234.4°（同一支连续相位 -165.3°→-215.6°）。
 *   而 `op_cal_lookup` 是"在 log-f 上线性插值"⇒ 这两个点之间会从 -75° 线性扫到
 *   +234°，插出 **最差 179.5° 的相位误差 @347 kHz**（>10° 的区间占带内 5.9%）。
 *   后果不是"相位读数难看"：`op_meas_build` 用各分量相位做 sin 重建取 max−min，
 *   实测 160 kHz 基波 + 二次谐波（320 kHz 落在坏区间）⇒ **Upp 偏 -18.95 mV**
 *   （题面判据 ±5 mV）。
 * 展开规则：把 p 平移到离**上一个点**最近的等价主值（相邻点真实相移 <180°，
 * 这是 17 点对数密度的保证）。idx==0 直接存主值。
 * ⚠️ 旧表（未展开）若已写进 Flash，读回来仍会插错 —— 重新标定一次即可。 */
void op_cal_set_point(op_cal_table_t *t, int idx, float g, float p);

/* log 域线性插值取 (g,p)。f 越界取端点值（与 verify_dsp 的 np.interp 同为钳位）。
 * 表为 NULL 时不改 g 与 p（调用方初始化为 1/0 即"不修正"）。 */
void op_cal_lookup(const op_cal_table_t *t, float freq_hz, float *g, float *p);

/* ---- Flash 布局（字节流，小端）----
 *   offset 0  : magic "GCAL"（4B）
 *   offset 4  : version = 1（1B）
 *   offset 5  : 保留（3B，全 0）
 *   offset 8  : 17 × (f,g,p) = 204B
 *   offset 212: CRC32（前 212 字节，多项式 0xEDB88320 反射 —— 与常见 zlib 一致）
 *   合计 216B —— 一页（256B）放得下，写/读都不用跨页。 */
#define OP_CAL_BLOB_SIZE 216U
#define OP_CAL_MAGIC 0x4C414347U /* "GCAL" 小端 */

int op_cal_pack(const op_cal_table_t *t, uint8_t *buf, size_t cap);
int op_cal_unpack(const uint8_t *buf, size_t len, op_cal_table_t *t);

#ifdef __cplusplus
}
#endif
#endif /* OP_CAL_H */
