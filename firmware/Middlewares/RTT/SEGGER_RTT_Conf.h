/*********************************************************************
*                   (c) SEGGER Microcontroller GmbH                  *
*                        The Embedded Experts                        *
*                           www.segger.com                           *
**********************************************************************
*                                                                    *
*        SEGGER RTT * Real Time Transfer for embedded targets        *
*                  https://github.com/SEGGERMicro/RTT                *
*                                                                    *
**********************************************************************

---------------------------END-OF-HEADER------------------------------
Purpose : User configuration file for RTT.
          For available configuration,
          refer to SEGGER_RTT_ConfDefaults.h.

----------------------------------------------------------------------
*/

#ifndef SEGGER_RTT_CONF_H
#define SEGGER_RTT_CONF_H

/*********************************************************************
 *
 *       Defines, configurable
 *
 **********************************************************************
 */

/* 通道规划（BSP/rtt_channels.h）：0=日志 1=数据 2=命令，
 * 用到索引 2 => 上/下行缓冲都必须 >= 3（与 ConfDefaults 默认值一致，
 * 这里显式钉住，防止换配置时通道 2 静默失效）。 */
#define SEGGER_RTT_MAX_NUM_UP_BUFFERS     (3)
#define SEGGER_RTT_MAX_NUM_DOWN_BUFFERS   (3)

#endif
/*************************** End of file ****************************/
