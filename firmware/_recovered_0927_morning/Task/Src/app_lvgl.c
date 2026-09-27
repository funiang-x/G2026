/* ============================================================================
 *  app_lvgl.c —— LVGL 9.5 适配层（显示 + 触摸）· 见 app_lvgl.h
 *
 *  ---- 一条数据通路（显示）----
 *      LVGL 渲染到 draw buffer（SRAM1）
 *        → disp_flush_cb
 *            → bsp_lcd_set_window()      开窗（0x2A/0x2B）
 *            → bsp_lcd_write_pixels()    0x2C + 16 位紧循环直写
 *        → lv_display_flush_ready()   ← 漏了这行界面会**永久卡死**（不再重绘）
 *
 *  ---- 内存落点（三条铁律）----
 *    · **LVGL 的堆**：CCM 高半边（32KB）—— 在 lv_conf.h 里由
 *      `BOARD_LVGL_MEM_ADDR` 指定，不占 .bss；
 *    · **draw buffer**：SRAM1 —— 本文件的 `s_draw_buf`。放 CCM 会怎样？
 *      本工程走 FSMC 直写（CPU 搬运，不用 DMA），所以 CCM 其实也能跑；
 *      但仍放 SRAM1，因为**将来若改用 DMA 搬 FSMC**，CCM 会静默失效（不报错）；
 *    · **采集缓冲**：绝不能在 CCM（见 bsp_adc.h）。
 * ==========================================================================*/
#include "app_lvgl.h"

#include "board_config.h"
#include "bsp.h"
#include "log.h"
#include "project_config.h"

#include "cmsis_os.h" /* osDelay —— UI 主循环的节拍 */
#include "lvgl.h"

/* draw buffer：WIDTH × N 行 × 2B。**放 SRAM1**（普通 .bss）。
 * ⚠️ 必须 4 字节对齐：LVGL 把它当 RGB565 数组用，我们把 uint8_t* 转成
 *    uint16_t* 直写 FSMC —— 没对齐在某些 -O 档下会取到错位的数据。
 * ⚠️ 尺寸不能太小：PARTIAL 模式下每次重绘至少要能装下"一行"。
 *    APP_LVGL_DRAW_BUF_LINES 越大越省 flush 次数，但也越吃 RAM。 */
static uint8_t s_draw_buf[BOARD_LCD_WIDTH * APP_LVGL_DRAW_BUF_LINES * 2U]
    __attribute__((aligned(4)));

static lv_display_t *s_disp = NULL;
static lv_indev_t   *s_indev = NULL;

/* ------------------------------------------------------------------ 日志 */
/* LVGL 自己的日志 → RTT。
 * ⚠️ 为什么不用 `LV_LOG_PRINTF = 1`：本工程的 printf **没有重定向**
 *    （syscalls.c 的 `__io_putchar` 是空弱实现），开了等于日志静默丢弃；
 *    而且 printf 无互斥，与 RTT 后端会抢输出。走回调最干净。
 * ⚠️ `buf` 自带结尾换行，所以这里用 %s 会多一个空行 —— 可接受。 */
static void lvgl_log_cb(lv_log_level_t level, const char *buf) {
    if (level <= LV_LOG_LEVEL_WARN) {
        LOG_W("[LVGL] %s", buf);
    } else {
        LOG_D("[LVGL] %s", buf);
    }
}

/* ------------------------------------------------------------------ 显示 */
/* flush：把 LVGL 渲染好的矩形搬进屏。
 * ⚠️ 坐标是**闭区间**且已在横屏坐标系里（MADCTL 负责方向，本层不做任何旋转）。 */
static void disp_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    uint32_t px_count;

    bsp_lcd_set_window((uint16_t)area->x1, (uint16_t)area->y1, (uint16_t)area->x2,
                       (uint16_t)area->y2);

    px_count = (uint32_t)lv_area_get_width(area) * (uint32_t)lv_area_get_height(area);
    /* px_map 就是 s_draw_buf（PARTIAL + 单缓冲），已 4 字节对齐 ⇒ 直接转 uint16_t*。
     * ⚠️ 颜色格式是 RGB565（非 SWAPPED），所以数组里就是本机字节序的 RGB565。 */
    bsp_lcd_write_pixels((const uint16_t *)(const void *)px_map, px_count);

    /* ⚠️ 必须调用，否则 LVGL 永久等待、界面卡死（文档 §13-7）。 */
    lv_display_flush_ready(disp);
}

static void lv_port_disp_init(void) {
    /* ① 创建显示 */
    s_disp = lv_display_create(BOARD_LCD_WIDTH, BOARD_LCD_HEIGHT);
    if (s_disp == NULL) {
        LOG_E("[LVGL] lv_display_create 失败 —— 内存池太小？看 LV_MEM_SIZE");
        return;
    }

    /* ② 颜色格式：8080 并口用**普通 RGB565**，不是 SWAPPED（文档 §5.6）。
     *    R/B 顺序由屏的 MADCTL 的 BGR 位负责（见 board_config.h）。 */
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);

    /* ③ draw buffer + 渲染模式：**单缓冲**（第二参数 NULL）+ PARTIAL。
     *    DIRECT/FULL 要 ≥ 整屏 480×320×2 = 300KB，装不下（文档 §5.6）。 */
    lv_display_set_buffers(s_disp, s_draw_buf, NULL, sizeof(s_draw_buf),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    /* ④ flush 回调 */
    lv_display_set_flush_cb(s_disp, disp_flush_cb);

    /* ⑤ 设为默认显示（多显示器才需要，但写上无副作用） */
    lv_display_set_default(s_disp);
}

