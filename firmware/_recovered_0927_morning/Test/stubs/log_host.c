/* ============================================================================
 *  log_host.c —— `log_put()` 的 **PC 侧实现**（stub）
 *
 *  依赖注入的另一半（见 Common/Inc/log.h 的文件头）：
 *      MCU 构建（固件） → Device/Src/bsp_log.c  提供 log_put()（RTT/UART + 互斥锁）
 *      PC 构建（单测）  → 本文件                   提供 log_put()（printf 到控制台）
 *
 *  ⇒ 同一份 Operation 源码，在板子上和在电脑上都能链接通过。
 *    这就是"业务逻辑能在电脑上验证"在链接层面的落地。
 * ==========================================================================*/
#include <stdio.h>

#include "log.h"

void log_put(log_level_t lvl, const char *buf, unsigned len) {
    static const char *tag = "EWID";
    int idx = (int)lvl;
    if (idx < 0 || idx > 3) {
        idx = 3;
    }
    /* PC 侧不做级别过滤：单测里**看到全部输出**比"安静"更有用。
     * 过滤阈值属于产品行为，在 MCU 侧由 bsp_log_set_level() 注入。 */
    (void)fprintf(stdout, "[%c] %.*s\n", tag[idx], (int)len, buf);
    (void)fflush(stdout);
}
