/* ============================================================================
 *  app_ui.c —— 测量 UI（LVGL 9.5）· 见 app_ui.h
 *
 *  ---- 架构（为什么这样组织）----
 *  480×320 逻辑横屏（`APP_LVGL_SCR_W/H`，坐标一律由它推导、不写死），
 *  **3 页横排滑动**（`lv_tileview`）：0 TIME DOMAIN / 1 FREQUENCY /
 *  2 CALIBRATION。**翻页只有"手指左右滑"一种方式** ——
 *  没有页签、箭头、圆点、页码；切页即 `app_engine_remeasure()`。
 *  splash 是全屏黑底 + 一个 `TAP TO START` 按钮，**点一下才进主界面**。
 *  分页规矩（2026-10-13 三轮定稿）：**参数不单独占页**，且"图形在上、数在它
 *  该在的那一侧"：时域 = 满宽波形 + **底部三连卡 F1/Vpp/Vrms**；频域 = 频谱
 *  + **右栏竖排**各次谐波（频率+幅值）；标定 = 自检 + 标定入口。
 *  周期切换只有**一个**键（键面 = 当前值）；上屏数值**统一 1 位小数**。
 *  THD 与 Period **不上屏**（题面不要求）；THD 仍留在数据层 —— 删它要牵动
 *  `_Static_assert`、S16~S18 用例与协议，收益为零。
 *
 *  ---- 视觉（Apple 发布会风：纯黑底 + 玻璃卡 + 巨型数字 + 单一强调色）----
 *    · 底 `#080B10`、玻璃卡 `#1A2430→#0B0F16`@30% + 1px `#3A4653`@50% + r18
 *    · 巨型数字 36 / 标签 12（同屏最多两档字号）；强调色全屏唯一 `#4CC9F0`
 *    · 24×24 线性图标用 `lv_line` 自绘（不用图片资源）
 *  ⚠️ 右下角是**官方 sysmon 浮层**（FPS/CPU，实测约 120×40）：它与第三张卡
 *    （y 248..308）**有重叠**——这是 2026-10-13 你明确接受的口径（"不怕重合"），
 *    不要再按旧注释"禁放内容"理解。
 *
 *  ---- 内存（这个文件的写法几乎被这一条决定）----
 *  LVGL 池只有 **32KB @CCM**（`board_config.h` 的 `BOARD_LVGL_MEM_SIZE_KB`，
 *  外加 4KB @SRAM1 扩展池），而 LVGL 的**局部样式是"每个对象各存一份属性"**。
 *  实测（PC 侧 `lv_mem_monitor`，同一把尺子；每次开机 `ui_heap_report()` 也打一份）：
 *  基线 12304 B（lv_init + 显示/输入设备）→ 界面建完 34168 B ⇒ **UI 增量 ≈21.4KB /
 *  **65 个对象**（本文件建 65 个，不含 LVGL 自建的 screen + 3 层 + sysmon 浮层）。
 *  ⚠️ 2026-10-14 八轮订正：旧注释里"27.5KB / 80 个对象"是**一轮那版 4 页界面**的
 *    数字（`board_config.h` 里还有一份同样过期的），与本文件的实测互斥 ⇒ 只认上面这一份。
 *  单价：**一个 label ≈322 B，一个挂共享样式的裸 obj ≈130 B** ⇒ "再加一行读数"
 *  是 KB 级的账，动手前先看池。两条硬规矩：
 *    ① **同款一律走共享样式**（`styles_init()` 里那 **14** 张表），对象只挂指针；
 *    ② `lv_conf.h` 里 **`LV_USE_THEME_DEFAULT = 0`** —— 界面全手绘，主题那份
 *       基线（~3.6KB）与每对象的主题样式条目都是白付的账。
 *  ⇒ 改这个文件时**不要再写 `lv_obj_set_style_*` 的散装属性**，加共享样式。
 *
 *  ---- 动效（2026-10-14 八轮：**三个动画全部取消**）----
 *    · 波形：单层 2px `lv_line`；每帧**变化检测**（所有列 ≤1px 就不重画、不脏区）
 *    · 谐波柱：直写高度（回调同改 height 与 y，不做 300ms 生长）
 *    · 巨型数字：直接 `set_text`（`roll_value`，不做 240ms 滚入）
 *  为什么全删：数据率 5~10Hz，而动画 240~300ms ⇒ 动画永远被下一帧数据打断，
 *  剩下的只有"按重绘节拍持续重绘"（柱子实测 ≈0.86 Mpx/s，与整个波形区同量级）。
 *  判据与机理见 `roll_value` 上方的长注释（六轮删数字动画、八轮删柱动画）。
 * ==========================================================================*/
#include "app_ui.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "app.h" /* app_selftest_snapshot()：页 2 的上电自检 8 项 */
#include "app_engine.h"
#include "app_lvgl.h" /* app_lvgl_flush_kpx_s()：页 2 带宽读数 */
#include "bsp_tick.h" /* 单调毫秒（UI 任务的时间口径，与 LVGL 同源）*/
#include "log.h"
#include "meas_result.h"
#include "op_spec.h"
#include "op_wave.h"
#include "project_config.h"

#include "FreeRTOS.h"
#include "task.h"

#include "lvgl.h"

/* ★ 2026-10-14 八轮：原来的 `APP_LVGL_USE_DEMO` 双模式（业务 UI / LVGL 官方 demo）
 *   已**下线**（屏与触摸的上板验证早跑完了）⇒ 本文件只剩业务 UI 一种形态，
 *   相关的 `#if` 守卫全部删除；`lv_demo_widgets()` 的调用、CMake 源条目、
 *   `lv_conf.h` 的 `LV_USE_DEMO_WIDGETS` 三处同步删掉。 */

/* ---------------------------------------------------------------- 画法常量 ----
 * 这些是"怎么画"，不是产品参数（周期/阈值才去 project_config.h）。 */
#define UI_W APP_LVGL_SCR_W /* 480（逻辑横屏，2026-10-13 起）*/
#define UI_H APP_LVGL_SCR_H /* 320 */
#define UI_PAGE_N 3

/* 每秒一行 UI 健康统计（刷新次数/当前页/seq/堆余量）—— 上板排障用，置 0 可关。 */
#define UI_STAT_LOG 1

/* ★ 渲染消融诊断（T0，2026-10-14）已**用完删除**：当时靠它把"渲染成本"分摊到对象上，
 *   结论 = 波形 93% / 三卡 2% / 地板 5%（记录见 `_work/lvgl_perf_20261014/`）。
 *   要重做同类消融：把 `wave_update()` 或三卡的 `roll_value()` 临时注释掉烧一版即可，
 *   不必再养一套开关（开关本身也占 RAM/行数，且会让人误以为它在产品路径上生效）。 */

#define UI_PAD 16    /* 页边距 / 卡内边距（全局一致）*/
#define UI_RADIUS 18 /* 玻璃卡圆角 */

#define UI_HEAD_ICON_X 16
#define UI_HEAD_ICON_Y 12
#define UI_HEAD_LBL_X 48 /* 图标 24 + 8 间隙 */
#define UI_HEAD_LBL_Y 18
#define UI_CONTENT_Y 44 /* 内容起点 */

#define UI_PERIOD_W 46 /* 周期切换：**一个**键，键面写着当前值 */
#define UI_PERIOD_H 24

#define UI_START_W 160 /* 开机页的 "TAP TO START" 按钮（要看出"这是个能按的东西"）*/
#define UI_START_H 44

/* 版面（2026-10-13 四轮定稿：**数一律贴到屏幕边缘**，不再给右下角的 sysmon
 * 浮层让位 —— 用户明确说"不要怕跟帧率显示重合"：宁可被 FPS 浮层压到一点，
 * 也不要数缩在屏幕中间。）
 *  · 页 0：波形铺满 16..464，底部三连卡 F1 / Vpp / Vrms **贴到底**（y 248..308）
 *  · 页 1：频谱 16..288，右边缘 300..464 竖着**五个矩形框**（一框一根谱线：
 *         基波 + 2~5 次谐波，框里写频率与幅值），一列顶到 y=306 */
#define UI_WAVE_X0 16
#define UI_WAVE_X1 464
/* ★ 波形区尺寸 = **每像素都是钱**（2026-10-13 六轮实测）：LVGL 局部渲染下，一次
 * 波形重绘要把整个 448×(2×HALF) 的面积**逐像素**填一遍（背景）再把两条线画上去，
 * 实测 ≈50 个 CPU 周期/像素 ⇒ 90 的一半高度（180px 高）= 80.6k 像素 ≈ **32ms/次**。
 * HALF 90→66 ⇒ 面积 −27%（132px 高），换回来的是肉眼可感的顺滑。 */
#define UI_WAVE_MID 146 /* 波形区 y 80..212 的中线 */
#define UI_WAVE_HALF 66 /* 满量程的半个高度 */
/* 波形对象四周留的余量（px）：折线宽 2px ⇒ 点之外还会"溢出" 1px，对象面积
 * 必须把这点算进去，否则会被 LVGL 按对象边界裁掉（见 wave_apply）。 */
#define UI_WAVE_PAD 2

/* ⚠️ 2026-10-14：这里曾有 S1 的 `UI_WAVE_BOX_*`（固定整框）与 `UI_WAVE_SEG_N`（分 3 帧
 * 只失效 1/3）两组宏，**已删**。它们把"紧 bbox"换成了固定整框，实测是回归
 * （px/s 翻倍、handler 20.6%→32.4%、DSP 段 7→15.3ms，用户报"波形更卡顿了"）。
 * 完整事故记录见 `wave_apply` 顶部的"回退记录"。 */

#define UI_SPEC_X0 16
#define UI_SPEC_X1 288
#define UI_SPEC_BASE_Y 250 /* 频域基线（五轮起轴下方**不再印刻度字**）*/
#define UI_BAR_W 12
#define UI_BAR_MAX_H 180 /* 基线 250 − 70（柱顶最高到 y=70）*/

/* 谱线框（页 1 右边缘的五个矩形框）：5×46 + 4×8 = 262 ⇒ y 44..306 */
#define UI_BOX_X 300
#define UI_BOX_W 164
#define UI_BOX_Y0 44
#define UI_BOX_H 46
#define UI_BOX_GAP 8

#define UI_CARD_W 138 /* 页 0 底部三连卡（三张 + 两段 16 间隙 = 16..462）*/
#define UI_CARD_H 60
#define UI_CARD_Y 248 /* 60 高 ⇒ 248..308：贴底，只留 12 */
#define UI_CARD_GAP 16
/* ⚠️ 页 2 的带宽卡是**单独一张**：它的标签 `BANDWIDTH (Mpx/s)` 实测 ≈130px，
 *    138 宽的卡（内宽 122）会把尾巴裁掉（2026-10-13 三轮截图抓到过）。*/
#define UI_BW_W 160

/* 色板（方案定稿；全屏只有一个强调色）*/
#define UI_C_BG 0x080B10U
#define UI_C_GLASS_TOP 0x1A2430U
#define UI_C_GLASS_BOT 0x0B0F16U
#define UI_C_LINE 0x3A4653U
#define UI_C_ACCENT 0x4CC9F0U
#define UI_C_TEXT 0xEDF2F7U
#define UI_C_DIM 0x8494A8U

#define UI_HEX(c) lv_color_hex(c)

/* ⚠️ 内置字体（montserrat 12/36）的字模范围只有 **0x20-0x7F + 0xB0 + 0x2022**
 * （见 lv_font_montserrat_12.c 头注释）⇒ 屏上文案只能是 ASCII：
 *  · **`µ`（U+00B5）不在字模里** ⇒ 任何单位都不许用 µ（二轮已把 `Period (µs)`
 *    整项删掉，这里留个记号，别再写回来）；
 *  · **CJK 同样没有** ⇒ 别往这里加中文（屏上全英文是硬件限制，不是偏好）。 */

/* ---- 共享样式表（**内存账**：见文件头"内存"一节）----
 * ⚠️⚠️ 每条"文字 / 裸容器"样式都带一条 `bg_opa TRANSP`，**不是冗余**：
 *   `LV_USE_THEME_DEFAULT = 0` 之后没有任何东西覆盖 LVGL 的属性默认值，而
 *   `lv_style_prop_get_default()` 里 `BG_COLOR = white`、`BG_OPA = COVER`
 *   （见 lv_style.c）⇒ **没设过的 obj/label 就是一个白色实心块**。
 *   漏掉这一条 = 整屏被白底盖住（2026-10-13 实测栽过这一次）。 */
static lv_style_t s_st_bare;      /* 裸容器：背景透明（点得住的东西自己再叠样式）*/
static lv_style_t s_st_wall;      /* 全屏不透明黑底（screen / splash / 标定覆盖层）*/
static lv_style_t s_st_glass;     /* 玻璃卡：唯一的容器外观 */
static lv_style_t s_st_cap;       /* 12px 次文字（卡片标签 / 页眉标题）*/
static lv_style_t s_st_txt12;     /* 12px **不指定颜色**（颜色继承父对象 ⇒ 按下态）*/

