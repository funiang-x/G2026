#ifndef BSP_TOUCH_H
#define BSP_TOUCH_H
/* ============================================================================
 *  bsp_touch —— 电容触摸 **FT6336**（FT5x06 系，走 I2C2：PB10=SCL / PB11=SDA）
 *
 *  ---- 器件事实（取证：屏厂例程 touch_CTP.c，出处见 bsp_touch.c 文件头）----
 *      · 7 位 I2C 地址固定 **0x38**（不像 GT911 那样靠复位时的 INT 电平选地址）
 *      · 寄存器地址 **8 位**；TD_STATUS(0x02) 低 4 位 = 触点数
 *      · 只读轮询即可：无需写寄存器、无需复位时序（例程连 RST/INT 都不接）
 *
 *  ---- 为什么用轮询而不是中断 ----
 *  LVGL 的 indev 本来就是周期性轮询的；FT6336 的 INT 是 **IC 的输出脚**，
 *  走中断要接 EXTI、还要在 ISR 里只投事件（规矩 3），收益只是省几次 I2C 读
 *  —— 不值。**将来要省电再改。**
 *
 *  ---- ⚠️ 未上板验证 ----
 *  自检判据 = "0x38 有 ACK、寄存器读得到"（FT6336 没有 GT911 那种 ASCII 产品 ID）；
 *  坐标的镜像/颠倒方向要在实物上点过才知道（bsp_touch_read 里标了改哪两行）。
 * ==========================================================================*/
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t x; /* 已映射到屏坐标（0 ~ BOARD_TOUCH_WIDTH-1） */
    uint16_t y;
} bsp_touch_point_t;

/* 初始化：探测 0x38 的 I2C 应答。
 * 返回 0 = 成功；-1 = I2C 不通（没上拉 / 地址不对 / 屏没供电）。
 * ⚠️ 依赖 `MX_I2C2_Init()` 已经跑过（main 里调）。 */
int bsp_touch_init(void);

/* 自检：读 TD_STATUS 并打印（能读通 = 总线与器件都在）。
 * 返回 0 = 读到了；-1 = 读不到。 */
int bsp_touch_self_check(void);

/* 读一次触摸。
 * 返回 1 = 有按下，`pt` 已填好（屏坐标）；
 * 返回 0 = 当前无触摸；
 * 返回 -1 = I2C 出错。 */
int bsp_touch_read(bsp_touch_point_t *pt);

#ifdef __cplusplus
}
#endif
#endif /* BSP_TOUCH_H */
