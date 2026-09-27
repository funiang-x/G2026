#ifndef RTT_CHANNELS_H
#define RTT_CHANNELS_H
/* RTT 通道规划（与 JLinkRTTViewer / pylink 的读取方式一一对应）：
 *   0 = 日志   ：系统日志 + AI 闭环验证标记（[AI_READY] / [AI_FAIL]）
 *   1 = 数据   ：二进制/波形/传感器数据，与日志分开以免刷屏干扰标记检测
 *   2 = 命令   ：上位机或 AI 下发的命令与应答
 *
 * 通道数上限由 SEGGER_RTT_Conf.h 的 SEGGER_RTT_MAX_NUM_UP_BUFFERS 决定，
 * 当前为 3，刚好覆盖 0..2；想加通道必须先改那个上限。 */
#define RTT_CH_LOG 0
#define RTT_CH_DATA 1
#define RTT_CH_CMD 2

#endif /* RTT_CHANNELS_H */
