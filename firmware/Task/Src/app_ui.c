/* ============================================================================
 *  app_ui.c —— 测量 UI（LVGL 9.5）· 见 app_ui.h
 *
 *  ---- 架构（为什么这样组织）----
 *  固定坐标 + 三个顶层区：顶栏（20px 状态）/ 横向 scroll 容器（424px 四页）/
 *  页签条（36px）。**页面切换用 LVGL 原生 scroll 动画**：手指直接滑页，
 *  滑动中 LVGL 只重绘暴露条带 —— 这是触摸丝滑的核心，也是重绘带宽最省的
 *  切页方式（对比：全屏 alpha 过渡要混合 300KB，8080 直写带宽下必卡）。
 *
 *  ---- 动效清单（全部小区域，见方案的性能预算）----
 *    · 页签指示条：x 平移 300ms ease-out（3px 高条，重绘 ~1KB）
 *    · 谱线柱：高度 lv_anim 300ms（28px 宽柱，从基线向上长）
 *    · 数值更新：文字青→白→青 150ms（一行 label）
 *    · splash：进度条 800ms，一次性（开机第一印象，之后不再有全屏动效）
 *
 *  ---- 数据 ----
 *  只读 app_engine_result()，按 seq 判断刷新；不碰测量算法（分层）。
 * ==========================================================================*/
#include "app_ui.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>

#include "app_engine.h"
#include "app_lvgl.h"
#include "bsp_tick.h" /* 单调毫秒（UI 任务的时间口径，与 LVGL 同源）*/
#include "log.h"
#include "meas_result.h"
#include "op_spec.h"
#include "op_wave.h"
#include "project_config.h"

#include "FreeRTOS.h"
#include "task.h"

#include "lvgl.h"

/* ---- 布局常量（屏 320×480 竖屏，board_config.h；这些是"画法"不是产品参数）*/
#define UI_SCR_W 320
#define UI_SCR_H 480
#define UI_TOP_H 20
#define UI_TAB_H 36
#define UI_PAGE_H (UI_SCR_H - UI_TOP_H - UI_TAB_H) /* 424 */
#define UI_PAGE_N 4
#define UI_SPEC_BASE_Y (UI_PAGE_H - 46) /* 谱线页基线（柱从这里向上长） */

/* ---- 色板（方案定稿的深色科技风）---- */
#define UI_C_BG 0x0B0F14U
#define UI_C_CARD 0x151B23U
#define UI_C_BORDER 0x232C38U
#define UI_C_PRIMARY 0x4CC9F0U
#define UI_C_ACCENT 0x06D6A0U
#define UI_C_AMBER 0xFFD166U
#define UI_C_TEXT 0xE8EDF2U
#define UI_C_DIM 0x8D99AEU

/* ---- 静态点数组（lv_line 持有指针 ⇒ 必须常驻）---- */
static lv_point_precise_t s_wave_pts[APP_UI_WAVE_POINTS];
static lv_point_precise_t s_glow_pts[APP_UI_WAVE_POINTS]; /* 发光层共享形状 */
static lv_point_precise_t s_grid_pts[3][2];
static lv_point_precise_t s_spec_base_pts[2];

/* ---- 对象句柄 ---- */
static lv_obj_t *s_pages[UI_PAGE_N];
static lv_obj_t *s_scroller;
static lv_obj_t *s_tab_btns[UI_PAGE_N];
static lv_obj_t *s_tab_ind;   /* 页签指示条（3px，x 动画） */
static lv_obj_t *s_scene_btn; /* 顶栏场景徽标 */
static lv_obj_t *s_cal_dot;   /* 顶栏标定状态点 */
static lv_obj_t *s_fps_lbl;   /* 顶栏等效 fps */
static lv_obj_t *s_wave_line;
static lv_obj_t *s_glow_line;
static lv_obj_t *s_wave_title;
static lv_obj_t *s_val_lbls[3];
static lv_obj_t *s_bars[MEAS_MAX_PEAKS];
static lv_obj_t *s_list_rows[MEAS_MAX_PEAKS];
static lv_obj_t *s_cal_panel;
static lv_obj_t *s_cal_prompt;
static lv_obj_t *s_cal_progress;
static lv_obj_t *s_splash;
static lv_obj_t *s_splash_bar;
static uint8_t   s_page = 0;
static int       s_cycles = 1; /* 波形 1T / 3T */

