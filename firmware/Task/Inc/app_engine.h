#ifndef APP_ENGINE_H
#define APP_ENGINE_H
/* ============================================================================
 *  app_engine —— 测量编排：启动采集 → 等 DMA → DSP 链 → 发布结果（Task 层）
 *
 *  ---- 为什么是"10ms 态机"而不是独立 FreeRTOS 任务（docs/01 §8 的修订）----
 *  §8 自己论证过"拆任务换不来并行度，只增一次队列交接的竞态面"——这条对
 *  engine/ui 分离同样成立：单次测量全链 ~40ms ≪ 2s 判据（余量 50×），DSP 期间
 *  LVGL 的一拍从 10ms 变 40ms 只是掉一帧，肉眼无感。态机版**没有队列、没有
 *  信号量、没有 FromISR、没有第二份任务栈** —— 每一样都是竞态/阻塞的候选来源。
 *  （§8 原表保留在 docs/01，修订理由见本文与 docs/01 §8 末尾。）
 *
 *  ---- 内存落点 ----
 *  s_adc_buf 在 SRAM1（DMA 可达）；s_xw/s_fft 两块 f32 在 CCM 低 32KB
 *  （section .ccmram，链接脚本有越界 ASSERT；CCM 不能被 DMA 摸 —— 它们不碰 DMA）。
 *
 *  ---- 标定流程（docs/01 §11）----
 *  UI 逐步驱动：cal_begin → 显示频点提示 → 人调信号源 → cal_step（同步测一次，
 *  ~40ms，LVGL 回调里可接受）→ …17 次… → cal_save（擦+写 Flash）。表在 RAM 里
 *  即时生效（下一次测量就带标定），save 失败只影响持久化。
 * ==========================================================================*/
#include <stdint.h>

#include "meas_result.h"
#include "op_cal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化：注入 DSP 参数 + 从 Flash 载入标定表（坏表/无表 = 不标定，照常测）。
 * ⚠️ 在 bsp_adc_init()（app_init 里已调）之后、调度器启动前调。 */
void app_engine_init(void);

/* 10ms 节拍：非阻塞态机。到周期（APP_MEAS_PERIOD_MS）或被 remeasure 触发时
 * 启动一帧；DMA 完成后的那一拍里跑 DSP 链（~40ms，LVGL 该拍掉一帧，无感）。
 * now_ms 来自组合根（与 LVGL 同源的 bsp_tick），R2/R3 的时间口径。 */
void app_engine_poll(uint32_t now_ms);

/* 立即重测一次（UI 切页时调，docs/01 §7）。 */
void app_engine_remeasure(void);

/* 最近一次结果（seq 判断新旧）。永不返回 NULL。 */
const meas_result_t *app_engine_result(void);

/* 场景标注（MODE 切换，算法唯一 —— docs/01 §7）。 */
void    app_engine_set_scene(uint8_t scene);
uint8_t app_engine_scene(void);

/* ---- 标定流程（UI 驱动，见文件头）---- */
void  app_engine_cal_begin(void);            /* 建空表 + 回到第 0 点 */
int   app_engine_cal_step(void);             /* 测当前点（同步 ~40ms），返回剩余点数，-1=失败 */
int   app_engine_cal_index(void);            /* 当前点序号 0..16（UI 进度） */
float app_engine_cal_freq(int idx);          /* 第 idx 点的提示频率（Hz） */
int   app_engine_cal_points(void);           /* 总点数（17） */
int   app_engine_cal_save(void);             /* 擦扇区 + 写 Flash；0=成功 */
int   app_engine_cal_loaded(void);           /* 1 = 开机时载到了有效表（UI 提示用） */

#ifdef __cplusplus
}
#endif
#endif /* APP_ENGINE_H */
