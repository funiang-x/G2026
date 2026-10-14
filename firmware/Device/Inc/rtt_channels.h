#ifndef RTT_CHANNELS_H
#define RTT_CHANNELS_H
/* RTT 通道规划（与 JLinkRTTViewer / pylink 的读取方式一一对应）：
 *   0 = 日志：系统日志 + AI 闭环验证标记（[AI_READY] / [AI_FAIL]）
 *
 * 目前全工程只用这一个通道。原来的 1=数据 / 2=命令 两个规划位
 * 2026-10-07 删除 —— 上游缓冲是 RAM（每个 ~32B 控制块），没人用的通道
 * 白占；真要加第二通道时在这里加宏、并把 SEGGER_RTT_Conf.h 的
 * SEGGER_RTT_MAX_NUM_UP_BUFFERS（当前 3）一起核对。 */
#define RTT_CH_LOG 0

#endif /* RTT_CHANNELS_H */
