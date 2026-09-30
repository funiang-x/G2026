#ifndef APP_ENGINE_H
#define APP_ENGINE_H
/* ============================================================================
 *  app_engine —— 测量编排：启动采集 → 等 DMA → DSP 链 → 发布结果（Task 层）
 *
 *  ---- 它是**独立 FreeRTOS 任务**（2026-09-28 改）----
 *  旧版是"10ms 轮询态机"（engine 与 ui 挤在同一拍，靠"DSP 拆两拍"绕开帧抖动）。
 *  改成任务后，三处硬伤一次消掉：
 *    · **采集完成不再轮询**：DMA 中断给二值信号量，任务 `bsp_adc_wait_frame()`
 *      阻塞等待 —— 等待期让出 CPU，且没有"最多白等一拍(10ms)"的延迟；
 *    · **重测即刻响应**：`app_engine_remeasure()` 走任务通知，切页/换 1T-3T
 *      不必等下一个节拍；
 *    · **结果跨任务安全**：发布走 1 深 mailbox 队列（xQueueOverwrite），
 *      UI 侧 xQueuePeek —— 不会读到写一半的结构（旧的单例靠"同任务"侥幸成立）。
 *
 *  ⚠️ 多任务没建起来时（`app_tasks_init()` 失败），组合根会退回旧形态：
 *     每 10ms 调 `app_engine_poll()`。两条路径共用 `engine_measure_once()`。
 *
 *  ---- 内存落点 ----
 *  s_adc_buf 在 SRAM1（DMA 可达）；s_xw/s_fft 两块 f32 在 CCM 低 32KB
 *  （section .ccmram，链接脚本有越界 ASSERT；CCM 不能被 DMA 摸 —— 它们不碰 DMA）。
 *
 *  ---- ⚠️ 为什么采集侧**不做**乒乓双缓冲（诚实标注）----
 *  本机是"按需采一帧"的门控采集：生产者 2.048ms/帧、消费者(DSP) ~13ms/帧，
 *  快 6 倍 ⇒ 双缓冲只会让新帧盖掉还没处理完的旧帧，收益为零、还白吃 8KB。
 *  真正需要双缓冲的是"双帧变频识别 u_J"（两个 fs 各采一帧交叉比对，残留
 *  1.31mV→~0.1mV）—— 那是 DSP 文档级的改动，**当前未实现**，见 docs/02 优化方案。
 *
 *  ---- 标定流程（docs/01 §11）----
 *  UI 逐步驱动：cal_begin → 显示频点提示 → 人调信号源 → cal_step（同步测一次）
 *  → …17 次… → cal_save（擦+写 Flash）。表在 RAM 里即时生效。
 *  ⚠️ 遗留：cal_step 目前在 UI 任务里**同步**跑（阻塞 ~15ms），标定时界面短暂停顿。
 *     彻底不卡 UI 需改成"engine 收请求 / UI 轮询 `app_engine_cal_index()`"的异步版。
 * ==========================================================================*/
#include <stdint.h>

#include "meas_result.h"
#include "op_cal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化：注入 DSP 参数 + 建结果 mailbox + 从 Flash 载入标定表
 * （坏表/无表 = 不标定，照常测）。
 * ⚠️ 在 bsp_adc_init()（app_init 里已调）之后、调度器启动前调。 */
void app_engine_init(void);

/* 测量任务体（由 `app_tasks_init()` 用 xTaskCreateStatic 建）。
 * 阻塞在 xTaskNotifyWait 上：周期到（APP_MEAS_PERIOD_MS）或被 remeasure
 * 唤醒时，跑一帧完整测量并发布。**不返回**。 */
void app_engine_task(void *arg);

/* 降级路径入口：多任务没建起来时由组合根每 10ms 调一次（旧"轮询态机"形态）。
 * 正常路径（任务已建）下**不要**调 —— 会与任务重复测量。 */
void app_engine_poll(uint32_t now_ms);

/* 立即重测一次（UI 切页时调，docs/01 §7）：给测量任务发一次通知。 */
void app_engine_remeasure(void);

/* ---- DSP 链自检（上电跑一次，Task 层）----
 * 合成一帧**已知单音**（整数周期，落在第 k0 个 bin）→ 跑完整 DSP 链 →
 * 校验谱峰**频率与幅值**都对。
 *
 * ★ 为什么必须有它（2026-09-28 加）：PC 单测（`fw.py test`）里的 FFT 是
 *   **朴素 DFT 替身**（`OP_DSP_HOST_FFT`），**不覆盖** MCU 上的 `arm_cfft_f32`
 *   路径 —— FFT 实例/表拿错、装配顺序错，PC 上完全看不出来。这条自检把那个
 *   缺口堵上：几毫秒，上电即跑，判据是"**谱峰落在预期位置且幅值对**"。
 * ⚠️ 必须在 `app_engine_init()`（注入 fs/lsb）之后、**调度器启动前**调 ——
 *    它会占用 engine 的静态缓冲，此时还没有任务在跑，无竞争。
 * 返回 0 = 通过；-1 = 失败（已打日志，指出是频率还是幅值超差）。 */
int app_engine_selftest(void);

/* 最近一次结果（从 mailbox 取快照，seq 判断新旧）。永不返回 NULL。 */
const meas_result_t *app_engine_result(void);

/* 1 = 采样窗进行中（engine 正在等 DMA 搬满）。
 * UI 任务据此**推迟** FSMC 刷新 —— 机制性消除"采集瞬间 LCD 总线噪声灌进 AFE"
 * 这一最大板内干扰源（硬件方案 02 §6 O1）。 */
int app_engine_acquiring(void);

/* 场景标注（MODE 切换，算法唯一 —— docs/01 §7）。 */
void    app_engine_set_scene(uint8_t scene);
uint8_t app_engine_scene(void);

/* ---- 标定流程（UI 驱动，见文件头）---- */
void  app_engine_cal_begin(void);            /* 建空表 + 回到第 0 点 */
int   app_engine_cal_step(void);             /* 测当前点（同步），返回剩余点数，-1=失败 */
int   app_engine_cal_index(void);            /* 当前点序号 0..16（UI 进度） */
float app_engine_cal_freq(int idx);          /* 第 idx 点的提示频率（Hz） */
int   app_engine_cal_points(void);           /* 总点数（17） */
int   app_engine_cal_save(void);             /* 擦扇区 + 写 Flash；0=成功 */
int   app_engine_cal_loaded(void);           /* 1 = 开机时载到了有效表（UI 提示用） */

#ifdef __cplusplus
}
#endif
#endif /* APP_ENGINE_H */