/* ------------------------------------------------------------------ 触摸 */
static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
    bsp_touch_point_t pt;

    LV_UNUSED(indev);

    if (bsp_touch_read(&pt) == 1) {
        /* 坐标**已经是屏幕坐标系** —— 缩放/转置/翻转全在 bsp_touch 里做掉了，
         * 上层不做任何换算（文档 §5.7 的铁律）。 */
        data->point.x = pt.x;
        data->point.y = pt.y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        /* 读不到（I2C 错）与"没按"都算抬起 —— 不把 I2C 抖动变成假点击。 */
        data->state = LV_INDEV_STATE_RELEASED;
    }
    /* ⚠️ 不要在回调里做长阻塞操作。 */
}

static void lv_port_indev_init(void) {
    s_indev = lv_indev_create();
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_indev, touch_read_cb);
    /* v9 需要**显式**关联显示（v8 靠注册顺序）。 */
    lv_indev_set_display(s_indev, s_disp);
}

/* ------------------------------------------------------------------ 占位界面 */
/* ⚠️ 这只是"链路通不通"的证据，不是最终 UI（文档 §12 第 9~10 步）。
 *    真正的界面（波形 + 数值 + 报警灯）等屏点亮之后再搬进来 —— 否则屏不亮时
 *    分不清是"屏没驱动起来"还是"UI 写错了"。
 *    ⚠️ UI 设计红线见文档 §12.6：不做全屏动画、少用圆角/阴影/半透明
 *       （F407 **没有 DMA2D**，软件渲染最贵的路径没兜底）。 */
static void btn_event_cb(lv_event_t *e) {
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_t *label = lv_obj_get_child(btn, 0);

    lv_label_set_text(label, "clicked!");
    LOG_I("[LVGL] 按钮被点击 —— 显示 + 触摸链路都通");
}

static void build_placeholder_ui(void) {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_t *title;
    lv_obj_t *hint;
    lv_obj_t *btn;

    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), LV_PART_MAIN);

    /* ① 必须显示 "Hello LVGL"（文档 §1 判据 4） */
    title = lv_label_create(scr);
    lv_label_set_text(title, "Hello LVGL");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0x4CC9F0), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);

    hint = lv_label_create(scr);
    lv_label_set_text(hint, "ST7796S / 8080-FSMC\nlandscape 480x320\nFT6336 touch");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, lv_color_hex(0xE0E0E0), LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 0);

    /* ② 一个可点的按钮 —— 用来证明触摸真的通了（文档 §1 判据 4） */
    btn = lv_button_create(scr);
    lv_obj_set_size(btn, 160, 48);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -24);
    lv_obj_add_event_cb(btn, btn_event_cb, LV_EVENT_CLICKED, NULL);

    {
        lv_obj_t *l = lv_label_create(btn);
        lv_label_set_text(l, "tap me");
        lv_obj_center(l);
    }
}

/* ------------------------------------------------------------------ 对外 */
int app_lvgl_init(void) {
    /* ① LVGL 内核。**必须最先** —— 它会创建内部互斥量（要 FreeRTOS 堆，
     *    见 FreeRTOSConfig.h 的 configTOTAL_HEAP_SIZE）。 */
    lv_init();
    lv_log_register_print_cb(lvgl_log_cb);

    /* ② tick 来源。与 `lv_tick_inc` **二选一，不要都做**（都用动画速度会翻倍）。
     *    前提：HAL 时基已从 SysTick 挪到 TIM6（见 .ioc 的 SYS 配置）。
     *    ⚠️ 用 Device 层的 `bsp_tick_ms()` 而不是裸的 `HAL_GetTick` ——
     *       两者都是 TIM6 驱动的 1ms 计数，但前者让 Task 层不必 include HAL 头
     *       （分层规矩：Task 不直接依赖 HAL）。 */
    lv_tick_set_cb(bsp_tick_ms);

    /* ③ 屏底层。**必须在 lv_port_disp_init 之前** —— 它要能写屏。
     *    这一步会关背光 → 硬复位 → 写屏厂初始化序列（见 bsp_lcd.c）。 */
    if (bsp_lcd_init() != 0) {
        LOG_E("[LVGL] bsp_lcd_init 失败");
        return -1;
    }
    /* 触摸失败继续：只显示不触摸是可用的降级。 */
    if (bsp_touch_init() != 0) {
        LOG_W("[LVGL] 触摸未就绪，本次只做显示");
    }

    /* ④ 注册显示与输入 */
    lv_port_disp_init();
    if (s_disp == NULL) {
        return -1;
    }
    lv_port_indev_init();

    /* ⑤ 业务 UI */
    build_placeholder_ui();

    /* 到这一步屏已经初始化完，可以开背光了（之前开只会看到一段白屏）。 */
    bsp_lcd_backlight(1);

    LOG_I("[LVGL] 就绪：%lux%lu 横屏 · draw buffer %u 行（%u B）· 池 %u KB @CCM",
          (unsigned long)BOARD_LCD_WIDTH, (unsigned long)BOARD_LCD_HEIGHT,
          (unsigned)APP_LVGL_DRAW_BUF_LINES, (unsigned)sizeof(s_draw_buf),
          (unsigned)BOARD_LVGL_MEM_SIZE_KB);
    return 0;
}

void app_lvgl_run(void) {
    for (;;) {
        /* 该重绘的重绘、该读触摸的读触摸。返回值是"距下次该调用还有多少毫秒"，
         * 本层忽略它 —— 节拍由这里定。 */
        (void)lv_timer_handler();
        (void)osDelay(APP_LVGL_HANDLER_PERIOD_MS);
    }
}
