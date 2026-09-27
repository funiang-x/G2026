#ifndef BSP_RTC_H
#define BSP_RTC_H
/* bsp_rtc —— 板载 RTC（LSE 32.768kHz + VBAT 掉电保持）。
 * 上层只处理"年月日时分秒"，RTC_TimeTypeDef 这类 HAL 类型挡在本层内。 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 本层自己的时间类型（**不暴露 HAL 的 RTC_TimeTypeDef / RTC_DateTypeDef**）。 */
typedef struct {
    uint16_t year;   /* 完整年份，如 2026 */
    uint8_t month;   /* 1..12 */
    uint8_t day;     /* 1..31 */
    uint8_t weekday; /* 1..7，**1 = 周一**（HAL 的约定，不是 0 = 周日）*/
    uint8_t hour;    /* 0..23 */
    uint8_t minute;  /* 0..59 */
    uint8_t second;  /* 0..59 */
} bsp_rtc_time_t;

/* 检查 RTC 是否已经被设过时间。
 *
 * 为什么要这个：RTC 由 VBAT 供电，**掉电后时间继续走**。所以上电时不能
 * 无脑重设时间（会把已经走准的时间冲掉）。本函数读 backup 寄存器里的
 * 一个魔术字来判断 —— 返回 1 = 配置过（时间可信），0 = 从没配过（值无意义）。
 *
 * ⚠️ 前提是 VBAT 有电。VBAT 断了（拔掉电池且整板无电）连 backup 寄存器也会丢。 */
int bsp_rtc_is_configured(void);

/* 设置时间日期。成功返回 0，参数非法返回 -1。
 * 成功后写入魔术字 ⇒ 之后 `bsp_rtc_is_configured()` 返回 1。 */
int bsp_rtc_set(const bsp_rtc_time_t *t);

/* 读取时间日期。成功返回 0，t 为 NULL 返回 -1。 */
int bsp_rtc_get(bsp_rtc_time_t *t);

#ifdef __cplusplus
}
#endif
#endif /* BSP_RTC_H */