static lv_style_t s_st_val;       /* 36px 主文字（开机页标题 / 标定页大字）*/
/* ★ 24px 主文字（2026-10-13 六轮）：**只给时域那三个数**用。
 *  why：它比 36px 小 1/3（用户要求），而**渲染像素按尺寸平方缩** ⇒ (24/36)² = 44%
 *  ⇒ 每帧少画一半以上的字形像素（配合 448×132 的波形区，这一页的重绘量再降一档）。
 *  开机页标题与标定页大字仍是 36px（那里没有性能压力，且要"大"）。 */
static lv_style_t s_st_val24;
static lv_style_t s_st_row;       /* 12px 主文字（频域右栏的谐波清单）*/
static lv_style_t s_st_icon;      /* 24×24 折线图标 */
static lv_style_t s_st_bar;       /* 谐波柱：顶强调色 → 底黑 */
static lv_style_t s_st_period;    /* 周期切换键：文字强调色 + 1px 描边（像个按钮）*/
static lv_style_t s_st_btn;       /* 描边文字按钮（标定覆盖层 / 标定按钮 / 开机页）*/
static lv_style_t s_st_btn_press; /* 按下：文字亮一档 */
static lv_style_t s_st_sep;       /* 3T 的周期分隔线（1px @20%）*/
static int s_styles_ready;

/* ---- 静态点数组（lv_line 只存指针 ⇒ 必须常驻）---- */
static lv_point_precise_t s_wave_pts[APP_UI_WAVE_POINTS];
static float s_wave_y[APP_UI_WAVE_POINTS]; /* 重建结果（mV），静态：UI 栈只剩 1.3KB */
/* ⚠️ 2026-10-14：这里原有 `s_raw_trace[APP_ENGINE_TRACE_N]` + 一条"必须整除"的
 *   `_Static_assert` —— **已删**。无基波时不再画原始轨迹（改成一条直线，见 `wave_idle`），
 *   那个数组和那条断言就没有读者了。省下 160 B RAM。
 *   ⚠️ 引擎侧的 `app_engine_raw()` 仍带 `trace` 参数（现在 UI 传 NULL）——
 *      它自己的抽取循环还在跑，属于"算了没人读"。收益 ≈0.1 ms/s，**本次不动**；
 *      要清的话是一次独立的小重构（改 `app_engine.c` 的 `raw_publish` + 收 API）。 */
static lv_point_precise_t s_axis_pts[2 + 5 * 3]; /* 基线 + 5 条刻度（见 build 说明）*/
static const lv_point_precise_t s_icon_time[5] = {{0, 12}, {6, 4}, {12, 20}, {18, 8}, {24, 12}};
static const lv_point_precise_t s_icon_freq[6] = {{3, 21}, {3, 13}, {11, 13}, {11, 7}, {19, 7}, {19, 2}};
static const lv_point_precise_t s_icon_cal[3] = {{2, 12}, {9, 19}, {22, 5}};

/* ---- 对象句柄 ---- */
static lv_obj_t *s_tv;
static lv_obj_t *s_tiles[UI_PAGE_N];
static lv_obj_t *s_cyc_key;      /* 周期切换键（**一个**：键面 = 当前值）*/
static lv_obj_t *s_cyc_lbl;      /* 键面上的字（1T / 3T）*/
static lv_obj_t *s_wave_line;    /* 波形（**单层 2px**；4px"发光层"2026-10-14 删，见页面构建处）*/
/* 波形"变化检测"用的上一帧（取整后的屏幕 y）+ 上一帧的点数（见 wave_apply）*/
static int16_t   s_wave_ylast[APP_UI_WAVE_POINTS];
static int       s_wave_last_n = -1;
static lv_obj_t *s_sep[2];      /* 3T 的周期分隔线 */
static lv_obj_t *s_time_val[3]; /* 0=F1 / 1=Vpp / 2=Vrms */
static lv_obj_t *s_bar[MEAS_MAX_PEAKS];
static lv_obj_t *s_box[MEAS_MAX_PEAKS];     /* 页 1 右边缘的谱线框（一框一根）*/
static lv_obj_t *s_box_val[MEAS_MAX_PEAKS]; /* 框里的"频率 + 幅值"一行字 */
/* ★ 五轮删掉频谱页的 `NO SIGNAL` 大字（用户要求）：五张参数框**常驻**显示，
 *  没信号的那几行写 `-- kHz -- mV`，比一个盖在中间的英文短语信息量大得多。 */
static lv_obj_t *s_st_list;
static lv_obj_t *s_cal_state;
static lv_obj_t *s_cal_bw;
static lv_obj_t *s_cal_panel;
static lv_obj_t *s_cal_progress;
static lv_obj_t *s_cal_prompt;
static lv_obj_t *s_splash;
static int s_cycles = 1; /* 波形 1T / 3T */

/* ---- 定点格式化（printf 浮点不可靠，docs/09）—— 数值一律手拼 ----
 * ★ 全 UI **只有 1 位小数**（2026-10-13 二轮定稿）：
 *   频率 1 位 = 100Hz 分辨率（判据 ≤1kHz）、幅值 1 位 = 0.1mV（判据 5mV），
 *   精度够；更要紧的是"所有参数同精度"⇒ 屏上数字长短齐整、不显随手。 */
static void fmt_dec1(char *buf, size_t cap, float v) { /* "241.7" */
    int32_t x = (int32_t)(v * 10.0f + (v >= 0.0f ? 0.5f : -0.5f));
    int32_t f = x % 10;
    if (f < 0) {
        f = -f;
    }
    (void)snprintf(buf, cap, "%ld.%ld", (long)(x / 10), (long)f);
}

/* ---- 小工具 ---- */

/* 只在内容真变了才写 —— 静屏不产生重绘（性能预算的第一条）
 * ★ 2026-10-14 起**必须容忍 NULL**：页面改成按需创建/用后销毁之后，刷新函数有可能在
 *   "这一页还没建 / 刚被销毁"时被调到。**早返回而不是解引用** —— 这是整套懒加载的安全网：
 *   与其在 8 个刷新函数里逐个判页面，不如让最底层的两个写入口直接免疫。 */
static void set_text(lv_obj_t *lbl, const char *txt) {
    if (lbl == NULL) {
        return;
    }
    if (lv_strcmp(lv_label_get_text(lbl), txt) != 0) {
        lv_label_set_text(lbl, txt);
    }
}

/* 显隐（幂等：状态没变就不碰，`lv_obj_add_flag` 会顺带触发失效重绘）
 * ★ 同 `set_text`：必须容忍 NULL（页没建/已销毁）。 */
static void set_hidden(lv_obj_t *o, int hidden) {
    if (o == NULL) {
        return;
    }
    const int cur = lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) ? 1 : 0;
    if (cur == (hidden != 0)) {
        return;
    }
    if (hidden != 0) {
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

/* 标签：外观**只挂共享样式**，不写散装属性 */
static lv_obj_t *label_create(lv_obj_t *parent, const char *txt, lv_style_t *style,
                              lv_coord_t x, lv_coord_t y) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_add_style(l, style, LV_PART_MAIN);
    lv_obj_set_pos(l, x, y);
    return l;
}

/* 页眉：24×24 线性图标（`lv_line` 折线，纯装饰、不可点）+ 12px 灰标题，起点固定 */
static void header_create(lv_obj_t *pg, const char *title,
                          const lv_point_precise_t *pts, uint32_t n) {
    lv_obj_t *l = lv_line_create(pg);
    lv_obj_set_pos(l, UI_HEAD_ICON_X, UI_HEAD_ICON_Y);
    lv_line_set_points(l, pts, n);
    lv_obj_add_style(l, &s_st_icon, LV_PART_MAIN);
    (void)label_create(pg, title, &s_st_cap, UI_HEAD_LBL_X, UI_HEAD_LBL_Y);
}

/* 玻璃卡：全 UI 唯一的容器（半透明 + 渐变 + 1px 描边 + r18）*/
static lv_obj_t *glass_card_create(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                                   lv_coord_t w, lv_coord_t h) {
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_size(c, w, h);
    lv_obj_add_style(c, &s_st_glass, LV_PART_MAIN);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;

}



/* ---- 数值滚入（240ms ease-out，**只位移不淡入**）----
 * ★ 2026-10-13 四轮把"对象级 opa 淡入"删了，只留 `translate_y` 位移。
 *   为什么（板上现场教训）：**对象 opa < COVER 会让 LVGL 给这个 label 开一张
 *   ARGB8888 层**（`refr_obj` 里 `lv_draw_layer_create`）——≈120×44×4 = 21KB，
 *   而且**每帧**一次 alloc → 渲染 → 混合 → free。板上池合计 36KB、界面已占
 *   ≈21KB ⇒ 这张层要么分配失败（静默，界面就少画一块），要么把渲染成本翻几倍。
 *   位移已经足够表达"数在动"，而"任何一帧都能读到数"本来就要求起点不透明。 */
/* ★★ 数字更新：**直接写，不做滚入动画**（2026-10-13 六轮，实测砍掉一半重绘量）
 *
 * 实测证据（板上 DWT 计时 + flush 像素统计）：
 *   · `lv_timer_handler()` 每秒吃 **0.89 s CPU**，而其中"写屏"只占 4.5%；
 *   · 每秒 flush **1.08 Mpx** —— 波形只占 269 kpx/s，**其余 ~590 kpx/s 全是滚入动画**：
 *     一次滚 = 该卡 14 帧重绘（140ms ÷ 10ms 节拍）× 3 张卡 × 5 次更新/s。
 *   · 而 5Hz 的更新下，这 140ms 的滚动**肉眼根本看不见**（下一帧数据马上又来了）。
 * ⇒ 拿掉动画，只留下"数值变了才写"（`set_text` 内部比字符串）。省下的是实打实的
 *   CPU 和重绘带宽；数字仍然 5Hz 更新，观感差异只在变化的那一瞬间。
 * ⚠️ 要恢复滚入观感：把本函数换成 `lv_anim` 的 translate_y（历史版本见 git）。 */
static void roll_value(lv_obj_t *lbl, const char *txt) {
    set_text(lbl, txt); /* 内容没变时 set_text 自己会跳过（不产生重绘）*/
}

/* 样式表：**只在这里 init 一次**（`lv_style_init` 不是幂等的 —— 重复初始化会
 * 把上一份属性数组漏在堆里，而池只有 32KB）。 */
static void styles_init(void) {
    lv_style_init(&s_st_bare);
    lv_style_set_bg_opa(&s_st_bare, LV_OPA_TRANSP);

    lv_style_init(&s_st_wall);
    lv_style_set_bg_color(&s_st_wall, UI_HEX(UI_C_BG));
    lv_style_set_bg_opa(&s_st_wall, LV_OPA_COVER);

    lv_style_init(&s_st_glass);
    lv_style_set_bg_color(&s_st_glass, UI_HEX(UI_C_GLASS_TOP));
    lv_style_set_bg_grad_color(&s_st_glass, UI_HEX(UI_C_GLASS_BOT));
    lv_style_set_bg_grad_dir(&s_st_glass, LV_GRAD_DIR_VER);
    lv_style_set_bg_opa(&s_st_glass, LV_OPA_30);
    lv_style_set_border_color(&s_st_glass, UI_HEX(UI_C_LINE));
    lv_style_set_border_width(&s_st_glass, 1);
    lv_style_set_border_opa(&s_st_glass, LV_OPA_50);
    lv_style_set_radius(&s_st_glass, UI_RADIUS);
    lv_style_set_pad_all(&s_st_glass, 0);

    lv_style_init(&s_st_cap);
    lv_style_set_bg_opa(&s_st_cap, LV_OPA_TRANSP);
    lv_style_set_text_font(&s_st_cap, &lv_font_montserrat_12);
    lv_style_set_text_color(&s_st_cap, UI_HEX(UI_C_DIM));

    lv_style_init(&s_st_txt12);
    lv_style_set_bg_opa(&s_st_txt12, LV_OPA_TRANSP);
    lv_style_set_text_font(&s_st_txt12, &lv_font_montserrat_12);

    lv_style_init(&s_st_val);
    lv_style_set_bg_opa(&s_st_val, LV_OPA_TRANSP);
    lv_style_set_text_font(&s_st_val, &lv_font_montserrat_36);
    lv_style_set_text_color(&s_st_val, UI_HEX(UI_C_TEXT));

    lv_style_init(&s_st_val24);
    lv_style_set_bg_opa(&s_st_val24, LV_OPA_TRANSP);
    lv_style_set_text_font(&s_st_val24, &lv_font_montserrat_24);
    lv_style_set_text_color(&s_st_val24, UI_HEX(UI_C_TEXT));

    lv_style_init(&s_st_row);
    lv_style_set_bg_opa(&s_st_row, LV_OPA_TRANSP);
    lv_style_set_text_font(&s_st_row, &lv_font_montserrat_12);
    lv_style_set_text_color(&s_st_row, UI_HEX(UI_C_TEXT));

    lv_style_init(&s_st_icon);
    lv_style_set_bg_opa(&s_st_icon, LV_OPA_TRANSP);
    lv_style_set_line_color(&s_st_icon, UI_HEX(UI_C_DIM));
    lv_style_set_line_width(&s_st_icon, 2);
    lv_style_set_line_rounded(&s_st_icon, true);

    lv_style_init(&s_st_bar);
    lv_style_set_bg_color(&s_st_bar, UI_HEX(UI_C_ACCENT));
    lv_style_set_bg_grad_color(&s_st_bar, UI_HEX(UI_C_BG));
    lv_style_set_bg_grad_dir(&s_st_bar, LV_GRAD_DIR_VER);
    lv_style_set_bg_opa(&s_st_bar, LV_OPA_COVER);
    lv_style_set_radius(&s_st_bar, 6);

    /* 周期切换键（三轮改成**单键**）：一个键没有"未选中"那半可以对照 ⇒ 改靠外形
     * 表态 —— 1px 描边让它"看起来是个能按的按钮"，文字用强调色表"这是当前设置"。
     * 按下态直接复用 s_st_btn_press（文字亮一档 + 描边点亮成强调色）。 */
    lv_style_init(&s_st_period);
    lv_style_set_text_color(&s_st_period, UI_HEX(UI_C_ACCENT));
    lv_style_set_border_color(&s_st_period, UI_HEX(UI_C_LINE));
    lv_style_set_border_width(&s_st_period, 1);
    lv_style_set_radius(&s_st_period, 12);

    lv_style_init(&s_st_btn);
    lv_style_set_text_color(&s_st_btn, UI_HEX(UI_C_DIM));
    lv_style_set_border_color(&s_st_btn, UI_HEX(UI_C_LINE));
    lv_style_set_border_width(&s_st_btn, 1);
    lv_style_set_radius(&s_st_btn, 12);

    lv_style_init(&s_st_btn_press); /* 按下：文字亮一档 + 描边点亮成强调色 */
    lv_style_set_text_color(&s_st_btn_press, UI_HEX(UI_C_TEXT));
    lv_style_set_border_color(&s_st_btn_press, UI_HEX(UI_C_ACCENT));

    lv_style_init(&s_st_sep);
    lv_style_set_bg_color(&s_st_sep, UI_HEX(UI_C_DIM));
    lv_style_set_bg_opa(&s_st_sep, LV_OPA_20);
}

