/* ============================================================================
 *  bsp_touch.c —— 电容触摸 FT6336（I2C2）· 见 bsp_touch.h
 *
 *  ---- 取证来源（不是"通用驱动惯例"）----
 *  屏厂例程《TK035F9629_ST7796版/STM32_16位并口_标准库_电容触摸》的
 *  `project/bsp/touch/touch_CTP.c`（在本仓库 docs/ 整理资料下）证实：
 *    · IC = **FT6336**（例程章节标题"电容屏 FT6336芯片驱动"），
 *      7 位 I2C 地址固定 **0x38**：`#define FT6206_ADDR 0x70 // 地址为0x38要移一位`；
 *    · 寄存器地址是 **8 位**（例程发单字节子地址，非 GT911 的 16 位）；
 *    · 轮询读法：读 reg 0x00 起的块，buf[2]（= reg 0x02）低 4 位 = 触点数；
 *      点 1 在 reg 0x03~0x06，每轴 12 位（高 4 位在 0x03/0x05 的低半字节），
 *      每点 +6 字节，最多 5 点；
 *    · **只读不写** —— 例程从不写寄存器，没有 GT911 那种"写 0 清标志"步骤；
 *    · 例程不接 RST/INT —— IC 上电自启，固件无需复位时序
 *      （TP_RST/TP_INT 的接线事实登记在 board_config.h，本层代码不碰）。
 *  历史备注：2026-09-28 前本文件按 GT911（16 位寄存器 @0x5D）实现，
 *  是当时"屏是电容触屏"之外的另一处错误假设；取证结论见项目 README。
 *
 *  ⚠️ 未上板验证：自检与坐标方向要在实物上过一遍（见 bsp_touch.h）。
 * ==========================================================================*/
#include "bsp_touch.h"

#include "board_config.h"
#include "log.h"

#include "i2c.h" /* CubeMX 生成的 hi2c2（Device 层允许依赖 Core 的句柄） */
#include "stm32f4xx_hal.h"

/* ---- FT6336 寄存器（8 位地址；出处见文件头）---- */
#define FT6336_REG_TD_STATUS 0x02U /* 低 4 位 = 触点数 */
#define FT6336_REG_POINT1 0x03U    /* 点 1：第一轴高/低、第二轴高/低 4 字节 */

#define FT6336_I2C_TIMEOUT_MS 100U

/* HAL 的地址要 7 位地址左移 1 位 */
#define FT6336_HAL_ADDR ((uint16_t)(BOARD_TOUCH_I2C_ADDR << 1))

static int s_ready = 0;

/* ------------------------------------------------------------------ 内部 */
static int ft6336_read(uint8_t reg, uint8_t *buf, uint16_t len) {
    return (HAL_I2C_Mem_Read(&hi2c2, FT6336_HAL_ADDR, reg, I2C_MEMADD_SIZE_8BIT, buf, len,
                             FT6336_I2C_TIMEOUT_MS) == HAL_OK)
               ? 0
               : -1;
}

/* ------------------------------------------------------------------ 对外 */
int bsp_touch_init(void) {
    uint8_t status = 0;

    /* 没有复位时序可做：IC 上电自启（屏厂例程不接 RST/INT，TP_RST 已由
     * .ioc 初始为高）。初始化 = 探测一次总线。 */
    if (ft6336_read(FT6336_REG_TD_STATUS, &status, 1) != 0) {
        LOG_W("[TOUCH] FT6336 应答失败（I2C 不通？地址不是 0x%02X？SDA/SCL 没上拉？）",
              BOARD_TOUCH_I2C_ADDR);
        return -1;
    }
    s_ready = 1;
    LOG_I("[TOUCH] FT6336 在线 @0x%02X（TD_STATUS=%02X）", BOARD_TOUCH_I2C_ADDR, status);
    return 0;
}

int bsp_touch_self_check(void) {
    uint8_t status = 0;

    if (ft6336_read(FT6336_REG_TD_STATUS, &status, 1) != 0) {
        LOG_W("[SELFTEST] FT6336 读不到 TD_STATUS（I2C2 不通 / 地址不对 / 没上拉）");
        return -1;
    }
    /* 判据：地址 0x38 有 ACK、寄存器读得通。
     * ⚠️ FT6336 没有 GT911 那种 ASCII 产品 ID 寄存器，"读得到"就是全部判据。 */
    LOG_I("[SELFTEST] FT6336 ok, TD_STATUS=%02X（低 4 位 = 触点数）", status);
    return 0;
}

int bsp_touch_read(bsp_touch_point_t *pt) {
    uint8_t status = 0;
    uint8_t buf[4];

    if (pt == NULL || !s_ready) {
        return -1;
    }

    if (ft6336_read(FT6336_REG_TD_STATUS, &status, 1) != 0) {
        return -1;
    }
    /* 低 4 位 = 触点数，0 = 全部抬起。FT6336 读走即刷新，无"清标志"写操作
     * （屏厂例程只读不写）。 */
    if ((status & 0x0FU) == 0U) {
        return 0;
    }

    if (ft6336_read(FT6336_REG_POINT1, buf, 4) != 0) {
        return -1;
    }
    {
        /* 0x03/0x04 = 第一轴，0x05/0x06 = 第二轴（12 位，高位在低半字节）。
         * 屏厂例程实测：第一轴是屏的**短边**、第二轴是**长边** —— 与本屏
         * 320×480 竖屏一致，直接映射。
         * ⚠️ 未上板验证：若点屏发现镜像/上下颠倒，只改下面两行（交换或取反），
         *    不要去动 bsp_lcd / MADCTL —— 那会把问题变成两个来源。 */
        uint32_t x_raw = (uint32_t)((buf[0] & 0x0FU) << 8) | buf[1];
        uint32_t y_raw = (uint32_t)((buf[2] & 0x0FU) << 8) | buf[3];

        uint32_t x = x_raw;
        uint32_t y = y_raw;

        if (x >= BOARD_TOUCH_WIDTH) {
            x = BOARD_TOUCH_WIDTH - 1U;
        }
        if (y >= BOARD_TOUCH_HEIGHT) {
            y = BOARD_TOUCH_HEIGHT - 1U;
        }
        pt->x = (uint16_t)x;
        pt->y = (uint16_t)y;
    }
    return 1;
}
