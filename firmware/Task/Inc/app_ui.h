#ifndef APP_UI_H
#define APP_UI_H
/* ============================================================================
 *  app_ui —— 测量 UI：四页面 + 触摸键控 + 标定流程页（Task 层）
 *
 *  页面（docs/01 §7）：波形（1T/3T 重建曲线）/ 参数（Upp·Urms·f1）/
 *  定性谱（log 轴谱线）/ 谱线表（数值行）。
 *  键控（物理键已取消，全部触摸 —— docs/01 §7 修订）：
 *      · 底部页签：点击切页（切页即触发一次重测）
 *      · 波形页：点画面区切 1T/3T
 *      · 页签条右端 MODE 徽标：点击循环 u_a/u_b/u_b+J（只换标注，算法唯一）
 *      · 参数页 CAL 按钮：**长按**进标定（防误触 —— 标定会改 Flash）
 *  数据：只读 app_engine_result()，按 seq 判断刷新。不碰测量算法。
 *
 *  ⚠️ 全英文标签：LVGL 内置字体无 CJK 字形（lv_conf 只开了 montserrat），
 *     中文标签会渲染成方块。
 *  ⚠️ 数值格式化用**定点放大 + 手拼小数点**（docs/09：printf 浮点支持常被裁）。
 * ==========================================================================*/
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化（建页面 + 挂回调）。在 app_lvgl_init() 之后调（lv_init 已发生）。
 * 返回 0 成功；-1 = LVGL 内存不够（见 app_lvgl_init 的说明）。 */
int app_ui_init(void);

/* 10ms 节拍：检查新结果 → 刷新当前页的数值/曲线。其余页面惰性刷新
 * （切到才刷 —— 测量周期 500ms，四页全刷是无谓的 FSMC 带宽）。 */
void app_ui_poll(uint32_t now_ms);

#ifdef __cplusplus
}
#endif
#endif /* APP_UI_H */
