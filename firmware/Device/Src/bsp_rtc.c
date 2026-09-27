/* bsp_rtc —— 板载 RTC 的实现。
 *
 * ⚠️ **不重复调 `HAL_RTC_Init()`** —— 那已经由 CubeMX 生成的
 *    `Core/Src/rtc.c` 的 `MX_RTC_Init()` 做了（时钟源 LSE 在
 *    `HAL_RTC_MspInit` 里配）。本文件只做"设置 / 读取 / 是否配置过"。
 *
 * ⚠️ HAL 的一个必踩坑：**`HAL_RTC_GetTime()` 会锁住影子寄存器**
 *    （防止读的过程中时间跳变），必须紧接着调 `HAL_RTC_GetDate()` 解锁。
 *    只读 time 不读 date ⇒ 下一次读拿到的是错值，而且**不报错**。
 *    本文件的 `bsp_rtc_get()` 把两者绑在一起，让调用方没机会踩。
 *
 * 参考：资料包 `第03章/代码例程/015RTC时钟实验(可做掉电实验).zip`（寄存器版）。 */
#include "bsp_rtc.h"

#include "board_config.h"
#include "log.h"
#include "rtc.h" /* hrtc —— CubeMX 生成 */
#include "stm32f4xx_hal.h"

/* backup 寄存器 0 存"已配置"魔术字。VBAT 有电时掉电不丢。 */
#define RTC_BKP_INDEX RTC_BKP_DR0
#define RTC_CONFIGURED_MAGIC 0x32F2U

int bsp_rtc_is_configured(void) {
    return (HAL_RTCEx_BKUPRead(&hrtc, RTC_BKP_INDEX) == RTC_CONFIGURED_MAGIC) ? 1 : 0;
}

int bsp_rtc_set(const bsp_rtc_time_t *t) {
    if (t == NULL) {
        return -1;
    }
    if (t->year < 2000U || t->month < 1U || t->month > 12U || t->day < 1U ||
        t->day > 31U || t->weekday < 1U || t->weekday > 7U || t->hour > 23U ||
        t->minute > 59U || t->second > 59U) {
        LOG_E("RTC 时间参数非法：%04u-%02u-%02u %02u:%02u:%02u weekday=%u", t->year,
              t->month, t->day, t->hour, t->minute, t->second, t->weekday);
        return -1;
    }

    RTC_TimeTypeDef st = {0};
    st.Hours = t->hour;
    st.Minutes = t->minute;
    st.Seconds = t->second;
    st.TimeFormat = RTC_HOURFORMAT12_AM; /* 24 小时制下被忽略，显式写只为不留悬念 */
    st.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
    st.StoreOperation = RTC_STOREOPERATION_RESET;
    if (HAL_RTC_SetTime(&hrtc, &st, RTC_FORMAT_BIN) != HAL_OK) {
        LOG_E("HAL_RTC_SetTime 失败");
        return -1;
    }

    RTC_DateTypeDef sd = {0};
    sd.Year = (uint8_t)(t->year % 100U); /* HAL 只收两位年 */
    sd.Month = t->month;
    sd.Date = t->day;
    sd.WeekDay = t->weekday;
    if (HAL_RTC_SetDate(&hrtc, &sd, RTC_FORMAT_BIN) != HAL_OK) {
        LOG_E("HAL_RTC_SetDate 失败");
        return -1;
    }

    HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_INDEX, RTC_CONFIGURED_MAGIC);
    LOG_I("RTC 已设置：%04u-%02u-%02u %02u:%02u:%02u", t->year, t->month, t->day,
          t->hour, t->minute, t->second);
    return 0;
}

int bsp_rtc_get(bsp_rtc_time_t *t) {
    if (t == NULL) {
        return -1;
    }
    RTC_TimeTypeDef st = {0};
    RTC_DateTypeDef sd = {0};

    /* ⚠️ 这两个调用**必须成对**：GetTime 锁影子寄存器，GetDate 解锁。
     *    顺序也不能反。理由见文件头。 */
    if (HAL_RTC_GetTime(&hrtc, &st, RTC_FORMAT_BIN) != HAL_OK) {
        return -1;
    }
    if (HAL_RTC_GetDate(&hrtc, &sd, RTC_FORMAT_BIN) != HAL_OK) {
        return -1;
    }

    t->year = (uint16_t)(2000U + sd.Year); /* 两位年 + 2000（板子活在 20xx 年）*/
    t->month = sd.Month;
    t->day = sd.Date;
    t->weekday = sd.WeekDay;
    t->hour = st.Hours;
    t->minute = st.Minutes;
    t->second = st.Seconds;
    return 0;
}