/* ============================================================ 页 0 · 时域 */
static void cycles_sync(void); /* 定义在下面；谱口径波形回来时要靠它恢复 1T/3T 分隔线 */

/* ★★ 提交折线（2026-10-14 七轮，**这一处是波形性能的真正大头**）：
 *   `lv_line` 的对象面积 = 点数组的**最大坐标**（SIZE_CONTENT 口径，取的是 max 而不是
 *   "min..max 的跨度"）。而我们的点从 (16,80) 铺到 (464,212) ⇒ 对象被撑成
 *   **465×150 ≈ 70 kpx**，且**每次 `lv_line_set_points()` 都要把这一整块重新填背景**
 *   （LVGL 的局部刷新按脏区矩形，脏区就是对象面积）—— T0 消融实测：波形吃掉整页
 *   渲染时间的 93%，其中大头是**这块面积**，不是画线本身。
 *   ⇒ 这里显式给对象设 pos/size（不再让 SIZE_CONTENT 决定），并把点数组**平移到对象
 *     原点**：对象面积缩到"轨迹自己扫过的那条带"（噪声带 ≈ 448×8，正弦 ≈ 448×134）。
 *   ⚠️ 对象位置每帧都随轨迹带宽变化 ⇒ 旧带 ∪ 新带都会被脏化，两块都很薄 ✓。
 *   ⚠️ 四周留 `UI_WAVE_PAD`：折线有 2px 宽，点之外还会溢出 1px，面积太紧会被裁掉。 */
static void wave_apply(int n) {
    /* ★ 2026-10-14 八轮（P2.2）：**变化检测** —— 稳定信号下不再每 100ms 重画同一条曲线。
     *   判据：把新算出的屏幕 y 取整后与"上一次真正画上去的"比，**所有列都 ≤1px** 就跳过
     *   （不设点、不脏区）。为什么用取整后比较：幅度上有 mV 级抖动，换算成像素只在半像素内
     *   晃 —— 那种"变化"画出来肉眼一致，却是实打实的 52~62 kpx 重绘（≈20ms 渲染）。
     *   ⚠️ 两种情况必须强制重画：① 点数变了（谱口径 160 / 噪声带 80 互切，两条轨迹口径
     *      不同）② 对象当前是隐藏的（上一帧没数据，这次有）。
     *
     * ⚠️⚠️ 2026-10-14 回退记录（S1 事故）：本轮曾把这里改成"**固定整框 + 分 3 帧只失效
     *   1/3**"，**上板实测是回归，已整段回退**。证据（用户 10:53 的 RTT 日志 vs 09:32 基线）：
     *     `[LVGL] diag` px/s **81,577 → 165,558**（翻倍）·
     *     `handler` **20.6% → 32.4%** ·
     *     `[ENG]` DSP 段 **7.05 → 15.30 ms**（被 UI 抢 CPU 拉长）·
     *     用户原话「波形更卡顿了」。
     *   **根因就在下面这个"紧 bbox"**：噪声带的紧 bbox 只有 **448×8 ≈ 3.6 kpx**，
     *   而固定整框 453×137 的 1/3 就是 **20.7 kpx** —— **失效面积涨 5.8×**。
     *   次要原因：分 3 帧 ⇒ 三段各显示**不同时刻**的数据（撕裂），而 `s_wave_ylast`
     *   记的是"最新数据"而非"真的画上去的" ⇒ 三个段会长期不同步。
     *   ⇒ **紧 bbox 不是"实现细节"，是这里最大的一笔优化。别拿它去换别的。** */
    int16_t ny[APP_UI_WAVE_POINTS];
    for (int j = 0; j < n; j++) {
        ny[j] = (int16_t)(s_wave_pts[j].y + 0.5f);
    }
    if (n == s_wave_last_n && !lv_obj_has_flag(s_wave_line, LV_OBJ_FLAG_HIDDEN)) {
        int same = 1;
        for (int j = 0; j < n; j++) {
            const int d = (int)ny[j] - (int)s_wave_ylast[j];
            if (d > 1 || d < -1) {
                same = 0;
                break;
            }
        }
        if (same != 0) {
            return; /* 屏上已经是对的 ⇒ 这一帧什么都不用做 */
        }
    }
    for (int j = 0; j < n; j++) {
        s_wave_ylast[j] = ny[j];
    }
    s_wave_last_n = n;

    float ymin = s_wave_pts[0].y;
    float ymax = s_wave_pts[0].y;
    for (int j = 1; j < n; j++) {
        if (s_wave_pts[j].y < ymin) {
            ymin = s_wave_pts[j].y;
        }
        if (s_wave_pts[j].y > ymax) {
            ymax = s_wave_pts[j].y;
        }
    }
    const lv_coord_t x0 = (lv_coord_t)(UI_WAVE_X0 - UI_WAVE_PAD);
    const lv_coord_t y0 = (lv_coord_t)ymin - UI_WAVE_PAD;
    const lv_coord_t w =
        (lv_coord_t)(UI_WAVE_X1 - UI_WAVE_X0 + 1) + 2 * (lv_coord_t)UI_WAVE_PAD;
    const lv_coord_t h =
        (lv_coord_t)(ymax - ymin) + 1 + 2 * (lv_coord_t)UI_WAVE_PAD;

    lv_obj_set_pos(s_wave_line, x0, y0);
    lv_obj_set_size(s_wave_line, w, h); /* 显式尺寸 ⇒ LVGL 不再用 SIZE_CONTENT 撑面积 */

    for (int j = 0; j < n; j++) { /* 平移到对象原点（对象坐标 = 屏幕坐标 − 对象位置）*/
        s_wave_pts[j].x -= (float)x0;
        s_wave_pts[j].y -= (float)y0;
    }
    lv_line_set_points(s_wave_line, s_wave_pts, n);
    lv_obj_remove_flag(s_wave_line, LV_OBJ_FLAG_HIDDEN);
}

/* 波形线的颜色按状态切：有周期信号 = 强调色；无 = 暗色（见 `wave_idle`）。
 * ★ 为什么要这个"同色不碰"的闸：`lv_obj_set_style_line_color()` 会触发样式刷新 + 失效，
 *   而调用方每 100ms 都会走一次 —— 不设闸就等于**每帧都把整条波形标脏**，
 *   把 `wave_apply` 的变化检测整个作废。 */
static int s_wave_dim = -1; /* -1 = 还没定过 */
static void wave_set_color(int dim) {
    if (s_wave_dim == dim) {
        return;
    }
    s_wave_dim = dim;
    lv_obj_set_style_line_color(s_wave_line, UI_HEX(dim != 0 ? UI_C_DIM : UI_C_ACCENT),
                                LV_PART_MAIN);
}