/* ---- 定点格式化（printf 浮点不可靠，docs/09）---- */
static void fmt_mv(char *buf, size_t cap, float mv) {
    const int v = (int)(mv * 10.0f + (mv >= 0.0f ? 0.5f : -0.5f));
    int frac = v % 10;
    if (frac < 0) {
        frac = -frac;
    }
    (void)snprintf(buf, cap, "%d.%d mV", v / 10, frac);
}

static void fmt_khz(char *buf, size_t cap, float hz) {
    (void)snprintf(buf, cap, "%lu.%02lu kHz", (unsigned long)(hz / 1000.0f),
                   (unsigned long)(((long)(hz / 10.0f)) % 100L));
}

/* ---- 通用小组件 ---- */
static lv_obj_t *card_create(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                             lv_coord_t w, lv_coord_t h) {
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_bg_color(c, lv_color_hex(UI_C_CARD), LV_PART_MAIN);
    lv_obj_set_style_border_color(c, lv_color_hex(UI_C_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_width(c, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(c, 8, LV_PART_MAIN);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

static lv_obj_t *label_create(lv_obj_t *parent, const char *txt, uint32_t color,
                              const lv_font_t *font, lv_coord_t x, lv_coord_t y) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, lv_color_hex(color), LV_PART_MAIN);
    if (font != NULL) {
        lv_obj_set_style_text_font(l, font, LV_PART_MAIN);
    }
    lv_obj_set_pos(l, x, y);
    return l;
}

/* 数值色闪：更新瞬间白 → 150ms 后回主色（一次性 timer 自删；
 * 比 style 动画省事，重绘区域只有一行 label）。 */
static void flash_back_cb(lv_timer_t *t) {
    lv_obj_set_style_text_color((lv_obj_t *)lv_timer_get_user_data(t), lv_color_hex(UI_C_PRIMARY),
                                LV_PART_MAIN);
    lv_timer_del(t);
}

static void flash_value(lv_obj_t *lbl, const char *txt) {
    lv_label_set_text(lbl, txt);
    lv_obj_set_style_text_color(lbl, lv_color_hex(UI_C_TEXT), LV_PART_MAIN);
    (void)lv_timer_create(flash_back_cb, 150, lbl);
}

/* ===================================================================== 顶栏 */
static void scene_cb(lv_event_t *e) {
    LV_UNUSED(e);
    const uint8_t next = (app_engine_scene() + 1U) % (MEAS_SCENE_UBJ + 1U);
    app_engine_set_scene(next);
    lv_label_set_text(s_scene_btn, (next == MEAS_SCENE_UA) ? "u_a"
                                   : (next == MEAS_SCENE_UB) ? "u_b"
                                                             : "u_b+J");
    app_engine_remeasure();
}

static lv_obj_t *topbar_create(lv_obj_t *scr) {
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_size(bar, UI_SCR_W, UI_TOP_H);
    lv_obj_set_style_bg_color(bar, lv_color_hex(UI_C_CARD), LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bar, 0, LV_PART_MAIN);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    /* 左：场景徽标（点击循环 u_a/u_b/u_b+J —— 算法唯一，docs/01 §7） */
    s_scene_btn = lv_button_create(bar);
    lv_obj_set_size(s_scene_btn, 52, 18);
    lv_obj_set_pos(s_scene_btn, 2, 1);
    lv_obj_set_style_bg_color(s_scene_btn, lv_color_hex(UI_C_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_radius(s_scene_btn, 4, LV_PART_MAIN);
    lv_obj_add_event_cb(s_scene_btn, scene_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = lv_label_create(s_scene_btn);
    lv_label_set_text(sl, "u_a");
    lv_obj_set_style_text_font(sl, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_center(sl);

    /* 中：标定状态（点 + 字，engine 载表结果） */
    s_cal_dot = lv_obj_create(bar);
    lv_obj_set_size(s_cal_dot, 6, 6);
    lv_obj_set_pos(s_cal_dot, 92, 7);
    lv_obj_set_style_radius(s_cal_dot, 3, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_cal_dot, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_cal_dot, lv_color_hex(UI_C_AMBER), LV_PART_MAIN);
    (void)label_create(bar, app_engine_cal_loaded() ? "CAL OK" : "NO CAL",
                       UI_C_DIM, &lv_font_montserrat_12, 104, 3);

    /* 右：等效 fps（每秒结算的像素吞吐口径，性能可观测） */
    s_fps_lbl = label_create(bar, "0.0 fps", UI_C_DIM, &lv_font_montserrat_12,
                             UI_SCR_W - 58, 3);
    return bar;
}

/* ===================================================================== 页签 */
static void tab_cb(lv_event_t *e) {
    lv_obj_t *page = (lv_obj_t *)lv_event_get_user_data(e);
    lv_obj_scroll_to_view(page, LV_ANIM_ON);
}

static void scroll_cb(lv_event_t *e) {
    lv_obj_t *cnt = lv_event_get_target(e);
    const lv_coord_t sx = lv_obj_get_scroll_x(cnt);
    const uint8_t page = (uint8_t)((sx + UI_SCR_W / 2) / UI_SCR_W);
    if (page == s_page || page >= UI_PAGE_N) {
        return;
    }
    s_page = page;
    /* 指示条平移（300ms ease-out，与滚动同节奏；重复 start 覆盖旧动画） */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_tab_ind);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_x);
    lv_anim_set_values(&a, lv_obj_get_x(s_tab_ind),
                       (lv_coord_t)(page * (UI_SCR_W / UI_PAGE_N)));
    lv_anim_set_duration(&a, 300);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
    for (uint8_t i = 0; i < UI_PAGE_N; i++) {
        lv_obj_set_style_text_color(s_tab_btns[i],
                                    lv_color_hex(i == page ? UI_C_PRIMARY : UI_C_DIM),
                                    LV_PART_MAIN);
    }
}

static lv_obj_t *tabbar_create(lv_obj_t *scr) {
    static const char *const names[UI_PAGE_N] = {"WAVE", "PARAM", "SPEC", "LIST"};
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_set_pos(bar, 0, (lv_coord_t)(UI_TOP_H + UI_PAGE_H));
    lv_obj_set_size(bar, UI_SCR_W, UI_TAB_H);
    lv_obj_set_style_bg_color(bar, lv_color_hex(UI_C_CARD), LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bar, 0, LV_PART_MAIN);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    for (uint8_t i = 0; i < UI_PAGE_N; i++) {
        s_tab_btns[i] = lv_button_create(bar);
        lv_obj_set_size(s_tab_btns[i], UI_SCR_W / UI_PAGE_N, UI_TAB_H);
        lv_obj_set_pos(s_tab_btns[i], (lv_coord_t)(i * (UI_SCR_W / UI_PAGE_N)), 0);
        lv_obj_set_style_bg_opa(s_tab_btns[i], LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(s_tab_btns[i], 0, LV_PART_MAIN);
        lv_obj_add_event_cb(s_tab_btns[i], tab_cb, LV_EVENT_CLICKED, s_pages[i]);
        lv_obj_t *l = lv_label_create(s_tab_btns[i]);
        lv_label_set_text(l, names[i]);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_12, LV_PART_MAIN);
        lv_obj_center(l);
    }

    /* 指示条：页签顶部 3px 主色条 —— 页切换时 x 动画（丝滑的方向感） */
    s_tab_ind = lv_obj_create(bar);
    lv_obj_set_size(s_tab_ind, UI_SCR_W / UI_PAGE_N, 3);
    lv_obj_set_pos(s_tab_ind, 0, 0);
    lv_obj_set_style_bg_color(s_tab_ind, lv_color_hex(UI_C_PRIMARY), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_tab_ind, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_tab_ind, 1, LV_PART_MAIN);
    return bar;
}

/* ================================================================ 四个页面 */
static void wave_toggle(void);
static void refresh_wave(const meas_result_t *r);
static void cal_open_cb(lv_event_t *e); /* 定义在"标定覆盖层"段（参数页要先引用） */

static void wave_click_cb(lv_event_t *e) {
    LV_UNUSED(e);
    wave_toggle();
}

static void page_wave_build(lv_obj_t *pg) {
    s_wave_title = label_create(pg, "no signal", UI_C_PRIMARY,
                                &lv_font_montserrat_16, 8, 6);

    /* 网格：中线 + 两条 ±50% 参照线（暗色虚线观感由细线 + 暗色达成） */
    const lv_coord_t mid = UI_PAGE_H / 2 + 10;
    s_grid_pts[0][0] = (lv_point_precise_t){0, mid};
    s_grid_pts[0][1] = (lv_point_precise_t){UI_SCR_W, mid};
    s_grid_pts[1][0] = (lv_point_precise_t){0, mid - UI_PAGE_H / 4};
    s_grid_pts[1][1] = (lv_point_precise_t){UI_SCR_W, mid - UI_PAGE_H / 4};
    s_grid_pts[2][0] = (lv_point_precise_t){0, mid + UI_PAGE_H / 4};
    s_grid_pts[2][1] = (lv_point_precise_t){UI_SCR_W, mid + UI_PAGE_H / 4};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *g = lv_line_create(pg);
        lv_line_set_points(g, s_grid_pts[i], 2U);
        lv_obj_set_style_line_color(g, lv_color_hex(UI_C_BORDER), LV_PART_MAIN);
        lv_obj_set_style_line_width(g, 1, LV_PART_MAIN);
    }

    /* 双层发光波形：底层 4px 30%α 主色晕 + 顶层 2px 琥珀亮线（共享点组）。
     * 初始隐藏 —— 点数组全 0 时画到角落，第一次数据来了再显示。 */
    s_glow_line = lv_line_create(pg);
    lv_line_set_points(s_glow_line, s_glow_pts, APP_UI_WAVE_POINTS);
    lv_obj_set_style_line_width(s_glow_line, 4, LV_PART_MAIN);
    lv_obj_set_style_line_color(s_glow_line, lv_color_hex(UI_C_PRIMARY),
                                LV_PART_MAIN);
    lv_obj_set_style_line_opa(s_glow_line, LV_OPA_30, LV_PART_MAIN);
    lv_obj_add_flag(s_glow_line, LV_OBJ_FLAG_HIDDEN);

    s_wave_line = lv_line_create(pg);
    lv_line_set_points(s_wave_line, s_wave_pts, APP_UI_WAVE_POINTS);
    lv_obj_set_style_line_width(s_wave_line, 2, LV_PART_MAIN);
    lv_obj_set_style_line_color(s_wave_line, lv_color_hex(UI_C_AMBER),
                                LV_PART_MAIN);
    lv_obj_add_flag(s_wave_line, LV_OBJ_FLAG_HIDDEN);

    /* 点击热区：切 1T/3T（透明、盖满绘图区） */
    lv_obj_t *hit = lv_obj_create(pg);
    lv_obj_set_size(hit, UI_SCR_W, UI_PAGE_H - 26);
    lv_obj_set_pos(hit, 0, 26);
    lv_obj_set_style_bg_opa(hit, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(hit, 0, LV_PART_MAIN);
    lv_obj_clear_flag(hit, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(hit, wave_click_cb, LV_EVENT_CLICKED, NULL);
}

/* 波形页点击：1T ↔ 3T（立即换轴重画，不等下一帧测量） */
static void wave_toggle(void) {
    s_cycles = (s_cycles == 1) ? 3 : 1;
    refresh_wave(app_engine_result());
}

/* 参数页：三张卡片（大数字 + 左上标签）+ 底部 CAL 长按 */
static void page_param_build(lv_obj_t *pg) {
    static const struct {
        const char *name;
        lv_coord_t  y;
    } k_rows[3] = {{"UPP", 16}, {"URMS", 130}, {"F1", 244}};

    for (int i = 0; i < 3; i++) {
        lv_obj_t *c = card_create(pg, 16, k_rows[i].y, UI_SCR_W - 32, 96);
        (void)label_create(c, k_rows[i].name, UI_C_DIM, &lv_font_montserrat_12,
                           10, 6);
        s_val_lbls[i] = label_create(c, "--", UI_C_PRIMARY, &lv_font_montserrat_28,
                                     10, 28);
        lv_obj_set_width(s_val_lbls[i], UI_SCR_W - 52);
        lv_obj_set_style_text_align(s_val_lbls[i], LV_TEXT_ALIGN_RIGHT,
                                    LV_PART_MAIN);
    }
    {
        lv_obj_t *b = lv_button_create(pg);
        lv_obj_set_size(b, 140, 44);
        lv_obj_set_pos(b, (UI_SCR_W - 140) / 2, 360);
        lv_obj_set_style_bg_color(b, lv_color_hex(UI_C_CARD), LV_PART_MAIN);
        lv_obj_set_style_border_color(b, lv_color_hex(UI_C_BORDER), LV_PART_MAIN);
        lv_obj_set_style_border_width(b, 1, LV_PART_MAIN);
        lv_obj_add_event_cb(b, cal_open_cb, LV_EVENT_LONG_PRESSED, NULL);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, "CAL (hold)");
        lv_obj_center(l);
    }
}

/* 谱线页：基线 + 对数刻度 + 三根渐变柱 */
static void bar_anim_cb(void *var, int32_t v) {
    /* 柱必须同时改高度和 y（顶对齐布局下改高度会向下长）——
     * 一个回调一起设，锚点永远是基线。 */
    lv_obj_t *b = (lv_obj_t *)var;
    lv_obj_set_height(b, (lv_coord_t)v);
    lv_obj_set_y(b, (lv_coord_t)(UI_SPEC_BASE_Y - v));
}

static void page_spec_build(lv_obj_t *pg) {
    static const struct {
        float       f;
        const char *txt;
    } k_ticks[5] = {
        {10000.0f, "10k"}, {50000.0f, "50k"}, {100000.0f, "100k"},
        {250000.0f, "250k"}, {500000.0f, "500k"},
    };

    s_spec_base_pts[0] = (lv_point_precise_t){0, UI_SPEC_BASE_Y};
    s_spec_base_pts[1] = (lv_point_precise_t){UI_SCR_W, UI_SPEC_BASE_Y};
    lv_obj_t *b = lv_line_create(pg);
    lv_line_set_points(b, s_spec_base_pts, 2U);
    lv_obj_set_style_line_color(b, lv_color_hex(UI_C_BORDER), LV_PART_MAIN);
    lv_obj_set_style_line_width(b, 2, LV_PART_MAIN);

    {
        const float lx_lo = 4.0f;                            /* log10(10k) */
        const float span = 2.6989700f - lx_lo;               /* 到 log10(500k) */
        for (int i = 0; i < 5; i++) {
            const float x01 = (log10f(k_ticks[i].f) - lx_lo) / span;
            (void)label_create(pg, k_ticks[i].txt, UI_C_DIM,
                               &lv_font_montserrat_12,
                               (lv_coord_t)(4 + x01 * (float)(UI_SCR_W - 24)),
                               (lv_coord_t)(UI_SPEC_BASE_Y + 6));
        }
    }

    for (int i = 0; i < MEAS_MAX_PEAKS; i++) {
        s_bars[i] = lv_obj_create(pg);
        lv_obj_set_size(s_bars[i], 28, 0);
        lv_obj_set_pos(s_bars[i], 60 + i * 70, UI_SPEC_BASE_Y);
        lv_obj_set_style_bg_grad_dir(s_bars[i], LV_GRAD_DIR_VER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_bars[i], lv_color_hex(UI_C_PRIMARY),
                                  LV_PART_MAIN);
        lv_obj_set_style_bg_grad_color(s_bars[i], lv_color_hex(UI_C_ACCENT),
                                       LV_PART_MAIN);
        lv_obj_set_style_radius(s_bars[i], 4, LV_PART_MAIN);
        lv_obj_set_style_border_width(s_bars[i], 0, LV_PART_MAIN);
    }
}

/* 谱线表页：三行卡片（序号 + 频率 + 幅值） */
static void page_list_build(lv_obj_t *pg) {
    static const lv_coord_t ys[MEAS_MAX_PEAKS] = {20, 100, 180};
    for (int i = 0; i < MEAS_MAX_PEAKS; i++) {
        lv_obj_t *c = card_create(pg, 16, ys[i], UI_SCR_W - 32, 64);
        s_list_rows[i] = label_create(c, "-", UI_C_TEXT, &lv_font_montserrat_16,
                                      12, 22);
    }
}

/* ================================================== 标定覆盖层（CAL 长按进） */
static void cal_refresh_prompt(void) {
    char buf[32];
    fmt_khz(buf, sizeof(buf), app_engine_cal_freq(app_engine_cal_index()));
    lv_label_set_text_fmt(s_cal_prompt,
                          "set source:\n100mV sine @ %s\nthen press MEAS", buf);
}

static void cal_open_cb(lv_event_t *e) {
    LV_UNUSED(e);
    app_engine_cal_begin();
    cal_refresh_prompt();
    lv_label_set_text_fmt(s_cal_progress, "step 0 / %d", app_engine_cal_points());
    lv_obj_remove_flag(s_cal_panel, LV_OBJ_FLAG_HIDDEN);
}

static void cal_close_cb(lv_event_t *e) {
    LV_UNUSED(e);
    lv_obj_add_flag(s_cal_panel, LV_OBJ_FLAG_HIDDEN);
}

static void cal_meas_cb(lv_event_t *e) {
    LV_UNUSED(e);
    const int left = app_engine_cal_step(); /* 同步 ~40ms（标定仪式流程可接受） */
    if (left < 0) {
        lv_label_set_text(s_cal_progress, "no peak! retry");
        return; /* 不前进：人检查信号源后重按 */
    }
    if (left == 0) {
        lv_label_set_text_fmt(s_cal_progress, "step %d / %d",
                              app_engine_cal_points(), app_engine_cal_points());
        lv_label_set_text(s_cal_prompt, app_engine_cal_save() == 0
                                            ? "done & saved"
                                            : "done (save FAILED)");
        return;
    }
    lv_label_set_text_fmt(s_cal_progress, "step %d / %d",
                          app_engine_cal_points() - left, app_engine_cal_points());
    cal_refresh_prompt();
}

static void cal_panel_create(lv_obj_t *scr) {
    s_cal_panel = lv_obj_create(scr);
    lv_obj_set_size(s_cal_panel, UI_SCR_W, UI_PAGE_H);
    lv_obj_set_pos(s_cal_panel, 0, UI_TOP_H);
    lv_obj_set_style_bg_color(s_cal_panel, lv_color_hex(UI_C_BG), LV_PART_MAIN);
    lv_obj_set_style_border_color(s_cal_panel, lv_color_hex(UI_C_BORDER),
                                  LV_PART_MAIN);
    lv_obj_set_style_border_width(s_cal_panel, 1, LV_PART_MAIN);
    lv_obj_clear_flag(s_cal_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_cal_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_cal_panel);

    s_cal_progress = label_create(s_cal_panel, "step 0 / 17", UI_C_DIM,
                                  &lv_font_montserrat_14, 12, 10);
    s_cal_prompt = label_create(s_cal_panel, "", UI_C_TEXT,
                                &lv_font_montserrat_14, 12, 60);
    lv_label_set_long_mode(s_cal_prompt, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_cal_prompt, UI_SCR_W - 24);

    static const struct {
        const char *txt;
        lv_coord_t  x;
        lv_event_cb_t cb;
    } k_btns[2] = {{"MEAS", 40, cal_meas_cb}, {"CLOSE", 180, cal_close_cb}};
    for (int i = 0; i < 2; i++) {
        lv_obj_t *b = lv_button_create(s_cal_panel);
        lv_obj_set_size(b, 100, 44);
        lv_obj_set_pos(b, k_btns[i].x, 340);
        lv_obj_set_style_bg_color(b, lv_color_hex(UI_C_CARD), LV_PART_MAIN);
        lv_obj_set_style_border_color(b, lv_color_hex(UI_C_BORDER), LV_PART_MAIN);
        lv_obj_set_style_border_width(b, 1, LV_PART_MAIN);
        lv_obj_add_event_cb(b, k_btns[i].cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, k_btns[i].txt);
        lv_obj_center(l);
    }
}

/* ================================================================== splash */
static void splash_done_cb(lv_timer_t *t) {
    LV_UNUSED(t);
    lv_obj_del(s_splash); /* 直接删（全屏 alpha 过渡在直写带宽下必卡，方案定稿） */
    s_splash = NULL;
}

static void splash_bar_cb(void *var, int32_t v) {
    lv_bar_set_value((lv_obj_t *)var, v, LV_ANIM_OFF);
}

static void splash_create(lv_obj_t *scr) {
    s_splash = lv_obj_create(scr);
    lv_obj_set_size(s_splash, UI_SCR_W, UI_SCR_H);
    lv_obj_set_pos(s_splash, 0, 0);
    lv_obj_set_style_bg_color(s_splash, lv_color_hex(UI_C_BG), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_splash, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_splash, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(s_splash);

    (void)label_create(s_splash, "G2026", UI_C_PRIMARY, &lv_font_montserrat_28,
                       (UI_SCR_W - 116) / 2, 170);
    (void)label_create(s_splash, "SIGNAL ANALYZER", UI_C_DIM,
                       &lv_font_montserrat_12, (UI_SCR_W - 130) / 2, 212);

    s_splash_bar = lv_bar_create(s_splash);
    lv_obj_set_size(s_splash_bar, 200, 6);
    lv_obj_set_pos(s_splash_bar, (UI_SCR_W - 200) / 2, 252);
    lv_obj_set_style_bg_color(s_splash_bar, lv_color_hex(UI_C_BORDER),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_splash_bar, lv_color_hex(UI_C_PRIMARY),
                              LV_PART_INDICATOR);
    lv_bar_set_range(s_splash_bar, 0, 100);
    lv_bar_set_value(s_splash_bar, 0, LV_ANIM_OFF);
    {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_splash_bar);
        lv_anim_set_exec_cb(&a, splash_bar_cb);
        lv_anim_set_values(&a, 0, 100);
        lv_anim_set_duration(&a, 800);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_start(&a);
    }
    (void)lv_timer_create(splash_done_cb, 950, NULL);
}

/* ================================================================== 刷新 */
static void refresh_wave(const meas_result_t *r) {
    if (r->n_peaks == 0U) {
        lv_label_set_text(s_wave_title, "no signal");
        return;
    }
    char buf[32];
    fmt_khz(buf, sizeof(buf), r->f1_hz);
    lv_label_set_text_fmt(s_wave_title, "f1 %s   %dT", buf, s_cycles);

    /* 纵向量程 = 分量幅值和 ×1.2（自动缩放；上限 1mV 防除零） */
    float amp_sum = 0.0f;
    for (int i = 0; i < r->n_peaks; i++) {
        amp_sum += r->peak[i].amp_mv;
    }
    const float range = (amp_sum < 1.0f) ? 1.0f : amp_sum * 1.2f;
    const lv_coord_t mid = UI_PAGE_H / 2 + 10;
    const float scale = ((float)UI_PAGE_H * 0.40f) / range;

    float y[APP_UI_WAVE_POINTS];
    if (op_wave_build(r, s_cycles, y, APP_UI_WAVE_POINTS) != APP_UI_WAVE_POINTS) {
        return;
    }
    for (int j = 0; j < APP_UI_WAVE_POINTS; j++) {
        s_wave_pts[j].x = (float)j * (float)UI_SCR_W / (float)(APP_UI_WAVE_POINTS - 1U);
        s_wave_pts[j].y = (float)mid - y[j] * scale; /* 屏 y 向下 */
        s_glow_pts[j] = s_wave_pts[j]; /* 发光层共享形状 */
    }
    lv_line_set_points(s_glow_line, s_glow_pts, APP_UI_WAVE_POINTS);
    lv_line_set_points(s_wave_line, s_wave_pts, APP_UI_WAVE_POINTS);
    lv_obj_remove_flag(s_glow_line, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_wave_line, LV_OBJ_FLAG_HIDDEN);
}

static void refresh_params(const meas_result_t *r) {
    char buf[32];
    fmt_mv(buf, sizeof(buf), r->upp_mv);
    flash_value(s_val_lbls[0], buf);
    fmt_mv(buf, sizeof(buf), r->urms_mv);
    flash_value(s_val_lbls[1], buf);
    fmt_khz(buf, sizeof(buf), r->f1_hz);
    flash_value(s_val_lbls[2], buf);
}

static void refresh_spec(const meas_result_t *r) {
    op_spec_line_t s[MEAS_MAX_PEAKS];
    const int n = op_spec_build(r, s, MEAS_MAX_PEAKS);
    const lv_coord_t max_h = UI_SPEC_BASE_Y - 60;

    for (int i = 0; i < MEAS_MAX_PEAKS; i++) {
        lv_coord_t h = 0;
        lv_coord_t x = 60 + (lv_coord_t)(i * 70);
        if (i < n) {
            x = (lv_coord_t)(20 + s[i].x01 * (float)(UI_SCR_W - 60));
            h = (lv_coord_t)(s[i].len01 * (float)max_h);
        }
        lv_obj_set_x(s_bars[i], x); /* 位置即时跟（对数轴相对位置），高度走动画 */
        if (lv_obj_get_height(s_bars[i]) == h) {
            continue;
        }
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_bars[i]);
        lv_anim_set_exec_cb(&a, bar_anim_cb);
        lv_anim_set_values(&a, lv_obj_get_height(s_bars[i]), h);
        lv_anim_set_duration(&a, 300);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
    }
}

static void refresh_list(const meas_result_t *r) {
    for (int i = 0; i < MEAS_MAX_PEAKS; i++) {
        if (i < (int)r->n_peaks) {
            char fb[32], ab[32];
            fmt_khz(fb, sizeof(fb), r->peak[i].freq_hz);
            fmt_mv(ab, sizeof(ab), r->peak[i].amp_mv);
            lv_label_set_text_fmt(s_list_rows[i], "#%d  %s   %s", i + 1, fb, ab);
        } else {
            lv_label_set_text(s_list_rows[i], "-");
        }
    }
}

/* ================================================================== 对外 */
int app_ui_init(void) {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(UI_C_BG), LV_PART_MAIN);

    (void)topbar_create(scr);

    /* 横向 scroll 容器：四页并排 + snap 居中（手指滑页，见文件头） */
    s_scroller = lv_obj_create(scr);
    lv_obj_set_pos(s_scroller, 0, UI_TOP_H);
    lv_obj_set_size(s_scroller, UI_SCR_W, UI_PAGE_H);
    lv_obj_set_style_bg_color(s_scroller, lv_color_hex(UI_C_BG), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_scroller, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_scroller, 0, LV_PART_MAIN);
    lv_obj_set_scroll_dir(s_scroller, LV_DIR_HOR);
    lv_obj_set_scroll_snap_x(s_scroller, LV_SCROLL_SNAP_CENTER);
    lv_obj_set_scrollbar_mode(s_scroller, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(s_scroller, scroll_cb, LV_EVENT_SCROLL, NULL);

    for (uint8_t i = 0; i < UI_PAGE_N; i++) {
        lv_obj_t *pg = lv_obj_create(s_scroller);
        s_pages[i] = pg;
        lv_obj_set_pos(pg, (lv_coord_t)(i * UI_SCR_W), 0);
        lv_obj_set_size(pg, UI_SCR_W, UI_PAGE_H);
        lv_obj_set_style_bg_color(pg, lv_color_hex(UI_C_BG), LV_PART_MAIN);
        lv_obj_set_style_border_width(pg, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(pg, 0, LV_PART_MAIN);
        lv_obj_clear_flag(pg, LV_OBJ_FLAG_SCROLLABLE);
    }
    page_wave_build(s_pages[0]);
    page_param_build(s_pages[1]);
    page_spec_build(s_pages[2]);
    page_list_build(s_pages[3]);
    (void)tabbar_create(scr);
    lv_obj_set_style_text_color(s_tab_btns[0], lv_color_hex(UI_C_PRIMARY),
                                LV_PART_MAIN);

    cal_panel_create(scr);
    splash_create(scr);
    return 0;
}

void app_ui_poll(uint32_t now_ms) {
    /* splash 期间不刷业务（它有自己的进度动画，别抢带宽） */
    if (s_splash != NULL) {
        return;
    }

    /* 顶栏：标定状态点变色 + 等效 fps（每秒结算一次） */
    static uint8_t  s_cal_shown = 0xFF;
    static uint32_t s_fps_last = 0xFFFFFFFFU;
    static uint32_t s_fps_ms = 0;
    const uint8_t cal = app_engine_cal_loaded() ? 1U : 0U;
    if (cal != s_cal_shown) {
        s_cal_shown = cal;
        lv_obj_set_style_bg_color(s_cal_dot,
                                  lv_color_hex(cal ? UI_C_ACCENT : UI_C_AMBER),
                                  LV_PART_MAIN);
    }
    if ((uint32_t)(now_ms - s_fps_ms) >= 1000U) {
        s_fps_ms = now_ms;
        const uint32_t fx = app_lvgl_fps_x10(now_ms);
        if (fx != s_fps_last) {
            s_fps_last = fx;
            lv_label_set_text_fmt(s_fps_lbl, "%lu.%lu fps",
                                  (unsigned long)(fx / 10U),
                                  (unsigned long)(fx % 10U));
        }
    }

    /* 业务页：seq 变了或翻了页才刷当前页（其余页惰性 —— 测量周期 500ms） */
    static uint32_t s_seen_seq = 0;
    static uint8_t  s_seen_page = 0xFF;
    const meas_result_t *r = app_engine_result();
    if (r->seq == s_seen_seq && s_page == s_seen_page) {
        return;
    }
    s_seen_seq = r->seq;
    s_seen_page = s_page;
    switch (s_page) {
    case 0:
        refresh_wave(r);
        break;
    case 1:
        refresh_params(r);
        break;
    case 2:
        refresh_spec(r);
        break;
    default:
        refresh_list(r);
        break;
    }
}

/* ============================================================ UI 任务体 ==== */
void app_ui_task(void *arg) {
    (void)arg;
    LOG_I("[UI] 界面任务启动（prio=%u, stack=%u words）", (unsigned)APP_TASK_UI_PRIORITY,
          (unsigned)APP_TASK_UI_STACK_WORDS);

    for (;;) {
        /* ---- 采集窗静默（硬件方案 02 §6 O1）----
         * engine 正在等 DMA 搬满时**不碰 FSMC**：LCD 总线的开关噪声是板内最大
         * 干扰源，而它正好会灌进同一时刻在采样的模拟前端。engine 优先级更高，
         * 它采集中时本任务本来也抢不到 CPU；但 engine 阻塞在信号量上时 CPU 会
         * 回到这里 —— 那时必须主动避让。采集窗只有 2.048ms，避让代价可忽略。 */
        while (app_engine_acquiring()) {
            vTaskDelay(1U);
        }

        const uint32_t now_ms = bsp_tick_ms();
        app_ui_poll(now_ms);  /* 业务页刷新（读结果 mailbox） */
        app_lvgl_poll(now_ms); /* LVGL 时基 + 重绘 + 读触摸 */

        /* ⚠️ 本延时**就是** LVGL 的时间分辨率（app_lvgl_poll 按两次调用差值
         *    推进时基）。改成大于 16ms 会掉帧 —— 见 project_config.h。 */
        vTaskDelay(pdMS_TO_TICKS(APP_TICK_PERIOD_MS));
    }
}
