/* ============================================================================
 *  app_ui.c —— 测量 UI · 见 app_ui.h
 * ==========================================================================*/
#include "app_ui.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>

#include "app_engine.h"
#include "log.h"
#include "meas_result.h"
#include "op_spec.h"
#include "op_wave.h"
#include "project_config.h"

#include "lvgl.h"

/* ---- 布局常量（屏 320×480 竖屏，见 board_config.h）----
 * 放本文件而不是 project_config：它们是"这个页面的画法"，不是产品参数 ——
 * 改了只影响长相，不影响行为。（工程禁魔数：给它们名字就是这里。） */
#define UI_SCR_W 320U
#define UI_SCR_H 480U
#define UI_TAB_H 36U
#define UI_TAB_N 4U
#define UI_MODE_W 64U
#define UI_TAB_W ((UI_SCR_W - UI_MODE_W) / UI_TAB_N)

/* ---- 页签与状态 ---- */
static lv_obj_t *s_pages[UI_TAB_N];
static lv_obj_t *s_tab_btns[UI_TAB_N];
static lv_obj_t *s_mode_btn;
static uint8_t   s_page = 0; /* 当前页 0..3 */

/* 波形页 */
static lv_obj_t *s_wave_line;
static lv_obj_t *s_wave_title;
static int       s_cycles = 1; /* 1T / 3T */
/* LVGL 的 line 持有指针 ⇒ 点数组必须常驻（static）。precise 版用 float。 */
static lv_point_precise_t s_wave_pts[APP_UI_WAVE_POINTS];

/* 参数页 */
static lv_obj_t *s_lbl_upp;
static lv_obj_t *s_lbl_urms;
static lv_obj_t *s_lbl_f1;
static lv_obj_t *s_lbl_cal_state;

/* 谱线页：基线 + 每峰一根竖线（≤3 根，静态点对常驻） */
static lv_obj_t           *s_spec_base;
static lv_obj_t           *s_spec_lines[MEAS_MAX_PEAKS];
static lv_point_precise_t  s_spec_base_pts[2];
static lv_point_precise_t  s_spec_pts[MEAS_MAX_PEAKS][2];

/* 谱线表页：3 行数值 */
static lv_obj_t *s_list_lbls[MEAS_MAX_PEAKS];

/* 标定覆盖层 */
static lv_obj_t *s_cal_panel;
static lv_obj_t *s_cal_prompt;
static lv_obj_t *s_cal_progress;

/* ---- 定点格式化（printf 浮点不可靠，见 app_ui.h）----
 * mv：三位整数 + 一位小数（"123.4 mV"）；khz：两位小数（"42.00 kHz"）。 */
static void fmt_mv(char *buf, size_t cap, float mv) {
    int v = (int)(mv * 10.0f + (mv >= 0.0f ? 0.5f : -0.5f)); /* 0.1mV 定点 */
    int frac = v % 10;
    if (frac < 0) {
        frac = -frac;
    }
    (void)snprintf(buf, cap, "%d.%d mV", v / 10, frac);
}

static void fmt_khz(char *buf, size_t cap, float hz) {
    (void)snprintf(buf, cap, "%lu.%02lu kHz",
                   (unsigned long)(hz / 1000.0f),
                   (unsigned long)(((long)(hz / 10.0f)) % 100L));
}

/* ---- 数据 → 当前页刷新 ---- */
static void refresh_wave(const meas_result_t *r) {
    if (r->n_peaks == 0U) {
        lv_label_set_text(s_wave_title, "no signal");
        return;
    }
    char buf[32];
    fmt_khz(buf, sizeof(buf), r->f1_hz);
    lv_label_set_text_fmt(s_wave_title, "%s  %dT", buf, s_cycles);

    /* 峰表 → 波形列（mV）→ 屏幕纵坐标（中点 0mV 对齐画面中线）。
     * 纵向满幅 ±100mV 封顶（题面最大 250mVpp 的重建波多在 ±125mV 内 ——
     * 按 ΣA 自动缩放更准：用 peak 幅值和的 1.2 倍做量程。 */
    float amp_sum = 0.0f;
    for (int i = 0; i < r->n_peaks; i++) {
        amp_sum += r->peak[i].amp_mv;
    }
    const float range = (amp_sum < 1.0f) ? 1.0f : amp_sum * 1.2f;
    const float content_h = (float)(UI_SCR_H - UI_TAB_H);
    const float mid_y = content_h * 0.5f;
    const float scale = (content_h * 0.45f) / range;

    float y[APP_UI_WAVE_POINTS];
    if (op_wave_build(r, s_cycles, y, APP_UI_WAVE_POINTS) != APP_UI_WAVE_POINTS) {
        return; /* 没峰等 —— 上面已挡，防御 */
    }
    for (int j = 0; j < APP_UI_WAVE_POINTS; j++) {
        s_wave_pts[j].x = (float)j * (float)UI_SCR_W / (float)(APP_UI_WAVE_POINTS - 1U);
        s_wave_pts[j].y = mid_y - y[j] * scale; /* 屏 y 向下 */
    }
    lv_line_set_points(s_wave_line, s_wave_pts, APP_UI_WAVE_POINTS);
}

