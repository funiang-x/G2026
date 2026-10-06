/*
 * 官方 LVGL widgets demo 的本工程轻量适配版。
 *
 * 目的不是做产品 UI，而是一次性验证显示、触控和常用控件：
 *   tabview / button / checkbox / switch / slider / dropdown / roller /
 *   textarea / keyboard / bar。
 *
 * 上游完整 widgets demo 需要至少 38KB LVGL heap，并依赖比本工程 vendor
 * 更前沿的 API；本工程 LVGL heap 固定为 CCM 高半区 32KB。因此保留官方
 * demo 入口与控件组合，删掉重型 Profile/Analytics/Shop 页面，确保能在
 * 实物上稳定跑起来。验证结束将 APP_LVGL_USE_DEMO 改回 0 即恢复业务 UI。
 */
#include "lv_demo_widgets.h"

#if LV_USE_DEMO_WIDGETS

static lv_obj_t *s_status;
static uint32_t s_clicks;

static void status_set(const char *text)
{
    if(s_status != NULL) lv_label_set_text(s_status, text);
}

static void button_event_cb(lv_event_t *e)
{
    if(lv_event_get_code(e) == LV_EVENT_CLICKED) {
        s_clicks++;
        lv_label_set_text_fmt(s_status, "Button clicked: %lu", (unsigned long)s_clicks);
    }
}

static void value_event_cb(lv_event_t *e)
{
    if(lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t *obj = lv_event_get_target(e);
        if(lv_obj_check_type(obj, &lv_slider_class)) {
            lv_label_set_text_fmt(s_status, "Slider value: %ld", (long)lv_slider_get_value(obj));
        } else if(lv_obj_check_type(obj, &lv_switch_class)) {
            status_set(lv_obj_has_state(obj, LV_STATE_CHECKED) ? "Switch: ON" : "Switch: OFF");
        } else if(lv_obj_check_type(obj, &lv_checkbox_class)) {
            status_set(lv_obj_has_state(obj, LV_STATE_CHECKED) ? "Checkbox: checked" : "Checkbox: unchecked");
        } else if(lv_obj_check_type(obj, &lv_dropdown_class)) {
            status_set("Dropdown changed");
        } else if(lv_obj_check_type(obj, &lv_roller_class)) {
            status_set("Roller changed");
        }
    }
}

static lv_obj_t *title(lv_obj_t *parent, const char *text)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(0x4CC9F0), LV_PART_MAIN);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 8);
    return label;
}

static void build_basic_tab(lv_obj_t *tab)
{
    title(tab, "Official widgets test");

    lv_obj_t *btn = lv_button_create(tab);
    lv_obj_set_size(btn, 150, 42);
    lv_obj_align(btn, LV_ALIGN_TOP_LEFT, 18, 42);
    lv_obj_add_event_cb(btn, button_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, "PRESS BUTTON");
    lv_obj_center(btn_label);

    s_status = lv_label_create(tab);
    lv_label_set_text(s_status, "Touch a control");
    lv_obj_align(s_status, LV_ALIGN_TOP_LEFT, 190, 55);

    lv_obj_t *check = lv_checkbox_create(tab);
    lv_checkbox_set_text(check, "Check me");
    lv_obj_align(check, LV_ALIGN_TOP_LEFT, 18, 96);
    lv_obj_add_event_cb(check, value_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *sw = lv_switch_create(tab);
    lv_obj_align(sw, LV_ALIGN_TOP_LEFT, 180, 96);
    lv_obj_add_event_cb(sw, value_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *slider = lv_slider_create(tab);
    lv_obj_set_width(slider, 270);
    lv_obj_align(slider, LV_ALIGN_TOP_LEFT, 18, 145);
    lv_slider_set_value(slider, 50, LV_ANIM_OFF);
    lv_obj_add_event_cb(slider, value_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *bar = lv_bar_create(tab);
    lv_obj_set_width(bar, 270);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 18, 188);
    lv_bar_set_value(bar, 72, LV_ANIM_OFF);

    lv_obj_t *hint = lv_label_create(tab);
    lv_label_set_text(hint, "Button / checkbox / switch / slider / bar");
    lv_obj_set_style_text_color(hint, lv_color_hex(0xB0B8C0), LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -8);
}

static void build_selector_tab(lv_obj_t *tab)
{
    title(tab, "Selectors");

    lv_obj_t *drop = lv_dropdown_create(tab);
    lv_dropdown_set_options(drop, "Red\nGreen\nBlue\nYellow");
    lv_obj_set_width(drop, 190);
    lv_obj_align(drop, LV_ALIGN_TOP_LEFT, 18, 46);
    lv_obj_add_event_cb(drop, value_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *roller = lv_roller_create(tab);
    lv_roller_set_options(roller, "320x480\n480x320\nRotation 90\nRotation 270", LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(roller, 3);
    lv_obj_align(roller, LV_ALIGN_TOP_LEFT, 245, 42);
    lv_obj_add_event_cb(roller, value_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *chart = lv_chart_create(tab);
    lv_obj_set_size(chart, 400, 125);
    lv_obj_align(chart, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_chart_series_t *series = lv_chart_add_series(chart, lv_color_hex(0x4CC9F0), LV_CHART_AXIS_PRIMARY_Y);
    const int values[] = {12, 38, 24, 72, 54, 81, 66, 91};
    for(uint32_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        lv_chart_set_next_value(chart, series, values[i]);
    }
    lv_chart_refresh(chart);
}

static void build_keyboard_tab(lv_obj_t *tab)
{
    title(tab, "Keyboard / textarea");

    lv_obj_t *ta = lv_textarea_create(tab);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, "Tap here and type");
    lv_obj_set_width(ta, 400);
    lv_obj_align(ta, LV_ALIGN_TOP_MID, 0, 48);

    lv_obj_t *kb = lv_keyboard_create(tab);
    lv_obj_set_size(kb, 440, 140);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_keyboard_set_textarea(kb, ta);
}

void lv_demo_widgets(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101820), LV_PART_MAIN);

    lv_obj_t *tabs = lv_tabview_create(screen);
    lv_obj_set_size(tabs, LV_PCT(100), LV_PCT(100));
    lv_tabview_set_tab_bar_size(tabs, 34);

    lv_obj_t *basic = lv_tabview_add_tab(tabs, "Basic");
    lv_obj_t *select = lv_tabview_add_tab(tabs, "Select");
    lv_obj_t *keyboard = lv_tabview_add_tab(tabs, "Keyboard");
    build_basic_tab(basic);
    build_selector_tab(select);
    build_keyboard_tab(keyboard);
}

#endif /* LV_USE_DEMO_WIDGETS */
