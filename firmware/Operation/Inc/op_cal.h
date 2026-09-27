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

/* CRC32（反射，0xEDB88320）—— 只给本模块序列化用，导出是为了让单测能
 * 独立复算校验（不依赖 pack 的内部布局假设）。 */
uint32_t op_cal_crc32(const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif /* OP_CAL_H */
