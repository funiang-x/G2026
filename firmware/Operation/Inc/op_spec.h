#ifndef OP_SPEC_H
#define OP_SPEC_H
/* ============================================================================
 *  op_spec —— 峰表 → 定性谱页的谱线几何（Operation 层，纯逻辑）
 *
 *  题面对谱线图的要求是"定性"：**根数、频率相对位置、长度比例**。
 *  横轴取 log10 对数刻度、**铺满 10k~500kHz**（docs/01 §12：谱线图横轴必须
 *  取满量程，不能只画到最高峰就截止 —— 否则"相对位置"失真）；
 *  log 轴下相对位置仍真实（单调映射），且谐波族不会全挤在左侧 1/10。
 *  纵轴：最长线 = 最大峰，其余按幅值比例。纯逻辑，PC 可测。
 * ==========================================================================*/
#include <stdint.h>

#include "meas_result.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float x01;    /* 横轴位置，0..1（log10 刻度，10k→500k 铺满） */
    float len01;  /* 线长，0..1（最高峰 = 1，其余按幅值比） */
} op_spec_line_t;

/* 峰表 → 谱线几何。out 容量 ≥ MEAS_MAX_PEAKS。返回线数；-1 参数非法。 */
int op_spec_build(const meas_result_t *r, op_spec_line_t *out, int cap);

#ifdef __cplusplus
}
#endif
#endif /* OP_SPEC_H */
