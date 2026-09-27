/* ============================================================================
 *  app_lvgl.c —— LVGL 9.5 适配层（显示 + 触摸）· 见 app_lvgl.h
 *
 *  ---- 一条数据通路（显示）----
 *      LVGL 渲染到 draw buffer（SRAM1）
 *        → lv_lcd_generic_mipi 的 flush_cb
 *            → 我们的 send_cmd  ：0x2A/0x2B 开窗（走 bsp_lcd）
 *            → 我们的 send_color：0x2C + 像素流（走 bsp_lcd）
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

#include "lvgl.h"

/* draw buffer：WIDTH × N 行 × 2B。**放 SRAM1**（普通 .bss）。
 * ⚠️ 尺寸不能太小：LVGL 的 PARTIAL 模式下每次重绘至少要能装下"一行"。
 *    APP_LVGL_DRAW_BUF_LINES 越大越省 flush 次数，但也越吃 RAM。 */
static uint8_t s_draw_buf[BOARD_LCD_WIDTH * APP_LVGL_DRAW_BUF_LINES * 2U];

static lv_display_t *s_disp = NULL;
static lv_indev_t   *s_indev = NULL;
static uint32_t      s_last_ms = 0;
static int           s_tick_started = 0;

/* ------------------------------------------------------------------ 日志 */
/* LVGL 自己的日志 → RTT（LV_USE_LOG=1 才有这条通路，lv_conf.h）。
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

/* ------------------------------------------------------------------ 显示回调 */

/* LVGL 要发命令 + 参数（开窗、MADCTL、COLMOD 等都走这里）。
 * ⚠️ `cmd` / `param` 是**字节流**，不是 16 位字 —— 8080 的命令参数
 *    一个字节占一次 16 位传输，转换在 bsp_lcd_write_data 里做。 */
static void lcd_send_cmd(lv_display_t *disp, const uint8_t *cmd, size_t cmd_size,
                         const uint8_t *param, size_t param_size) {
    LV_UNUSED(disp);
    for (size_t i = 0; i < cmd_size; i++) {
        bsp_lcd_write_cmd(cmd[i]);
    }
    if (param != NULL && param_size > 0U) {
        bsp_lcd_write_data(param, (uint32_t)param_size);
    }
}

/* LVGL 要发像素流。
 * ⚠️ 开窗（0x2A/0x2B）**已经由 flush_cb 通过 send_cmd 发过了**，这里只发
 *    写显存命令 + 数据 —— 再发一次开窗不会错，但会白费时间。
 * ⚠️ `cmd` 恒为 0x2C（LV_LCD_CMD_WRITE_MEMORY_START）。bsp_lcd_write_pixels
 *    内部自己会发这条命令，所以这里**故意忽略 cmd 参数**。 */
static void lcd_send_color(lv_display_t *disp, const uint8_t *cmd, size_t cmd_size,
                           uint8_t *param, size_t param_size) {
    LV_UNUSED(disp);
    LV_UNUSED(cmd);
    LV_UNUSED(cmd_size);
    bsp_lcd_write_pixels(param, (uint32_t)param_size);
}

/* ------------------------------------------------------------------ 触摸回调 */

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
    bsp_touch_point_t pt;

    LV_UNUSED(indev);

    if (bsp_touch_read(&pt) == 1) {
        data->point.x = pt.x;
        data->point.y = pt.y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        /* 读不到（I2C 错）与"没按"都算抬起 —— 不把 I2C 抖动变成假点击。 */
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

/* ------------------------------------------------------------------ 占位界面 */
/* ⚠️ 这只是"链路通不通"的证据，不是最终 UI。
 *    真正的界面（4 个页面 + 曲线）等屏点亮之后再搬进来 —— 否则屏不亮时
 *    分不清是"屏没驱动起来"还是"UI 写错了"。 */
static void build_placeholder_ui(void) {
    lv_obj_t *scr = lv_screen_active();

    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "G2026  LVGL 9.5 ok");
    lv_obj_set_style_text_color(title, lv_color_hex(0x4CC9F0), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);

    lv_obj_t *hint = lv_label_create(scr);
    lv_label_set_text(hint, "ST7796 / 8080-FSMC\n320x480\n点一下屏幕试试");
    lv_obj_set_style_text_color(hint, lv_color_hex(0xE0E0E0), LV_PART_MAIN);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 0);
}

