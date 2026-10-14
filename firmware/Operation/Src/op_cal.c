/* ============================================================================
 *  op_cal.c —— 标定表 · 见 op_cal.h
 * ==========================================================================*/
#include "op_cal.h"

#include <math.h>

void op_cal_default_freqs(op_cal_table_t *t) {
    if (t == NULL) {
        return;
    }
    /* f_k = 10k · 50^(k/16)，k=0..16 —— 两端严格 10k / 500k（与 verify_dsp
     * 的 logspace(log10(10e3), log10(500e3), 17) 一致）。 */
    for (int k = 0; k < OP_CAL_POINTS; k++) {
        const float x = (float)k / (float)(OP_CAL_POINTS - 1);
        t->f[k] = OP_CAL_F_LO * powf(50.0f, x);
    }
}

void op_cal_set_point(op_cal_table_t *t, int idx, float g, float p) {
    if (t == NULL || idx < 0 || idx >= OP_CAL_POINTS) {
        return;
    }
    /* 相位展开（unwrap）：把 p 折到离上一个点最近的主值上。见 op_cal.h 的长说明
     * —— 不展开的话，跨 ±180° 的那个区间查表会插出 180° 量级的错。 */
    if (idx > 0) {
        const float two_pi = 6.28318530717958647692f;
        p -= two_pi * roundf((p - t->p[idx - 1]) / two_pi);
    }
    t->g[idx] = g;
    t->p[idx] = p;
}

void op_cal_lookup(const op_cal_table_t *t, float freq_hz, float *g, float *p) {
    if (t == NULL) {
        return; /* 未标定：保持调用方的 1/0 */
    }

    /* 钳位到端点（np.interp 语义）；区间内 log 域线性插值。
     * g 在 log 域插值（增益是乘性量），p 直接线性（verify_dsp 同款）。 */
    if (freq_hz <= t->f[0]) {
        *g = t->g[0];
        *p = t->p[0];
        return;
    }
    if (freq_hz >= t->f[OP_CAL_POINTS - 1]) {
        *g = t->g[OP_CAL_POINTS - 1];
        *p = t->p[OP_CAL_POINTS - 1];
        return;
    }

    int k = 0;
    while (k < OP_CAL_POINTS - 1 && freq_hz > t->f[k + 1]) {
        k++;
    }
    const float lf = logf(freq_hz);
    const float lf0 = logf(t->f[k]);
    const float lf1 = logf(t->f[k + 1]);
    const float u = (lf - lf0) / (lf1 - lf0); /* 0..1 */

    *g = expf(logf(t->g[k]) + u * (logf(t->g[k + 1]) - logf(t->g[k])));
    *p = t->p[k] + u * (t->p[k + 1] - t->p[k]);
}

/* ---- CRC32（反射 0xEDB88320）----
 * 位算法（无 256 项查表）：216B × 8 位 = 1728 次循环，毫秒不到，不值得为它
 * 挂一张 1KB 表。只给本模块 pack/unpack 用 —— 单测通过"改一个字节 ⇒ unpack
 * 必失败"验证 CRC 有效性，不需要独立复算。 */
static uint32_t crc32_reflect(const uint8_t *buf, size_t len) {
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint32_t)buf[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
        }
    }
    return crc ^ 0xFFFFFFFFU;
}

/* 小端 32 位读写。float 走 union 位型（IEEE-754，F4 与 host 一致），
 * 非 float 的 32 位（CRC）直接移位 —— 两种写法分开，不借道 float。 */
static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)((v >> 8) & 0xFFU);
    p[2] = (uint8_t)((v >> 16) & 0xFFU);
    p[3] = (uint8_t)((v >> 24) & 0xFFU);
}

static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void put_f32(uint8_t *p, float v) {
    union {
        float f;
        uint32_t u;
    } x;
    x.f = v;
    put_u32(p, x.u);
}

static float get_f32(const uint8_t *p) {
    union {
        uint32_t u;
        float f;
    } x;
    x.u = get_u32(p);
    return x.f;
}

int op_cal_pack(const op_cal_table_t *t, uint8_t *buf, size_t cap) {
    if (t == NULL || buf == NULL || cap < OP_CAL_BLOB_SIZE) {
        return -1;
    }
    buf[0] = 'G';
    buf[1] = 'C';
    buf[2] = 'A';
    buf[3] = 'L';
    buf[4] = 1U; /* version */
    buf[5] = 0U;
    buf[6] = 0U;
    buf[7] = 0U;
    for (int k = 0; k < OP_CAL_POINTS; k++) {
        uint8_t *row = &buf[8U + (size_t)k * 12U];
        put_f32(row, t->f[k]);
        put_f32(row + 4U, t->g[k]);
        put_f32(row + 8U, t->p[k]);
    }
    /* CRC 只覆盖前 212B（不含 CRC 域本身），小端写在 212..215。 */
    put_u32(&buf[212], crc32_reflect(buf, 212U));
    return (int)OP_CAL_BLOB_SIZE;
}

int op_cal_unpack(const uint8_t *buf, size_t len, op_cal_table_t *t) {
    if (buf == NULL || t == NULL || len < OP_CAL_BLOB_SIZE) {
        return -1;
    }
    if (buf[0] != 'G' || buf[1] != 'C' || buf[2] != 'A' || buf[3] != 'L' ||
        buf[4] != 1U) {
        return -1; /* 不是本模块的表 / 版本不认 —— 按缺失处理 */
    }
    if (get_u32(&buf[212]) != crc32_reflect(buf, 212U)) {
        return -1; /* CRC 不符：表损坏，等价于"没有标定表" */
    }
    for (int k = 0; k < OP_CAL_POINTS; k++) {
        const uint8_t *row = &buf[8U + (size_t)k * 12U];
        t->f[k] = get_f32(row);
        t->g[k] = get_f32(row + 4U);
        t->p[k] = get_f32(row + 8U);
    }
    return 0;
}