static void wave_update(const meas_result_t *r) {
    wave_set_color(0); /* 有周期信号 ⇒ 回到强调色 */
    if (r->n_peaks == 0U) {
        lv_obj_add_flag(s_wave_line, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    /* 纵向量程 = 分量幅值和 ×1.2（自动缩放；下限 1mV 防除零）*/
    float amp_sum = 0.0f;
    for (int i = 0; i < (int)r->n_peaks; i++) {
        amp_sum += r->peak[i].amp_mv;
    }
    const float range = (amp_sum < 1.0f) ? 1.0f : amp_sum * 1.2f;
    const float scale = (float)UI_WAVE_HALF / range;

    if (op_wave_build(r, s_cycles, s_wave_y, APP_UI_WAVE_POINTS) != APP_UI_WAVE_POINTS) {
        return;
    }
    const float dx = (float)(UI_WAVE_X1 - UI_WAVE_X0) / (float)(APP_UI_WAVE_POINTS - 1);
    for (int j = 0; j < APP_UI_WAVE_POINTS; j++) {
        s_wave_pts[j].x = (float)UI_WAVE_X0 + (float)j * dx;
        s_wave_pts[j].y = (float)UI_WAVE_MID - s_wave_y[j] * scale; /* 屏 y 向下 */
    }
    wave_apply(APP_UI_WAVE_POINTS);
    cycles_sync(); /* 原始路径可能把 1T/3T 分隔线藏了 —— 回到谱口径时按当前档位恢复 */
}

/* ★ 原始曲线（2026-10-13 六轮）：**没有检出基波时**画 ADC 原始帧的抽取轨迹。
 *  why：谱口径的波形是"按峰表重建"的，没有峰 ⇒ 屏上空白 —— 而"ADC 在采、链路是通的"
 *  这件事必须一眼可见（悬空输入就该显示一条噪声带，而不是什么都不显示）。
 *  ⚠️ 与谱口径波形**同名不同口径**：谱口径按"分量幅值和"定纵向标尺（物理 mV 可比），
 *    这里按**自身 min/max 自动缩放** —— 否则几 mV 的噪声在满量程里就是一条直线，
 *    等于还是"什么都不显示"。绝对幅度由 Vpp / Vrms 两张卡给（原始帧口径）。
 *  返回 1 = 画了（并藏掉 1T/3T 分隔线：没有基波就没有"周期"可分）；0 = 无数据。 */
/* ---- 无基波：画一条**不动的直线**（2026-10-14，用户提的方案）----
 *
 * 原来是画"抽取后的原始轨迹"（80 点噪声带）。两个问题：
 *   ① **噪声每帧都在变** ⇒ `wave_apply` 的变化检测**永远命中"变了"** ⇒
 *      无信号时反而**每 100ms 都在重绘**（`[LVGL] diag` 的 px/s 底噪主要就是它）；
 *   ② 那条带虽然紧 bbox 只有 448×8，但它是"一直在动的背景负载"，观感上也在闪。
 *
 * ⇒ 改成一条**点数组恒定**的直线：`wave_apply` 的变化检测第 2 帧起**永远跳过**
 *   ⇒ **无信号状态的重绘降到 0**。这正是"噪声不显示成波形"的收益来源 ——
 *   不是省了单次重绘，而是**把持续负载变成了零**。
 *
 * ★ 只给 **2 个点**：`lv_draw_sw_line.c:68` 判 `p1.y == p2.y` ⇒ 走 `draw_line_hor`
 *   快路径（**不建覆盖率掩膜、不 malloc**）。点数再多也只是白花。
 * ★ 颜色用 `UI_C_DIM`（暗）而不是波形的强调色 —— 让"没有周期信号"与"信号平直"能区分。
 *   ⚠️ 想改成强调色：把 `wave_set_color(1)` 换成 `wave_set_color(0)` 即可。
 * ⚠️ **统计（Vpp/Vrms）照旧给**（走原始帧口径，见 `refresh_time`）—— 只是不画它的轨迹。
 *   ⇒ 引擎侧的抽取轨迹（`APP_ENGINE_TRACE_N`）**已随之删除**（没有读者了）。
 * ⚠️ 这是**用户 2026-10-14 拍板**的设计：悬空输入时屏上是一条基线，而不是一团噪声。
 *   它**推翻了** 2026-10-13 六轮"不留空屏、屏上仍是一屏活数据"的旧决定 ——
 *   那条旧决定的代价正是"无信号时每 100ms 重绘一次"。 */
static int wave_idle(void) {
    app_engine_raw_t st;
    const int ok = (app_engine_raw(&st) == 0); /* 只要统计（轨迹已于 2026-10-14 删）*/

    wave_set_color(1);
    s_wave_pts[0].x = (float)UI_WAVE_X0;
    s_wave_pts[0].y = (float)UI_WAVE_MID;
    s_wave_pts[1].x = (float)UI_WAVE_X1;
    s_wave_pts[1].y = (float)UI_WAVE_MID;
    wave_apply(2);

    for (int i = 0; i < 2; i++) {
        set_hidden(s_sep[i], 1); /* 12px 的 1T/3T 分隔线在"没有周期"时不出现 */
    }
    return ok;
}

/* 周期键的样子：键面写**当前值**（一个键只能这样表态）；3T 才放出两条分隔线。 */
static void cycles_sync(void) {
    set_text(s_cyc_lbl, (s_cycles == 3) ? "3T" : "1T");
    for (int i = 0; i < 2; i++) {
        if (s_cycles == 3) {
            lv_obj_remove_flag(s_sep[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_sep[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* 一个键来回切。按下态 = 临时加一张共享样式（`s_st_btn_press`：文字亮一档 +
 * 描边点亮成强调色），松手回落到 `s_st_period` —— 不用 LV_STATE_PRESSED，
 * 是为了跟标定页那几个按钮走同一条路（少一套状态样式）。 */
static void cycles_key_cb(lv_event_t *e) {
    lv_obj_t *k = lv_event_get_current_target(e);
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        lv_obj_add_style(k, &s_st_btn_press, LV_PART_MAIN);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        lv_obj_remove_style(k, &s_st_btn_press, LV_PART_MAIN);
    } else if (code == LV_EVENT_CLICKED) {
        s_cycles = (s_cycles == 3) ? 1 : 3; /* 一个键：来回切 */
        cycles_sync();
        wave_update(app_engine_result()); /* 立即重画，不等下一帧测量 */
    }
}

static void page_time_build(lv_obj_t *pg) {
    header_create(pg, "TIME DOMAIN", s_icon_time, 5U);

    /* 周期切换：**一个**键（页眉右端）。两个键靠"选中 / 未选中"的对照表态，
     * 一个键只能靠键面上那个字 —— 所以描边要留着（暗示"能按"），文字用强调色
     * 表示"这就是当前的周期数"。 */
    lv_obj_t *k = lv_obj_create(pg);
    lv_obj_set_pos(k, (lv_coord_t)(UI_W - UI_PAD - UI_PERIOD_W), UI_HEAD_ICON_Y);
    lv_obj_set_size(k, UI_PERIOD_W, UI_PERIOD_H);
    lv_obj_add_style(k, &s_st_bare, LV_PART_MAIN);
    lv_obj_add_style(k, &s_st_period, LV_PART_MAIN);
    lv_obj_remove_flag(k, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(k, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(k, cycles_key_cb, LV_EVENT_ALL, NULL);
    s_cyc_key = k;
    s_cyc_lbl = lv_label_create(k);
    lv_obj_add_style(s_cyc_lbl, &s_st_txt12, LV_PART_MAIN); /* 只给字号：颜色继承父对象 */
    lv_obj_center(s_cyc_lbl);

    /* ★ 五轮删掉那行 `no signal`（用户要求：屏上不出现这个词）。
     * 无信号时每张卡本来就写着 `--`，信息不缺；再写一句英文短语只是噪声。 */

    /* ★★ 波形线（2026-10-14 七轮 T2：三处"减负"，全部由 T0 消融实测指出）----
     * T0（`[UI] 消融相位` + `[LVGL] diag`）把成本钉死了：
     *   **冻结波形 ⇒ 每轮 LVGL 从 187 ms 掉到 10~16 ms**，而 flush 只占 4%
     *   ⇒ 钱全花在 LVGL 的**软件光栅化**上，不是写屏、也不是三卡数字（那只有 2 ms）。
     * 机理（读 `lv_draw_sw_line.c` + LVGL 9.5 的绘制模型，逐条对上实测）：
     *   · 斜线段走 `draw_line_skew`：**每段**建一次覆盖率掩膜，再把整段包围盒
     *     逐像素混合 —— 本屏波形是噪声/高次谐波，段段陡峭（|Δy| 几十像素）
     *     ⇒ 包围盒面积 × 段数 = 真正的开销来源；
     *   · `line_rounded = true` 会给**每段两端各加一个圆头填充任务**（320 段 ⇒ 最多
     *     640 个额外任务）—— 既吃 LVGL 池（32KB）又吃时间；
     *   · 原"发光层"是 **4px** 粗线 ⇒ 掩膜宽度翻倍（像素数 ×2）。
     * ⇒ 三条减负：① 删掉 4px 发光层（只留 2px 主线，视觉 = 更清晰的细线）
     *   ② `line_rounded = false` ③ 点数 320 → 160（段数减半，见 project_config.h）。
     * ⚠️ 为什么不"每列一个矩形自绘"（更彻底但此版本不可行）：LVGL 9.5 的绘制只能
     *    **加 draw task**（在 DRAW_MAIN 里直接写 draw buffer 会被随后执行的背景填充
     *    任务覆盖）；而"每列一个矩形" = 449 个任务 × ~百字节 ⇒ 池直接爆。
     *    要走到那一步得等一次真正的架构升级（自定义 draw unit / 显示叠加层）。 */
    s_wave_line = lv_line_create(pg);
    lv_line_set_points(s_wave_line, s_wave_pts, APP_UI_WAVE_POINTS);
    lv_obj_add_style(s_wave_line, &s_st_bare, LV_PART_MAIN); /* 否则是白底大块 */
    lv_obj_set_style_line_width(s_wave_line, 2, LV_PART_MAIN);
    /* 线色走**唯一入口** `wave_set_color()`（有信号 = 强调色 / 无 = 暗色）——
     * 不再在这里直接写样式，否则"线色"就有两个真相源。 */
    wave_set_color(0);
    lv_obj_set_style_line_rounded(s_wave_line, false, LV_PART_MAIN); /* 每段省两个圆头任务 */
    /* 先给一个"紧"的显式尺寸：不用 SIZE_CONTENT，否则对象面积会被点数组的**最大
     * 坐标**撑成 465×150 ≈ 70 kpx（见 wave_apply 的注释）。真实尺寸每帧由 wave_apply 给。 */
    lv_obj_set_pos(s_wave_line, (lv_coord_t)(UI_WAVE_X0 - UI_WAVE_PAD),
                   (lv_coord_t)(UI_WAVE_MID - UI_WAVE_PAD));
    lv_obj_set_size(s_wave_line, (lv_coord_t)(UI_WAVE_X1 - UI_WAVE_X0 + 1 + 2 * UI_WAVE_PAD),
                    (lv_coord_t)(2 * UI_WAVE_PAD + 1));
    lv_obj_add_flag(s_wave_line, LV_OBJ_FLAG_HIDDEN);

    /* 3T 的周期分隔线：1px 竖条 @20%，与波形区等高 */
    for (int i = 0; i < 2; i++) {
        const lv_coord_t x =
            (lv_coord_t)(UI_WAVE_X0 + (UI_WAVE_X1 - UI_WAVE_X0) * (i + 1) / 3);
        s_sep[i] = lv_obj_create(pg);
        lv_obj_set_pos(s_sep[i], x, (lv_coord_t)(UI_WAVE_MID - UI_WAVE_HALF));
        lv_obj_set_size(s_sep[i], 1, (lv_coord_t)(2 * UI_WAVE_HALF));
        lv_obj_add_style(s_sep[i], &s_st_sep, LV_PART_MAIN);
        lv_obj_remove_flag(s_sep[i], LV_OBJ_FLAG_SCROLLABLE);
    }

    /* 底部三连卡：F1 / Vpp / Vrms（三轮：时域的数**全在底下** ⇒ 波形因此吃满整宽）。
     * 三张 138 + 两段 16 间隙 = x 16..462；56 高 ⇒ y 212..268，**正好止在 sysmon
     * 浮层保留区（y>268 且 x>376）的上沿** —— 第三张卡不会被 FPS 浮层压住。 */
    static const char *const k_cap[3] = {"F1 (kHz)", "Vpp (mV)", "Vrms (mV)"};
    for (int i = 0; i < 3; i++) {
        const lv_coord_t x = (lv_coord_t)(UI_PAD + i * (UI_CARD_W + UI_CARD_GAP));
        lv_obj_t *c = glass_card_create(pg, x, UI_CARD_Y, UI_CARD_W, UI_CARD_H);
        (void)label_create(c, k_cap[i], &s_st_cap, UI_PAD, 8);
        /* 24px 数字（用户：小 1/3）+ y 从 18 挪到 22 ⇒ 卡内上下留白更匀 */
        s_time_val[i] = label_create(c, "--", &s_st_val24, UI_PAD, 22);
    }

    cycles_sync();
}

static void refresh_time(const meas_result_t *r) {
    char b[24];

    if (r->n_peaks != 0U) {
        fmt_dec1(b, sizeof(b), r->f1_hz / 1000.0f);
        roll_value(s_time_val[0], b);
        fmt_dec1(b, sizeof(b), r->upp_mv);
        roll_value(s_time_val[1], b);
        fmt_dec1(b, sizeof(b), r->urms_mv);
        roll_value(s_time_val[2], b);
        wave_update(r);
        return;
    }

    /* ---- 没检出基波：**不写 no signal、也不留空屏**（2026-10-13 六轮）----
     * F1 没有就是没有（写 `--`，不编数）；Vpp / Vrms 用**原始帧口径**顶上
     * （谱口径在无基波时数学上恒为 0，见 app_engine.h）；波形画原始轨迹。
     * 典型场景：输入悬空（引脚 ~0.8V 偏置 + 几 mV 噪声）—— 屏上仍是一屏活数据。 */
    app_engine_raw_t st;
    const int raw_ok = (app_engine_raw(&st) == 0);
    roll_value(s_time_val[0], "--");
    if (raw_ok) {
        fmt_dec1(b, sizeof(b), st.vpp_mv);
        roll_value(s_time_val[1], b);
        fmt_dec1(b, sizeof(b), st.vrms_mv);
        roll_value(s_time_val[2], b);
    } else {
        roll_value(s_time_val[1], "--"); /* 一帧都还没采到（开机头几拍）*/
        roll_value(s_time_val[2], "--");
    }
    (void)wave_idle();
}

/* ============================================================ 页 1 · 频域 */
static void bar_grow_cb(void *var, int32_t v) {
    /* 柱必须**同时**改高度和 y（顶锚在基线）—— 一个回调一起设 */
    lv_obj_t *b = (lv_obj_t *)var;
    lv_obj_set_height(b, (lv_coord_t)v);
    lv_obj_set_y(b, (lv_coord_t)(UI_SPEC_BASE_Y - v));
}

/* 对数横轴：**与 op_spec.c 的 x01 同一公式**（那边是 Operation 层的权威实现，
 * 这边只给刻度用；柱位置直接用 op_spec_build 给的 x01）。
 * ⚠️ 改映射要两处一起改 —— `test_op_render.c` 钉住了 op_spec 那一侧。 */
static lv_coord_t spec_x_from_hz(float hz) {
    const float x01 = (log10f(hz) - log10f(10000.0f)) /
                      (log10f(500000.0f) - log10f(10000.0f));
    return (lv_coord_t)(UI_SPEC_X0 + x01 * (float)(UI_SPEC_X1 - UI_SPEC_X0) + 0.5f);
}

static void page_freq_build(lv_obj_t *pg) {
    static const float k_tick_hz[5] = {10000.0f, 50000.0f, 100000.0f, 250000.0f, 500000.0f};

    header_create(pg, "FREQUENCY", s_icon_freq, 6U);

    /* 基线 + 5 条 1px 短竖合成**一个** lv_line（省 5 个对象）：
     * 走法是"基线一次 → 每个刻度 下去 / 回来 / 沿基线到下一个"，
     * 其中"回来"和"沿基线"两段与既有线段**完全重合** ⇒ 视觉上只有基线与刻度。
     * ★ 五轮去掉刻度**文字**（10k/50k/100k/250k/500k，用户要求）：五根柱各自带
     *   频率读数（右侧五个框），轴上再标一遍数字是同一信息的第二次出场 ——
     *   刻度短线留着当"轴存在"的记号即可。 */
    uint32_t n = 0;
    s_axis_pts[n].x = UI_SPEC_X0;
    s_axis_pts[n++].y = UI_SPEC_BASE_Y;
    s_axis_pts[n].x = UI_SPEC_X1;
    s_axis_pts[n++].y = UI_SPEC_BASE_Y;
    for (int i = 0; i < 5; i++) {
        const lv_coord_t x = spec_x_from_hz(k_tick_hz[i]);
        s_axis_pts[n].x = x;
        s_axis_pts[n++].y = UI_SPEC_BASE_Y;
        s_axis_pts[n].x = x;
        s_axis_pts[n++].y = (lv_coord_t)(UI_SPEC_BASE_Y + 6);
        s_axis_pts[n].x = x;
        s_axis_pts[n++].y = UI_SPEC_BASE_Y;
    }
    lv_obj_t *axis = lv_line_create(pg);
    lv_line_set_points(axis, s_axis_pts, n);
    lv_obj_add_style(axis, &s_st_bare, LV_PART_MAIN);
    lv_obj_set_style_line_color(axis, UI_HEX(UI_C_LINE), LV_PART_MAIN);
    lv_obj_set_style_line_width(axis, 1, LV_PART_MAIN);

    /* 1~5 次谐波柱：12px 宽、圆角 6、顶强调色→底黑竖直渐变。
     * 未检出的柱直接**藏掉**（不是留一个 0 高的空壳）—— 屏幕上不出现"没有的
     * 东西"的位置，是这个页面的留白纪律。 */
    for (int i = 0; i < MEAS_MAX_PEAKS; i++) {
        s_bar[i] = lv_obj_create(pg);
        lv_obj_set_size(s_bar[i], UI_BAR_W, 0);
        lv_obj_set_pos(s_bar[i], UI_SPEC_X0, UI_SPEC_BASE_Y);
        lv_obj_add_style(s_bar[i], &s_st_bar, LV_PART_MAIN);
        lv_obj_remove_flag(s_bar[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(s_bar[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* 右边缘五个矩形框：一框一根谱线（基波 + 2~5 次谐波），框里写"频率 + 幅值"。
     * ★ 四轮从"一块多行 label 打五行"改成"一框一根"（用户要求）：框的边界让
     *   "哪根是哪根"一眼可分，代价是 5 卡 + 10 label ≈ +3.2KB —— 池里有余量，值。
     * ⚠️ 框里**不能开 recolor**：序号写成 `#1`…`#5`，而 LVGL 的重着色语法也是
     *   `#RRGGBB 文字#` ⇒ 两套 '#' 会打架（三轮实测屏上出现 "1250.0k" 残渣）。 */
    for (int i = 0; i < MEAS_MAX_PEAKS; i++) {
        const lv_coord_t y = (lv_coord_t)(UI_BOX_Y0 + i * (UI_BOX_H + UI_BOX_GAP));
        s_box[i] = glass_card_create(pg, UI_BOX_X, y, UI_BOX_W, UI_BOX_H);
        char tag[8];
        (void)snprintf(tag, sizeof(tag), "#%d", i + 1);
        (void)label_create(s_box[i], tag, &s_st_cap, UI_PAD, 5);
        s_box_val[i] = label_create(s_box[i], "--", &s_st_row, UI_PAD, 22);
        /* ★ 五轮：框**常驻**（不再"没检出的框不占视觉"）+ 框里恒有单位，
         * 没信号时写 `-- kHz  -- mV`（用户要求：框和单位必须看得见）。 */
        set_text(s_box_val[i], "-- kHz  -- mV");
    }
}

/* 五个框的内容：**恒 5 行**（一框一根：基波 + 2~5 次谐波）。
 * 检出几根就写几组真数（频率从低到高，基波在最上面那个框里），没检出的写
 * `-- kHz  -- mV` —— 框与单位是**常驻**的（用户要求：没信号也要把框和单位显示出来），
 * 这样"1 基波 + 4 谐波 = 5 根柱"在页面上恒有对应，空框反而像页面缺东西。
 * 数值走 `set_text` 而**不走** `roll_value`：五框同滚 = 五处动画同时跑，而这一页
 * 的主角动画是"柱在长"（见 refresh_freq）—— 读者看的是柱，不是数字的淡入。 */
static void harm_boxes_update(const meas_result_t *r) {
    const int n = (int)r->n_peaks;

    for (int i = 0; i < MEAS_MAX_PEAKS; i++) {
        if (i >= n) {
            set_text(s_box_val[i], "-- kHz  -- mV"); /* 没检出 ⇒ 留框留单位 */
            continue;
        }
        char fq[16];
        char am[16];
        char line[40];
        fmt_dec1(fq, sizeof(fq), r->peak[i].freq_hz / 1000.0f);
        fmt_dec1(am, sizeof(am), r->peak[i].amp_mv);
        (void)snprintf(line, sizeof(line), "%s kHz  %s mV", fq, am);
        set_text(s_box_val[i], line);
    }
}

static void refresh_freq(const meas_result_t *r) {
    op_spec_line_t s[MEAS_MAX_PEAKS];
    const int n = op_spec_build(r, s, MEAS_MAX_PEAKS); /* 未检出/参数非法 → -1 */
    const float span = (float)(UI_SPEC_X1 - UI_SPEC_X0);

    for (int i = 0; i < MEAS_MAX_PEAKS; i++) {
        const int on = (i < n);
        lv_coord_t h = 0;
        lv_coord_t x = UI_SPEC_X0;
        if (on) {
            x = (lv_coord_t)(UI_SPEC_X0 + s[i].x01 * span + 0.5f);
            h = (lv_coord_t)(s[i].len01 * (float)UI_BAR_MAX_H + 0.5f);
        }
        lv_obj_set_x(s_bar[i], (lv_coord_t)(x - UI_BAR_W / 2)); /* 位置即时跟 */
        set_hidden(s_bar[i], !on);
        if (lv_obj_get_height(s_bar[i]) == h) {
            continue;
        }
        /* ★ 2026-10-14 八轮（P2.1）：**直写高度，不再走 300ms 动画**。
         *   为什么：数据每 100ms 就来一次，而动画要 300ms ⇒ 每次都被
         *   `lv_anim_delete` + 重启 ⇒ 动画**永远跑不完**，柱区按重绘节拍（10ms）
         *   持续重绘（5 柱 × 12px × h≤180 ≈ 11 kpx/帧 × ~78 帧/s ≈ **0.86 Mpx/s**，
         *   与整个波形区同量级）。台面上"输入悬空 ⇒ 没柱"所以看不出来，**接上信号源必现形**。
         *   与六轮"数字不滚入"（见 roll_value 上方的长注释）同一个决定：数据 5~10Hz 时，
         *   任何长于更新周期的动画都只是白付的重绘。 */
        bar_grow_cb(s_bar[i], (int32_t)h); /* 回调做的正是动画末帧的事：同改 height 与 y */
    }

    harm_boxes_update(r);
}

/* ============================================================ 页 2 · 标定 */
static void cal_overlay_cb(lv_event_t *e);

static void cal_prompt_refresh(void) {
    char b[16];
    fmt_dec1(b, sizeof(b), app_engine_cal_freq(app_engine_cal_index()) / 1000.0f);
    char txt[96]; /* 最坏 "Set the source to\n100 mV sine @ <最长频点> kHz\nthen press MEAS." ≈ 66 B */
    (void)snprintf(txt, sizeof(txt),
                   "Set the source to\n100 mV sine @ %s kHz\nthen press MEAS.", b);
    set_text(s_cal_prompt, txt);
}

static void cal_sync_selftest(void) {
    app_st_item_t st[APP_ST_MAX];
    const int n = app_selftest_snapshot(st, APP_ST_MAX);
    if (n < 0) {
        return;
    }
    /* **一个** label 打完"表头 + 8 行"：每行 = 名字（次文字色）+ 明写的
     * `PASS` / `FAIL`（重着色：通过=强调色、未过=主文字色）。
     * ★ 四轮把"状态点"改成**明写 PASS / FAIL**：三轮那版是个小圆点，
     *   实测没人看得懂（用户原话："亮了就通过没亮就是失败？？"）——
     *   状态必须是**词**，不是一颗要靠猜的色块。表头顺手报通过数 `7/8`。 */
    int ok = 0;
    for (int i = 0; i < n; i++) {
        if (st[i].ok) {
            ok++;
        }
    }
    char buf[APP_ST_MAX * 32 + 24];
    (void)snprintf(buf, sizeof(buf), "SELF TEST   %d/%d", ok, n);
    size_t o = lv_strlen(buf);
    for (int i = 0; i < n && o < sizeof(buf) - 1U; i++) {
        /* `%-10s` 把名字撑到 10 **字符**（不是像素）⇒ 状态词落在同一列附近。
         * montserrat 是变宽字，仍会有几像素抖动 —— 诊断表，够用。 */
        const char *tag = st[i].ok ? "#4cc9f0 PASS#" : "#edf2f7 FAIL#";
        const int w = snprintf(buf + o, sizeof(buf) - o, "\n%-10s%s", st[i].name, tag);
        if (w < 0 || (size_t)w >= sizeof(buf) - o) {
            break;
        }
        o += (size_t)w;
    }
    set_text(s_st_list, buf);
}

static void page_cal_build(lv_obj_t *pg) {
    header_create(pg, "CALIBRATION", s_icon_cal, 3U);

    /* 左半：上电自检（首行 "SELF TEST" 直接做表头，省一个对象）*/
    s_st_list = label_create(pg, "SELF TEST", &s_st_cap, UI_PAD, UI_CONTENT_Y);
    lv_obj_set_style_text_line_space(s_st_list, 3, LV_PART_MAIN);
    lv_label_set_recolor(s_st_list, true);

    /* 右半：CAL OK / NO CAL 大字 + 一个**点一下就进**的标定按钮。
     * ★ 四轮把"长按"改成"点击"：三轮绑的是 `LV_EVENT_LONG_PRESSED`，用户反馈
     *   "点击 hold to cal 没有任何反应" —— 防误触那点收益抵不过"按了没动静"带来
     *   的"这功能是不是坏的"。覆盖层里本来就有 CLOSE，误进一次退出来即可。 */
    s_cal_state = label_create(pg, "NO CAL", &s_st_val, 248, 52);

    lv_obj_t *btn = lv_obj_create(pg);
    lv_obj_set_pos(btn, 248, 116);
    lv_obj_set_size(btn, 216, 44);
    lv_obj_add_style(btn, &s_st_bare, LV_PART_MAIN);
    lv_obj_add_style(btn, &s_st_btn, LV_PART_MAIN);
    lv_obj_add_style(btn, &s_st_btn_press, LV_STATE_PRESSED);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, cal_overlay_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *hl = lv_label_create(btn);
    lv_label_set_text(hl, "CALIBRATE"); /* 点了就进 ⇒ 别再写 HOLD */
    lv_obj_add_style(hl, &s_st_txt12, LV_PART_MAIN); /* 只给字号：颜色继承父对象 */
    lv_obj_center(hl);

    /* 左下：带宽读数（口径 = **像素吞吐 ÷ 整帧**，与 sysmon 的重绘帧率不是一回事）*/
    lv_obj_t *c = glass_card_create(pg, UI_PAD, UI_CARD_Y, UI_BW_W, UI_CARD_H);
    /* 八轮：`card_value_create()` 已删（只有一个调用点，另三张卡是同款两行手写）——
     * 统一成"标签 + 数值"两行直写，少一层只转发一次的包装。 */
    (void)label_create(c, "BANDWIDTH (Mpx/s)", &s_st_cap, UI_PAD, 8);
    s_cal_bw = label_create(c, "--", &s_st_val, UI_PAD, 18);
}

static void refresh_cal(uint32_t now_ms) {
    static uint32_t s_st_ms = 0;
    static uint32_t s_bw_ms = 0;
    static uint8_t s_loaded = 0xFF;

    const uint8_t loaded = app_engine_cal_loaded() ? 1U : 0U;
    if (loaded != s_loaded) {
        s_loaded = loaded;
        set_text(s_cal_state, loaded ? "CAL OK" : "NO CAL");
        lv_obj_set_style_text_color(s_cal_state,
                                    UI_HEX(loaded ? UI_C_ACCENT : UI_C_DIM), LV_PART_MAIN);
    }
    if ((uint32_t)(now_ms - s_st_ms) >= 200U) { /* 自检是"边跑边填"的，要定时重读 */
        s_st_ms = now_ms;
        cal_sync_selftest();
    }
    if ((uint32_t)(now_ms - s_bw_ms) >= 1000U) {
        s_bw_ms = now_ms;
        char b[24];
        /* ★ 八轮：口径换成"窗口法的 kpx/s"（旧 `app_lvgl_fps_x10` 读的累计值不可信） */
        fmt_dec1(b, sizeof(b), (float)app_lvgl_flush_kpx_s(now_ms) / 1000.0f);
        roll_value(s_cal_bw, b);
    }
}

/* ============================================ 标定覆盖层（点 CALIBRATE 进） */
static void cal_panel_show(int show) {
    if (show != 0) {
        lv_obj_remove_flag(s_cal_panel, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_cal_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

static void cal_overlay_cb(lv_event_t *e) {
    LV_UNUSED(e);
    app_engine_cal_begin();
    cal_prompt_refresh();
    char b[24];
    (void)snprintf(b, sizeof(b), "step 0 / %d", app_engine_cal_points());
    set_text(s_cal_progress, b);
    cal_panel_show(1);
}

static void cal_close_cb(lv_event_t *e) {
    LV_UNUSED(e);
    cal_panel_show(0);
}

static void cal_meas_cb(lv_event_t *e) {
    LV_UNUSED(e);
    char b[32];
    const int left = app_engine_cal_step(); /* 同步 ~40ms（标定仪式流程可接受）*/
    if (left < 0) {
        set_text(s_cal_progress, "no peak! retry");
        return; /* 不前进：人检查信号源后重按 */
    }
    if (left == 0) {
        (void)snprintf(b, sizeof(b), "step %d / %d", app_engine_cal_points(),
                       app_engine_cal_points());
        set_text(s_cal_progress, b);
        set_text(s_cal_prompt, (app_engine_cal_save() == 0) ? "Done - saved to flash."
                                                           : "Done - SAVE FAILED.");
        return;
    }
    (void)snprintf(b, sizeof(b), "step %d / %d", app_engine_cal_points() - left,
                   app_engine_cal_points());
    set_text(s_cal_progress, b);
    cal_prompt_refresh();
}

/* 文字按钮（标定覆盖层与开机页共用）：无按钮色块，只有 1px 描边；按下时
 * 文字亮一档 + 描边点亮成强调色（`s_st_btn_press`）—— 这就是"点击反应"。 */
static void text_btn_create(lv_obj_t *parent, const char *txt, lv_coord_t x,
                            lv_coord_t y, lv_coord_t w, lv_coord_t h,
                            lv_event_cb_t cb) {
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_add_style(b, &s_st_bare, LV_PART_MAIN);
    lv_obj_add_style(b, &s_st_btn, LV_PART_MAIN);
    lv_obj_add_style(b, &s_st_btn_press, LV_STATE_PRESSED);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_add_style(l, &s_st_txt12, LV_PART_MAIN); /* 颜色继承父对象 ⇒ 按下态生效 */
    lv_obj_center(l);
}

static void cal_panel_create(lv_obj_t *scr) {
    s_cal_panel = lv_obj_create(scr);
    lv_obj_set_pos(s_cal_panel, 0, 0);
    lv_obj_set_size(s_cal_panel, UI_W, UI_H);
    lv_obj_add_style(s_cal_panel, &s_st_wall, LV_PART_MAIN);
    lv_obj_remove_flag(s_cal_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_cal_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_cal_panel);

    lv_obj_t *c = glass_card_create(s_cal_panel, UI_PAD, 70,
                                    (lv_coord_t)(UI_W - 2 * UI_PAD), 180);
    s_cal_progress = label_create(c, "step 0 / 17", &s_st_cap, UI_PAD, 14);
    s_cal_prompt = label_create(c, "", &s_st_cap, UI_PAD, 46);
    lv_obj_set_style_text_font(s_cal_prompt, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_cal_prompt, UI_HEX(UI_C_TEXT), LV_PART_MAIN);
    lv_label_set_long_mode(s_cal_prompt, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_cal_prompt, (lv_coord_t)(UI_W - 2 * UI_PAD - 2 * UI_PAD));

    text_btn_create(c, "MEAS", UI_PAD, 118, 120, 44, cal_meas_cb);
    text_btn_create(c, "CLOSE", (lv_coord_t)(UI_PAD + 152), 118, 120, 44, cal_close_cb);
}

/* ================================================================ splash
 * 开机页：纯黑底 + `G2026 / SIGNAL ANALYZER` + 一个 `TAP TO START` 按钮，
 * **点任意位置（含按钮）才进主界面**（画面定格的这段时间是"产品在等人"，
 * 不是"加载中" —— 所以不给进度条、不给倒计时、不自动消失）。
 * ⚠️ 不做全屏 alpha 淡出：8080 直写下全屏混合就是帧率杀手。 */
static void splash_tap_cb(lv_event_t *e) {
    LV_UNUSED(e);
    /* 在自身事件回调里删自己 —— LVGL 官方支持这条路：对象析构里会
     * `lv_event_mark_deleted()` 把整条事件链标掉（lv_obj.c）。 */
    lv_obj_delete(s_splash);
    s_splash = NULL; /* 置空后 `app_ui_poll` 立刻恢复刷业务页 */
}

static void splash_create(lv_obj_t *scr) {
    s_splash = lv_obj_create(scr);
    lv_obj_set_pos(s_splash, 0, 0);
    lv_obj_set_size(s_splash, UI_W, UI_H);
    lv_obj_add_style(s_splash, &s_st_wall, LV_PART_MAIN);
    lv_obj_remove_flag(s_splash, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_splash, LV_OBJ_FLAG_CLICKABLE); /* 整屏可点 */
    lv_obj_add_event_cb(s_splash, splash_tap_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_move_foreground(s_splash);

    /* 子 label 默认**不可点**（lv_label.c 的构造函数里 remove CLICKABLE）⇒
     * 点在字上也会落到 splash 自己身上，不必逐个去关。 */
    lv_obj_t *t1 = lv_label_create(s_splash);
    lv_label_set_text(t1, "G2026");
    lv_obj_add_style(t1, &s_st_val, LV_PART_MAIN);
    lv_obj_set_style_text_color(t1, UI_HEX(UI_C_ACCENT), LV_PART_MAIN);
    lv_obj_align(t1, LV_ALIGN_CENTER, 0, -34);
    lv_obj_t *t2 = lv_label_create(s_splash);
    lv_label_set_text(t2, "SIGNAL ANALYZER");
    lv_obj_add_style(t2, &s_st_cap, LV_PART_MAIN);
    lv_obj_align(t2, LV_ALIGN_CENTER, 0, 6);

    /* 三轮改成**看得出是个"能按的东西"**：1px 描边的文字按钮，按下时文字亮
     * 一档、描边点亮成强调色。整屏仍然可点（题面要的是"点任意位置"），这个
     * 按钮负责把"可以点"说明白 —— 它是可用性零件，不是装饰。 */
    text_btn_create(s_splash, "TAP TO START", (lv_coord_t)((UI_W - UI_START_W) / 2), 230,
                    UI_START_W, UI_START_H, splash_tap_cb);
}

/* ============================================================== 对外入口 */
static uint8_t ui_page(void) {
    lv_obj_t *act = lv_tileview_get_tile_active(s_tv);
    for (uint8_t i = 0; i < UI_PAGE_N; i++) {
        if (act == s_tiles[i]) {
            return i;
        }
    }
    return 0;
}

/* ==================================================== 页面生命周期（2026-10-14）
 * 目标：**按需加载 + 用后销毁** —— 不再在开机时把三页全建出来。
 *
 * ---- 为什么 tile 常驻、只销毁"内容" ----
 *  `lv_tileview` 靠 tile 对象算布局与滚动行程（删了 tile 会破坏滚动条与坐标），
 *  而 tile 只是 3 个空容器（每个几十字节）；**内容**（几十个 obj/label/line）才是
 *  吃 32KB 池的大头 ⇒ 销毁内容就拿到了全部收益，零风险。
 *
 * ---- ★★ 时机是"切换流畅"的全部秘密 ----
 *  · **建得早**：在 `tile_changed_cb` 里建。`lv_tileview` 在**拖动过程中**就会把目标页
 *    变成"活动页"并发 `VALUE_CHANGED`（app_ui.c 下面那段注释早就记着这条）
 *    ⇒ **手指还没松就开始建**，等页真正滑到位时早已建好 ⇒ 既看不到空白、也感觉不到停顿。
 *  · **删得晚**：离开后**推迟** `APP_UI_PAGE_FREE_DELAY_MS` 再删 —— 拖动动画期间老页
 *    仍在屏幕上，立刻删就是"半屏空白"。
 *  · 顺序**先建后删**：反了就会出现"老页已删、新页没建"的一帧空白。
 *
 * ---- 为什么"删了重建"不丢状态 ----
 *  本文件的页面状态**全在静态 C 变量里**（波形点 `s_wave_pts/y`、1T/3T `s_cycles`、
 *  自检表…），LVGL 对象只是这些数据的**视图** ⇒ 删视图不丢数据。
 *  ⚠️ 但**"跳过重复更新"的门必须复位**，否则重建出来的新对象不会被灌数据：
 *     见 `page_destroy()` 里对 `s_wave_last_n` / `s_wave_dim` 的复位，
 *     以及 `page_build(2)` 里对 `cal_sync_selftest()` 的直接调用。
 *
 * ---- 安全性依据（都是查过源码/实测的事实）----
 *  · `lv_obj_delete/clean` 会 `lv_event_mark_deleted()` + `lv_anim_delete(obj, NULL)`
 *    （`core/lv_obj.c:590-602`）⇒ **删页时正在跑的滚入/生长动画会被自动清掉**，
 *    不必手动杀（与 `splash_tap_cb` 里"在自己回调里删自己"是同一套机制）。
 *  · 标定覆盖层（`s_cal_panel/progress/prompt`）建在**屏幕**上、由 `s_st_wall` 铺满
 *    且默认 CLICKABLE ⇒ 覆盖层显示时**滑不动页** ⇒ 标定期间不可能销毁页 2。
 *  · 三页的活动性判据只有一个：`ui_page()`；销毁前还要再确认一次。
 */
#define PAGE_BIT(p) ((uint8_t)(1U << (p)))

static uint8_t  s_page_built = 0;            /* bit i = 第 i 页内容已建 */
static void ui_heap_report(void);            /* 定义在后面（tile_changed_cb 之后）；page_lifecycle 要用 */
static uint8_t  s_page_free_pending = 0xFFU; /* 待销毁页号；0xFF = 无 */
static uint32_t s_page_free_at_ms = 0;       /* 销毁到期时刻 */
static const char *const s_page_name[UI_PAGE_N] = {"TIME", "FREQ", "CAL"};

/* 把某一页拥有的共享指针全部置 NULL。
 * ⚠️ 归属按**创建者**分，不是按"看起来像哪页"：
 *    `s_cal_panel/progress/prompt` 由 `cal_panel_create(scr)` 建在**屏幕**上 ⇒
 *    **不属于页 2**，销毁页 2 时必须留着它们（它们自己管显示/隐藏）。 */
static void page_clear_refs(uint8_t p) {
    if (p == 0U) {
        s_wave_line = NULL;
        s_cyc_key = NULL;
        s_cyc_lbl = NULL;
        s_sep[0] = NULL;
        s_sep[1] = NULL;
        for (uint32_t i = 0; i < 3U; i++) {
            s_time_val[i] = NULL;
        }
    } else if (p == 1U) {
        for (uint32_t i = 0; i < MEAS_MAX_PEAKS; i++) {
            s_bar[i] = NULL;
            s_box[i] = NULL;
            s_box_val[i] = NULL;
        }
    } else {
        s_st_list = NULL;
        s_cal_state = NULL;
        s_cal_bw = NULL;
    }
}

static void page_build(uint8_t p) {
    if ((s_page_built & PAGE_BIT(p)) != 0U) {
        return; /* 已建（幂等） */
    }
    /* 它正好是"等着被销毁"的那一页 ⇒ 用户滑回来了，撤销销毁 */
    if (s_page_free_pending == p) {
        s_page_free_pending = 0xFFU;
    }

    lv_mem_monitor_t m0;
    lv_mem_monitor(&m0);
    const uint32_t t0 = bsp_tick_cyc_now();

    switch (p) {
        case 0U:  page_time_build(s_tiles[0]); break;
        case 1U:  page_freq_build(s_tiles[1]); break;
        default:  page_cal_build(s_tiles[2]);  break;
    }
    s_page_built |= PAGE_BIT(p);

    /* ★ 重建后要把"当前状态"灌回新对象：翻页本来就会强制刷新（`app_ui_poll` 的
     *   `page_changed`），但页 2 的自检表是**每 N 秒**才刷的 ⇒ 这里直接同步一次，
     *   否则刚建出来的页 2 会空着等下一个周期。 */
    if (p == 2U) {
        cal_sync_selftest();
    }

    lv_mem_monitor_t m1;
    lv_mem_monitor(&m1);
    const uint32_t used0 = m0.total_size - m0.free_size;
    const uint32_t used1 = m1.total_size - m1.free_size;
    LOG_I("[UI] 页 %u %s **建成**：%lu us · 池 used %lu→%lu B（本页 %ld B）",
          (unsigned)p, s_page_name[p],
          (unsigned long)bsp_tick_cyc_to_us(bsp_tick_cyc_now() - t0),
          (unsigned long)used0, (unsigned long)used1, (long)used1 - (long)used0);
}

static void page_destroy(uint8_t p) {
    if ((s_page_built & PAGE_BIT(p)) == 0U) {
        return; /* 没建（幂等） */
    }
    if (p == ui_page()) {
        return; /* ★ 绝不删正在看的页（调用方判过一次，这是最后一道闸）*/
    }

    lv_mem_monitor_t m0;
    lv_mem_monitor(&m0);
    const uint32_t t0 = bsp_tick_cyc_now();

    /* `lv_obj_clean` = 删掉该 tile 的**全部子对象**；tile 自己留着（tileview 要用）。
     * 动画会被析构自动清掉（依据见本节注释）。 */
    lv_obj_clean(s_tiles[p]);

    s_page_built &= (uint8_t)~PAGE_BIT(p);
    page_clear_refs(p);

    /* ★★ 复位"跳过重复更新"的门（**少了这两行，重建出来的波形页就永远是空的**）：
     *  `wave_apply()` 见 `n == s_wave_last_n` 就早返回，`wave_set_color()` 同理 ——
     *  而新 `lv_line` 上一个点都没有，门却记着"上次已画过"。 */
    if (p == 0U) {
        s_wave_last_n = -1;
        s_wave_dim = -1;
    }

    lv_mem_monitor_t m1;
    lv_mem_monitor(&m1);
    const uint32_t used0 = m0.total_size - m0.free_size;
    const uint32_t used1 = m1.total_size - m1.free_size;
    LOG_I("[UI] 页 %u %s **销毁**：%lu us · 池 used %lu→%lu B（回收 %ld B）",
          (unsigned)p, s_page_name[p],
          (unsigned long)bsp_tick_cyc_to_us(bsp_tick_cyc_now() - t0),
          (unsigned long)used0, (unsigned long)used1, (long)used0 - (long)used1);
}

/* 到期销毁（由 `app_ui_poll` 每拍调一次）。
 *
 * ★★ 2026-10-14 上板实测踩到并改掉的一个漏洞：**别只记"一个待销毁页"**。
 *   第一版用单槽 `s_page_free_pending`，结果快速连滑（0→1→2，每步 <500ms）时，
 *   后一页的排期把前一页的排期**覆盖**掉 ⇒ 日志里"页 0 建成"之后**没有**"页 0 销毁"，
 *   而"页 1 销毁"出现两次 —— 最先离开的那一页**永远不会被回收**，直接违背"用后销毁"。
 * ⇒ 改成"到期就扫描一遍：**除了当前页，全部销毁**"。这样无论滑多快，稳定后驻留的
 *   永远只有当前那一页（也顺带把 `page_destroy` 的幂等性当成了防线）。 */
static void page_lifecycle(uint32_t now_ms) {
    if (s_page_free_pending == 0xFFU) {
        return; /* 没有待销毁的事（驻留已是"只有当前页"）*/
    }
    if ((int32_t)(now_ms - s_page_free_at_ms) < 0) {
        return; /* 还没到期：让翻页动画把老页走完 */
    }
    s_page_free_pending = 0xFFU;

    const uint8_t cur = ui_page();
    for (uint8_t p = 0; p < UI_PAGE_N; p++) {
        if (p != cur) {
            page_destroy(p); /* 内部幂等，且会再确认"它不是当前页" */
        }
    }
}

static void tile_changed_cb(lv_event_t *e) {
    LV_UNUSED(e);
    /* ★ 只在"活动页真的变了"时才重测（2026-10-13 四轮）。
     *   为什么不能无条件重测：tileview 在**拖动过程中**会反复发 VALUE_CHANGED
     *   （手指蹭、或触摸卡住一直在拖），而每次重测 = 唤醒 engine 跑一整帧
     *   （ADC 2ms + DSP ~15ms）。重测风暴会把 engine 打到接近满载，UI 任务又被
     *   "采集窗静默"挡在门外 ⇒ 板上现象就是**帧率掉、CPU 满、触摸没反应**。 */
    static uint8_t s_last_page = 0xFFU;
    const uint8_t page = ui_page();
    if (page == s_last_page) {
        return;
    }
    /* ★ 2026-10-14 页面生命周期：**先建新页、再排期销毁老页**（顺序不能反 ——
     *   反了就会有一帧"老页已删、新页没建"的空白）。销毁只是**排期**，到期由
     *   `page_lifecycle()` 执行（拖动动画期间老页还在屏幕上，不能立刻删）。 */
    if (s_last_page != 0xFFU) {
        s_page_free_pending = s_last_page;
        s_page_free_at_ms = bsp_tick_ms() + APP_UI_PAGE_FREE_DELAY_MS;
    }
    s_last_page = page;
    page_build(page);
    app_engine_remeasure();
}

static void ui_heap_report(void) {
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    LOG_I("[UI] LVGL 堆：used=%u free=%u maxblk=%u（池合计 %u B：CCM 主池 + SRAM1 扩展）",
          (unsigned)(mon.total_size - mon.free_size), (unsigned)mon.free_size,
          (unsigned)mon.free_biggest_size, (unsigned)mon.total_size);
    /* ★ 余量告警（红线 2KB）。why 要有它：`lv_malloc` 失败**只按 INFO 打**，而本工程
     *   日志级 = WARN ⇒ 池耗尽是**静默**的，现场表现是"跑一会儿屏卡死/界面缺块"。
     *   一次性报出来，比事后拿 J-Link 猜便宜得多。
     *   ⚠️ 阈值 2KB 是与板上预算对齐的：界面 ≈27.5KB + 板侧基线 ≈4KB + 扩展 4KB
     *      ⇒ 健康时 free ≈ 2.5~3.5KB（见 board_config.h 的 BOARD_LVGL_MEM_EXT_KB）。 */
    if (mon.free_size < 2048U) {
        LOG_W("[UI] LVGL 堆只剩 %u B —— 再加对象就会静默失败（屏会缺块/卡死）！",
              (unsigned)mon.free_size);
    }
    /* ★ 2026-10-14：**按页报驻留**（需求 5 的判据）—— `built` 位图说明"现在还剩哪一页"，
     *   配合建/销日志的两行字节数，就能确认"用后销毁"真的发生了。 */
    LOG_I("[UI] 页驻留：built=0x%02x（bit0=TIME bit1=FREQ bit2=CAL）· 碎片 %u%%",
          (unsigned)s_page_built, (unsigned)mon.frag_pct);

    /* ★★ 2026-10-14：**碎片告警** —— "按需创建 + 用后销毁"这套策略的**固有代价**是池会碎。
     *   实测反复翻页后 `frag` 从 4% 涨到 16~23%（`maxblk` 仍有 20 KB ⇒ 暂时无害）。
     *   加这行是为了**别等它静默变坏**：碎片高到 `maxblk` 装不下一页时，现象是
     *   "某一页建到一半失败、屏上缺块"，而不是崩溃 —— 那种最难查。
     *   真到了这一步，两条路：调大 `APP_UI_PAGE_FREE_DELAY_MS`（少删几次）或放弃销毁。 */
    if (mon.frag_pct >= 40U) {
        LOG_W("[UI] LVGL 池碎片 %u%%（maxblk %u B）—— 反复翻页所致；"
              "若某页开始缺块，把 APP_UI_PAGE_FREE_DELAY_MS 调大或改回不销毁",
              (unsigned)mon.frag_pct, (unsigned)mon.free_biggest_size);
    }
}

int app_ui_init(void) {
    if (s_styles_ready != 0) {
        return 0; /* 幂等：样式表只能 init 一次，重复建也会把 32KB 池吃光 */
    }
    s_styles_ready = 1;
    styles_init();

    lv_obj_t *scr = lv_screen_active();
    /* ★ 黑底靠 `s_st_wall`（`LV_USE_THEME_DEFAULT = 0` ⇒ 屏幕默认**没有任何**
     *   背景样式；若哪天主题被打开，这一步也必须保留，否则会退回白底）。*/
    lv_obj_add_style(scr, &s_st_wall, LV_PART_MAIN);

    s_tv = lv_tileview_create(scr);
    lv_obj_set_pos(s_tv, 0, 0);
    lv_obj_set_size(s_tv, UI_W, UI_H);
    lv_obj_add_style(s_tv, &s_st_bare, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_tv, 0, LV_PART_MAIN); /* 必须 0：tile 位置按内容宽算 */
    lv_obj_set_scrollbar_mode(s_tv, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(s_tv, tile_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    for (uint8_t i = 0; i < UI_PAGE_N; i++) {
        lv_obj_t *t = lv_tileview_add_tile(s_tv, i, 0, LV_DIR_HOR);
        s_tiles[i] = t;
        lv_obj_add_style(t, &s_st_bare, LV_PART_MAIN); /* 透出屏幕黑底 */
        lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE); /* 滚的是 tv，不是 tile */
    }
    /* ★ 2026-10-14 页面生命周期：**只建首页**，其余两页等用户滑过去时再建
     *   （原来三页全在这里建 ⇒ 开机的池占用与建页耗时都是现在的 ~3 倍）。 */
    page_build(0);

    cal_panel_create(scr);
    splash_create(scr);
    ui_heap_report();
    return 0;
}

/* ★ 分段耗时探针（2026-10-13 六轮）：定位"UI 每秒钟有一次 ~215ms 停顿"停在哪一段。
 * app_ui_task 的循环有三段：① 采集窗等待（`while (app_engine_acquiring())`）
 * ② `app_ui_poll`（业务刷新：重建波形 + 数字 + 滚入动画）③ `app_lvgl_poll`
 * （LVGL 时基 + 重绘 + flush + 触摸）。每秒把**本秒最大值**打进 `[UI] 1s` 那行。
 * 判读：三段都小 ⇒ 时间丢在"两次调用之间"（被谁抢了 CPU / 卡在锁上）；
 * 哪一段大 ⇒ 就是那一段的锅。没有这个数就只能猜（本轮已经猜错一次了）。 */
#if UI_STAT_LOG
static uint32_t s_probe[3];
/* ★ 2026-10-14：**UI 循环实际跑了多少次**（每秒）。
 * why：屏上官方 sysmon 报的 FPS 是 `1000×refr_cnt/窗口`，而 `refr_cnt` 只在
 *   `lv_timer_handler()` 里被推进 ⇒ **FPS ≈ 本循环的频率**（上限被 `1000/重绘周期`
 *   夹住）。要判"为什么屏上是 50 而不是 100"，唯一的判据就是本循环到底跑多快 ——
 *   日志里原先**没有**这个数（`[UI] 1s` 的"刷新 N 次"是**业务刷新**次数，不是循环次数）。
 * ⚠️ 临时诊断，定案后连同 `s_probe` 一起删。 */
static uint32_t s_loop_n = 0;
#endif

void app_ui_poll(uint32_t now_ms) {
    /* splash 期间不刷业务（全屏不透明，刷了也看不见，白占带宽）*/
    if (s_splash != NULL) {
        return;
    }

    /* 页面生命周期：把"到期的待销毁页"删掉（推迟销毁的理由见页面生命周期一节）*/
    page_lifecycle(now_ms);

    const uint8_t page = ui_page();
    const meas_result_t *r = app_engine_result();

    /* 业务页：seq 变了或翻了页才刷（其余页惰性 —— 测量周期 500ms）*/
    static uint32_t s_seen_seq = 0xFFFFFFFFU;
    static uint8_t s_seen_page = 0xFF;
    static uint32_t s_last_refr_ms = 0;
    static uint32_t s_stat_refr = 0;
    static uint32_t s_lat_max_ms = 0U; /* 本窗口"发布→决定重绘"最大延迟（见下）*/
    const int page_changed = (page != s_seen_page);
    const int data_changed = (r->seq != s_seen_seq);
    /* ★ 刷新速率闸（2026-10-13 四轮加，2026-10-14 从 100ms 收到 `APP_UI_REFR_MIN_MS`）。
     *   原始动机：engine 因任何原因变成"连续重测"时 `seq` 会满速跳，而本函数每拍会重建
     *   波形并重启动画 ⇒ FSMC 写满、UI 挤不出时间读触摸。**这道闸只该挡突发，不该挡正常 10Hz。**
     * ⚠️ 100 → 20ms 的理由（**用户的延迟要求**）：数据是 10Hz、闸门也是 100ms ⇒
     *   **两个同周期又不同步的计时器会锁在一个任意的固定相位差上**，
     *   最坏给"发布 → 上屏"白加 100ms。见 `project_config.h` 的 `APP_UI_REFR_MIN_MS`。
     * ⚠️ 翻页**不受闸限制**（人手指的动作必须立刻看到）。 */
    const int too_soon = (s_last_refr_ms != 0U) &&
                         ((uint32_t)(now_ms - s_last_refr_ms) < APP_UI_REFR_MIN_MS);

    /* ★ 2026-10-14：**滑动期间冻结业务刷新**（本次为"滑动更流畅"做的那一条）。
     *
     * why：`lv_tileview` 的滑动 = **滚动整屏** ⇒ 每个动画帧整屏 480×320 全部变脏 ⇒
     *   一次整屏重绘要切 **7 个 draw buffer 带**（320 行 ÷ 48 行/带），每带都要
     *   "软件渲染 + flush"，而 flush 的物理下限是 `23040 px × 76.4 ns = 1.76 ms`
     *   ⇒ **一次整屏帧光 FSMC 就 ≥12 ms**。此时若再叠一层"数据刷新"
     *   （`refresh_time` 会重建波形 160 点、重设 `lv_line`、重启滚入动画），
     *   两者抢同一拍 ⇒ 滑动明显掉帧。
     *   滑动只有几百毫秒，冻结业务**肉眼看不出来**：松手后 `tile_changed_cb`
     *   会 `app_engine_remeasure()`，数据立刻跟上（而且 `s_seen_page` 没被更新 ⇒
     *   滑完第一拍 `page_changed` 必为真 ⇒ 一定补刷一次）。
     *
     * 判据用 `lv_obj_is_scrolling()`：它**同时覆盖**"跟手拖动中"与"松手后的惯性滑行"
     *   （见 `lv_obj_scroll.c` —— 既查 indev 的 `scroll_obj`，也查 `scroll_x/y_anim`）。
     * ⚠️ 只挡**业务刷新**，**不挡** LVGL 自己的重绘 —— 滚动本身必须继续画。 */
    const int scrolling = (s_tv != NULL) && lv_obj_is_scrolling(s_tv);

    if (!scrolling && (page_changed || data_changed) && (!too_soon || page_changed)) {
        /* ★ 2026-10-14：量"**发布 → 决定重绘**"这段延迟（用户要求：接入信号要及时刷出来）。
         *   只在**真的因新数据而刷**时量（翻页不算），取本窗口最大值。
         *   ⚠️ 若因为 `too_soon` 被挡下，`s_seen_seq` 不更新 ⇒ `data_changed` 保持为真
         *      ⇒ 下一拍会带着**更长的延迟**再进来 ⇒ 最大值自然捕捉到"被闸门拖了多久"。 */
        if (data_changed) {
            const uint32_t pub = app_engine_pub_ms();
            if (pub != 0U) {
                const uint32_t lat = (uint32_t)(now_ms - pub);
                if (lat > s_lat_max_ms) {
                    s_lat_max_ms = lat;
                }
            }
        }
        s_seen_seq = r->seq;
        s_seen_page = page;
        s_last_refr_ms = now_ms;
        s_stat_refr++;
        if (page == 0) {
            refresh_time(r);
        } else if (page == 1) {
            refresh_freq(r);
        }
    }
    if (page >= 2) {
        /* ★ 八轮（P2.3）：覆盖层（不透明整屏）盖着时**不刷页 2** —— 刷了也看不见，
         *   而自检清单那把 recolor 一次是 ~60 kpx。判据：覆盖层可见就整段跳过。
         * ★ 2026-10-14：滑动期间同理跳过（理由见上面 `scrolling` 段）。 */
        if (!scrolling && (s_cal_panel == NULL || lv_obj_has_flag(s_cal_panel, LV_OBJ_FLAG_HIDDEN))) {
            refresh_cal(now_ms); /* 页 2 每拍（自检边跑边填 + 带宽读数，内部自带节流）*/
        }
    }

    /* ★ 每秒一行"黑匣子"（2026-10-13 四轮）：板上出"帧率掉/CPU 满/触摸不动"
     *   时，这一行直接在 RTT 里回答三件事 —— 屏按什么节奏刷、页有没有在抖、
     *   池还剩多少。没有它，这类故障只能靠猜（本轮就是）。 */
#if UI_STAT_LOG
    static uint32_t s_stat_ms = 0;
    static uint32_t s_gap_ms = 0;  /* 本窗口内相邻两次 poll 的**最大间隔** */
    static uint32_t s_last_ms = 0;
    /* ★ "最大间隔" = "UI 有没有被别人插队"的直接证据（2026-10-13 六轮加）：
     *   引擎的 DSP 是 40ms 纯计算 —— 若它的优先级高于 UI，这个值会被顶到 40+ms，
     *   现象正是"波形从上到下逐带刷、一卡一卡"（每两条渲染带之间被插一刀）。
     *   修好后应贴着 UI 自己的节拍（10ms 级）。判据写进日志，别靠肉眼。 */
    /* ★ 2026-10-14：**再加一个"间隔分布"**（4 桶）。
     *   why：`最大间隔` 只回答"**最坏**有多坏"，回答不了"**多久坏一次**" ——
     *   而"每 100ms 顿一下"和"每 5 秒顿一下"是完全不同的体验、也指向完全不同的根因。
     *   **"显示流畅度"要能验收，靠的就是这个分布**（平均帧率骗人，尖峰频率才是观感）。
     *   分桶按 UI 目标节拍 10ms 划：≤12 正常 / ≤20 掉一拍 / ≤33 掉两拍 / >33 明显卡。 */
    static uint32_t s_gap_bin[4]; /* ≤12 / ≤20 / ≤33 / >33 ms */
    if (s_last_ms != 0U) {
        const uint32_t d = (uint32_t)(now_ms - s_last_ms);
        if (d > s_gap_ms) {
            s_gap_ms = d;
        }
        s_gap_bin[(d <= 12U) ? 0U : ((d <= 20U) ? 1U : ((d <= 33U) ? 2U : 3U))]++;
    }
    s_last_ms = now_ms;
    if (s_stat_ms == 0U) {
        s_stat_ms = now_ms;
    } else if ((uint32_t)(now_ms - s_stat_ms) >= 1000U) {
        s_stat_ms = now_ms;
        /* ★ 八轮（P2.5）：堆读数**每 5s 才真遍历一次池**（`lv_mem_monitor` 要走空闲链），
         *   1s 这一行其余字段照打 —— 判"堆有没有在漏"用 5s 粒度足够。 */
        static uint32_t s_mon_ms = 0U;
        static uint32_t s_mon_free = 0U;
        if (s_mon_ms == 0U || (uint32_t)(now_ms - s_mon_ms) >= 5000U) {
            lv_mem_monitor_t mon;
            lv_mem_monitor(&mon);
            s_mon_ms = now_ms;
            s_mon_free = (uint32_t)mon.free_size;
        }
        LOG_I("[UI] 1s: 刷新 %lu 次 · 循环 %lu 次/s · 最大间隔 %lu ms"
              " · 间隔分布 ≤12/≤20/≤33/>33 = %lu/%lu/%lu/%lu",
              (unsigned long)s_stat_refr, (unsigned long)s_loop_n,
              (unsigned long)s_gap_ms,
              (unsigned long)s_gap_bin[0], (unsigned long)s_gap_bin[1],
              (unsigned long)s_gap_bin[2], (unsigned long)s_gap_bin[3]);
        /* ★★ 2026-10-14：**拆成两行** —— 原来这行 > 160 B，尾部（`分段`/`页`/`堆`）被
         *   `Common/Src/log.c` 的 `LOG_LINE_MAX` 静默截断 ⇒ 那几个字段**一直在骗人**：
         *   `分段 等待/业务/LVGL` 只剩 `分段 等`、`页` 与 `堆` 整段消失。
         *   ⚠️ 纪律：带数字的日志行不许超过 160 B，装不下就分行（见 `log.c` 的 `~` 记号）。 */
        LOG_I("[UI] 1s: 发布→重绘 max %lu ms · 分段 等待/业务/LVGL %lu/%lu/%lu ms"
              " · 页 %u · seq %lu · 堆 free %u B",
              (unsigned long)s_lat_max_ms,
              (unsigned long)s_probe[0], (unsigned long)s_probe[1],
              (unsigned long)s_probe[2], (unsigned)page, (unsigned long)r->seq,
              (unsigned)s_mon_free);
        s_stat_refr = 0U;
        s_loop_n = 0U;
        s_gap_ms = 0U;
        s_lat_max_ms = 0U;
        s_gap_bin[0] = 0U;
        s_gap_bin[1] = 0U;
        s_gap_bin[2] = 0U;
        s_gap_bin[3] = 0U;
        s_probe[0] = 0U;
        s_probe[1] = 0U;
        s_probe[2] = 0U;
    }
#endif
}

/* ============================================================ UI 任务体 ==== */
void app_ui_task(void *arg) {
    (void)arg;
    LOG_I("[UI] 界面任务启动（prio=%u, stack=%u words）", (unsigned)APP_TASK_UI_PRIORITY,
          (unsigned)APP_TASK_UI_STACK_WORDS);

    /* 节拍锚点（`vTaskDelayUntil` 的"上一次唤醒时刻"）。
     * ⚠️ 必须在**循环外**取一次 —— 放进循环里就退化成 `vTaskDelay`。见循环尾。 */
    TickType_t wake = xTaskGetTickCount();

    for (;;) {
        /* 循环计数（临时诊断，见 s_loop_n 的说明）—— 它直接给出"UI 循环每秒跑几圈"，
         * 也就是屏上 sysmon 那个 FPS 的分母来源。 */
#if UI_STAT_LOG
        s_loop_n++;
#endif
        /* ---- 采集窗静默（硬件方案 02 §6 O1）----
         * engine 正在等 DMA 搬满时**不碰 FSMC**：LCD 总线的开关噪声是板内最大
         * 干扰源，而它正好会灌进同一时刻在采样的模拟前端。
         *
         * ★ 2026-10-14 订正（原文把优先级方向写反了）：旧注释说"engine 优先级更高，
         *   它采集中时本任务本来也抢不到 CPU" —— **不成立**：engine(25) 比本任务(26)
         *   **低**（六轮为消掉"波形逐带刷"主动降下来的，见 project_config.h）。
         *   真实情形相反：engine 阻塞在 ADC 帧信号量上时，CPU 必然回到本任务。
         *   ⇒ 避让只能**由本任务自己做**，优先级不替我们挡这一刀。
         *
         *   两道保险，职责不同（别把任一条当成冗余删掉）：
         *     ① 本循环（这一道）= **不起 flush**：采样窗内本任务根本不去调
         *        `app_ui_poll`/`app_lvgl_poll` ⇒ 一次 flush 都不会被"拦腰截断"，
         *        代价只是屏晚刷 ≤2ms。这是**延迟优化**。
         *     ② `bsp_lcd_bus_take()`（写屏侧每块抢一次，超时 `APP_LCD_BUS_WAIT_MS`
         *        =20ms > 采样窗 2.048ms）= **正确性**那一道：就算真起了一个跨窗的
         *        flush，它也只能**等窗口过去再写**（超时才退化成"照常写"）。
         *        engine 侧在**开采集之前**就把闸拿到手 ⇒ "拿到闸"才等于"窗口开始"。
         *   采集窗只有 2.048ms，①的代价可忽略。 */
        const uint32_t t0 = bsp_tick_ms();
        while (app_engine_acquiring()) {
            vTaskDelay(1U);
        }
        const uint32_t t1 = bsp_tick_ms();

        app_ui_poll(t1);  /* 业务页刷新（读结果 mailbox）*/
        const uint32_t t2 = bsp_tick_ms();
        app_lvgl_poll(t2); /* LVGL 时基 + 重绘 + 读触摸 */
        const uint32_t t3 = bsp_tick_ms();

#if UI_STAT_LOG
        {   /* 分段耗时（本秒取最大）—— 判读见 s_probe 的注释 */
            const uint32_t d[3] = {t1 - t0, t2 - t1, t3 - t2};
            for (int i = 0; i < 3; i++) {
                if (d[i] > s_probe[i]) {
                    s_probe[i] = d[i];
                }
            }
        }
#endif

        /* ★ 2026-10-14：`vTaskDelay` → **`vTaskDelayUntil`**（本工程第一处用它）。
         *   why：`vTaskDelay` 是**相对延时** ⇒ 实际周期 = 设定值 + 本次工作耗时，
         *   每一拍都把"本拍干了多久"累加进节拍。而本延时**就是** LVGL 的时间分辨率
         *   （`app_lvgl_poll` 按两次调用的差值推进 `lv_tick_inc`），也是官方 sysmon
         *   报 FPS 上限的分母（`1000/period`）⇒ 漂移会让"报的上限"与"实际节拍"
         *   变成两个口径。`vTaskDelayUntil` 睡到**绝对时刻**，工作耗时藏在周期内，
         *   节拍是真 10ms 网格。
         * ⚠️ 锚点 `wake` 在**循环外**取一次（见函数开头）—— 放进循环里就退化。
         * ⚠️ 若某拍工作 >10ms，它**立即返回不睡** ⇒ 自然追赶、不会永久漂后；
         *   这正是要的行为：LVGL 的 dt 取实际差值，追赶不会让动画跳变。
         * ⚠️ 本延时**就是** LVGL 的时间分辨率，改成大于 16ms 会掉帧 —— 见
         *   project_config.h 的 `APP_TICK_PERIOD_MS` / `APP_LVGL_REFR_PERIOD_MS`。 */
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(APP_TICK_PERIOD_MS));
    }
}