/* ------------------------------------------------------------------ 对外 */
int app_lvgl_init(void) {
    /* ① 屏的硬复位与背光控制在 Device 层（bsp_lcd_init）—— 这里只开背光，
     *    等 LVGL 把屏初始化完再开，否则用户会看到一段花屏。 */
    if (bsp_lcd_init() != 0) {
        LOG_E("[LVGL] bsp_lcd_init 失败");
        return -1;
    }

    /* ② LVGL 内核。⚠️ 必须在创建任何对象之前；日志回调先挂上，
     *    lv_init() 自己的告警才不会漏。 */
    lv_log_register_print_cb(lvgl_log_cb);
    lv_init();

    /* ③ 显示：用 LVGL 自带的 ST7796 驱动（初始化序列是权威来源，见文件头）。
     * ⚠️ `LV_LCD_FLAG_*` 决定 MADCTL 的镜像/红蓝序。这里先用 NONE，
     *    上板若发现**画面上下颠倒**或**红蓝互换**，改这一行：
     *      上下颠倒 → LV_LCD_FLAG_MIRROR_Y
     *      左右颠倒 → LV_LCD_FLAG_MIRROR_X
     *      红蓝互换 → LV_LCD_FLAG_BGR
     *    **只改这一处** —— 同时改别处（比如 bsp_lcd）会让问题变成两个来源。 */
    s_disp = lv_st7796_create(BOARD_LCD_WIDTH, BOARD_LCD_HEIGHT, LV_LCD_FLAG_NONE,
                              lcd_send_cmd, lcd_send_color);
    if (s_disp == NULL) {
        LOG_E("[LVGL] lv_st7796_create 失败 —— 内存池太小？看 LV_MEM_SIZE");
        return -1;
    }

    /* ④ 颜色格式：屏要**大端** RGB565，用 SWAPPED 让 LVGL 在绘制时顺手换掉。 */
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565_SWAPPED);

    /* ⑤ draw buffer：PARTIAL 模式（只重绘脏区，320×480 全帧 300KB 装不下）。 */
    lv_display_set_buffers(s_disp, s_draw_buf, NULL, sizeof(s_draw_buf),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    /* ⑥ 触摸：FT6336 走 I2C2。初始化失败也继续 —— 只显示不触摸是可用的降级。 */
    if (bsp_touch_init() == 0) {
        s_indev = lv_indev_create();
        lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(s_indev, touch_read_cb);
    } else {
        LOG_W("[LVGL] 触摸未就绪，本次只做显示");
    }

    build_placeholder_ui();

    bsp_lcd_backlight(1); /* 到这一步屏已经初始化完，可以开背光了 */
    LOG_I("[LVGL] 就绪：%lux%lu · draw buffer %u 行（%u B）· 池 %u KB @CCM",
          (unsigned long)BOARD_LCD_WIDTH, (unsigned long)BOARD_LCD_HEIGHT,
          (unsigned)APP_LVGL_DRAW_BUF_LINES, (unsigned)sizeof(s_draw_buf),
          (unsigned)BOARD_LVGL_MEM_SIZE_KB);
    return 0;
}

void app_lvgl_poll(uint32_t now_ms) {
    if (s_disp == NULL) {
        return;
    }

    /* LVGL 的时基靠"两次调用的差值"推进。
     * ⚠️ 第一次调用只记时间、不推 —— 否则会把"从上电到第一次调用"的
     *    那一大段（几秒）当成一次 tick 灌进去，动画会瞬移。 */
    if (!s_tick_started) {
        s_tick_started = 1;
        s_last_ms = now_ms;
        return;
    }
    {
        uint32_t dt = now_ms - s_last_ms; /* 无符号减法，天然处理回绕 */
        s_last_ms = now_ms;
        if (dt > 0U) {
            lv_tick_inc(dt);
        }
    }

    /* 跑一次 LVGL 的定时器：该重绘的重绘、该读触摸的读触摸。
     * 返回"距离下次该调用还有多少毫秒"，本层忽略它 —— 节拍由 Task 层定。 */
    (void)lv_timer_handler();
}