static void refresh_params(const meas_result_t *r) {
    char buf[32];
    fmt_mv(buf, sizeof(buf), r->upp_mv);
    lv_label_set_text_fmt(s_lbl_upp, "Upp  %s", buf);
    fmt_mv(buf, sizeof(buf), r->urms_mv);
    lv_label_set_text_fmt(s_lbl_urms, "Urms %s", buf);
    fmt_khz(buf, sizeof(buf), r->f1_hz);
    lv_label_set_text_fmt(s_lbl_f1, "f1   %s", buf);
}

static void refresh_spec(const meas_result_t *r) {
    op_spec_line_t s[MEAS_MAX_PEAKS];
    const int n = op_spec_build(r, s, MEAS_MAX_PEAKS);
    const float content_h = (float)(UI_SCR_H - UI_TAB_H);

    s_spec_base_pts[0].x = 0.0f;
    s_spec_base_pts[0].y = content_h - 20.0f;
    s_spec_base_pts[1].x = (float)UI_SCR_W;
    s_spec_base_pts[1].y = content_h - 20.0f;
    lv_line_set_points(s_spec_base, s_spec_base_pts, 2U);

    for (int i = 0; i < MEAS_MAX_PEAKS; i++) {
        if (i < n) {
            const float x = s[i].x01 * (float)UI_SCR_W;
            s_spec_pts[i][0].x = x;
            s_spec_pts[i][0].y = content_h - 20.0f;
            s_spec_pts[i][1].x = x;
            s_spec_pts[i][1].y = (content_h - 20.0f) - s[i].len01 * (content_h - 60.0f);
            lv_line_set_points(s_spec_lines[i], s_spec_pts[i], 2U);
            lv_obj_clear_flag(s_spec_lines[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_spec_lines[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void refresh_list(const meas_result_t *r) {
    for (int i = 0; i < MEAS_MAX_PEAKS; i++) {
        if (i < (int)r->n_peaks) {
            char fbuf[32], abuf[32];
            fmt_khz(fbuf, sizeof(fbuf), r->peak[i].freq_hz);
            fmt_mv(abuf, sizeof(abuf), r->peak[i].amp_mv);
            lv_label_set_text_fmt(s_list_lbls[i], "%d: %s  %s", i + 1, fbuf, abuf);
        } else {
            lv_label_set_text(s_list_lbls[i], "-");
        }
    }
}

/* ---- 页切换（切页即重测，docs/01 §7）---- */
static const char *const k_tab_names[UI_TAB_N] = {"WAVE", "PARAM", "SPEC", "LIST"};

static void show_page(uint8_t page) {
    if (page >= UI_TAB_N) {
        return;
    }
    s_page = page;
    for (uint8_t i = 0; i < UI_TAB_N; i++) {
        if (i == page) {
            lv_obj_remove_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    app_engine_remeasure(); /* 切页即刻重测：页面一出来就是新数（§7） */
}

static void tab_cb(lv_event_t *e) {
    show_page((uint8_t)(uintptr_t)lv_event_get_user_data(e));
}

static void wave_click_cb(lv_event_t *e) {
    LV_UNUSED(e);
    s_cycles = (s_cycles == 1) ? 3 : 1;
    refresh_wave(app_engine_result()); /* 立即换轴 —— 不等下一帧测量 */
}

static void mode_cb(lv_event_t *e) {
    LV_UNUSED(e);
    const uint8_t next = (app_engine_scene() + 1U) % (MEAS_SCENE_UBJ + 1U);
    app_engine_set_scene(next);
    lv_label_set_text(s_mode_btn, (next == MEAS_SCENE_UA) ? "u_a"
                                   : (next == MEAS_SCENE_UB) ? "u_b"
                                                             : "u_b+J");
}

/* ---- 标定覆盖层 ---- */
static void cal_refresh_progress(void) {
    const int idx = app_engine_cal_index();
    const int total = app_engine_cal_points();
    lv_label_set_text_fmt(s_cal_progress, "step %d / %d", idx, total);
}

static void cal_refresh_prompt(void) {
    /* 提示当前频点：信号源设 100mV 单音 @f（100mV 标称，docs/01 §11） */
    char buf[32];
    fmt_khz(buf, sizeof(buf), app_engine_cal_freq(app_engine_cal_index()));
    lv_label_set_text_fmt(s_cal_prompt, "set source:\n100mV sine @ %s\nthen press MEAS", buf);
}

static void cal_open_cb(lv_event_t *e) {
    LV_UNUSED(e);
    app_engine_cal_begin();
    cal_refresh_prompt();
    cal_refresh_progress();
    lv_obj_remove_flag(s_cal_panel, LV_OBJ_FLAG_HIDDEN);
}

static void cal_close_cb(lv_event_t *e) {
    LV_UNUSED(e);
    lv_obj_add_flag(s_cal_panel, LV_OBJ_FLAG_HIDDEN);
}

static void cal_meas_cb(lv_event_t *e) {
    LV_UNUSED(e);
    const int left = app_engine_cal_step();
    if (left < 0) {
        lv_label_set_text(s_cal_progress, "no peak! retry");
        return; /* 未检出：留在当前点让人检查 */
    }
    if (left == 0) {
        cal_refresh_progress();
        if (app_engine_cal_save() == 0) {
            lv_label_set_text(s_cal_prompt, "done & saved");
        } else {
            lv_label_set_text(s_cal_prompt, "done (save FAILED)\nunplug-safe until reboot");
        }
        lv_label_set_text(s_cal_progress, "step 17 / 17");
        return;
    }
    cal_refresh_progress();
    cal_refresh_prompt();
}

/* ---- 页面构建 ---- */
static lv_obj_t *page_create(uint8_t idx) {
    lv_obj_t *pg = lv_obj_create(lv_screen_active());
    lv_obj_set_size(pg, UI_SCR_W, UI_SCR_H - UI_TAB_H);
    lv_obj_set_pos(pg, 0, 0);
    lv_obj_set_style_bg_opa(pg, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(pg, 0, LV_PART_MAIN);
    lv_obj_clear_flag(pg, LV_OBJ_FLAG_SCROLLABLE);
    return s_pages[idx] = pg;
}

static lv_obj_t *btn_create(lv_obj_t *parent, const char *txt, lv_coord_t x,
                            lv_coord_t y, lv_coord_t w, lv_coord_t h,
                            lv_event_cb_t cb, void *user_data, bool long_press) {
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_obj_add_event_cb(b, cb, long_press ? LV_EVENT_LONG_PRESSED : LV_EVENT_CLICKED,
                        user_data);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_center(l);
    return b;
}

int app_ui_init(void) {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), LV_PART_MAIN);

    /* ---- 波形页 ---- */
    {
        lv_obj_t *pg = page_create(0);
        s_wave_title = lv_label_create(pg);
        lv_obj_set_style_text_color(s_wave_title, lv_color_hex(0x4CC9F0), LV_PART_MAIN);
        lv_obj_align(s_wave_title, LV_ALIGN_TOP_MID, 0, 6);

        s_wave_line = lv_line_create(pg);
        lv_obj_set_style_line_width(s_wave_line, 2, LV_PART_MAIN);
        lv_obj_set_style_line_color(s_wave_line, lv_color_hex(0xFFD166), LV_PART_MAIN);

        /* 点击画面切 1T/3T：透明热区盖满内容区 */
        lv_obj_t *hit = lv_obj_create(pg);
        lv_obj_set_size(hit, UI_SCR_W, UI_SCR_H - UI_TAB_H - 30U);
        lv_obj_set_pos(hit, 0, 30U);
        lv_obj_set_style_bg_opa(hit, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(hit, 0, LV_PART_MAIN);
        lv_obj_add_event_cb(hit, wave_click_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_move_foreground(s_wave_line); /* 线在热区上面只影响视觉，事件在热区 */
    }

    /* ---- 参数页 ---- */
    {
        lv_obj_t *pg = page_create(1);
        static const lv_coord_t ys[3] = {60, 150, 240};
        lv_obj_t **lbls[3] = {&s_lbl_upp, &s_lbl_urms, &s_lbl_f1};
        for (int i = 0; i < 3; i++) {
            *lbls[i] = lv_label_create(pg);
            lv_obj_set_style_text_font(*lbls[i], &lv_font_montserrat_28, LV_PART_MAIN);
            lv_obj_set_style_text_color(*lbls[i], lv_color_hex(0xE0E0E0), LV_PART_MAIN);
            lv_obj_align(*lbls[i], LV_ALIGN_TOP_MID, 0, ys[i]);
        }
        s_lbl_cal_state = lv_label_create(pg);
        lv_obj_set_style_text_color(s_lbl_cal_state, lv_color_hex(0x8D99AE), LV_PART_MAIN);
        lv_obj_align(s_lbl_cal_state, LV_ALIGN_BOTTOM_MID, 0, -8);
        lv_label_set_text(s_lbl_cal_state, app_engine_cal_loaded() ? "cal loaded"
                                                                   : "not calibrated");
        btn_create(pg, "CAL (hold)", 90, 330, 140, 48, cal_open_cb, NULL, true);
    }

    /* ---- 定性谱页 ---- */
    {
        lv_obj_t *pg = page_create(2);
        s_spec_base = lv_line_create(pg);
        lv_obj_set_style_line_width(s_spec_base, 2, LV_PART_MAIN);
        lv_obj_set_style_line_color(s_spec_base, lv_color_hex(0x8D99AE), LV_PART_MAIN);
        for (int i = 0; i < MEAS_MAX_PEAKS; i++) {
            s_spec_lines[i] = lv_line_create(pg);
            lv_obj_set_style_line_width(s_spec_lines[i], 6, LV_PART_MAIN);
            lv_obj_set_style_line_color(s_spec_lines[i], lv_color_hex(0x06D6A0),
                                        LV_PART_MAIN);
        }
    }

    /* ---- 谱线表页 ---- */
    {
        lv_obj_t *pg = page_create(3);
        static const lv_coord_t ys[MEAS_MAX_PEAKS] = {60, 120, 180};
        for (int i = 0; i < MEAS_MAX_PEAKS; i++) {
            s_list_lbls[i] = lv_label_create(pg);
            lv_obj_set_style_text_font(s_list_lbls[i], &lv_font_montserrat_16,
                                       LV_PART_MAIN);
            lv_obj_set_style_text_color(s_list_lbls[i], lv_color_hex(0xE0E0E0),
                                        LV_PART_MAIN);
            lv_obj_align(s_list_lbls[i], LV_ALIGN_TOP_MID, 0, ys[i]);
            lv_label_set_text(s_list_lbls[i], "-");
        }
    }

    /* ---- 底部页签条 ---- */
    for (uint8_t i = 0; i < UI_TAB_N; i++) {
        s_tab_btns[i] = btn_create(scr, k_tab_names[i], (lv_coord_t)(i * UI_TAB_W),
                                   (lv_coord_t)(UI_SCR_H - UI_TAB_H), UI_TAB_W,
                                   UI_TAB_H, tab_cb, (void *)(uintptr_t)i, false);
    }
    s_mode_btn = btn_create(scr, "u_a", (lv_coord_t)(UI_SCR_W - UI_MODE_W),
                            (lv_coord_t)(UI_SCR_H - UI_TAB_H), UI_MODE_W, UI_TAB_H,
                            mode_cb, NULL, false);

    /* ---- 标定覆盖层（默认隐藏）---- */
    {
        s_cal_panel = lv_obj_create(scr);
        lv_obj_set_size(s_cal_panel, UI_SCR_W, UI_SCR_H - UI_TAB_H);
        lv_obj_set_pos(s_cal_panel, 0, 0);
        lv_obj_set_style_bg_color(s_cal_panel, lv_color_hex(0x101820), LV_PART_MAIN);
        lv_obj_add_flag(s_cal_panel, LV_OBJ_FLAG_HIDDEN);

        s_cal_prompt = lv_label_create(s_cal_panel);
        lv_obj_set_style_text_align(s_cal_prompt, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_center(s_cal_prompt);

        s_cal_progress = lv_label_create(s_cal_panel);
        lv_obj_align(s_cal_progress, LV_ALIGN_TOP_MID, 0, 40);

        btn_create(s_cal_panel, "MEAS", 40, 380, 100, 50, cal_meas_cb, NULL, false);
        btn_create(s_cal_panel, "CLOSE", 180, 380, 100, 50, cal_close_cb, NULL, false);
    }

    show_page(0);
    return 0;
}

void app_ui_poll(uint32_t now_ms) {
    LV_UNUSED(now_ms);
    /* seq 变了才刷当前页（测量周期 500ms，页签切换另有即时重测） */
    static uint32_t s_seen_seq = 0;
    const meas_result_t *r = app_engine_result();
    if (r->seq == s_seen_seq) {
        return;
    }
    s_seen_seq = r->seq;

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
