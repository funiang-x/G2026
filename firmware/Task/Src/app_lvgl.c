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
#if APP_LVGL_USE_DEMO
#if APP_LVGL_USE_DEMO
#include "lv_demos.h" /* 官方 widgets demo（开关见 project_config.h）；=0 时不必包含 */
#endif
#endif

/* draw buffer：WIDTH × N 行 × 2B。**放 SRAM1**（普通 .bss）。
 * ⚠️ 尺寸不能太小：LVGL 的 PARTIAL 模式下每次重绘至少要能装下"一行"。
 *    APP_LVGL_DRAW_BUF_LINES 越大越省 flush 次数，但也越吃 RAM。 */
static uint8_t s_draw_buf[BOARD_LCD_WIDTH * APP_LVGL_DRAW_BUF_LINES * 2U];

static lv_display_t *s_disp = NULL;
static lv_indev_t   *s_indev = NULL;
static uint32_t      s_last_ms = 0;
static int           s_tick_started = 0;

/* 诊断浮层的状态（实现见下方 `dbg_overlay_create()`）；
 * `s_tp_*` 由触摸回调写、poll 里读（单任务访问，无需加锁）。 */
static lv_obj_t *s_dbg_label = NULL;
static lv_obj_t *s_dbg_dot = NULL;

/* ★★ 临时诊断（2026-10-02 花屏排查，测完删除）：当前显示方向（度）。
 *    由 app_lvgl_poll 里的 8 秒轮换驱动，并显示在诊断浮层末尾（R90 = 90°）。 */
static uint16_t  s_rot_deg = 90U;
static uint16_t  s_tp_x = 0;
static uint16_t  s_tp_y = 0;
static int       s_tp_down = 0;

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
 *    内部自己会发这条命令，所以这里**故意忽略 cmd 参数**。
 * 顺带统计 flush 像素：顶栏的"等效 fps"= 每秒刷屏像素 / 整帧像素 ——
 * 性能可观测（PARTIAL 模式下一次重绘会被切成多块，数"块数"没有意义）。 */
static uint32_t s_flush_px = 0;        /* 窗口内累计像素 */

static void lcd_send_color(lv_display_t *disp, const uint8_t *cmd, size_t cmd_size,
                           uint8_t *param, size_t param_size) {
    LV_UNUSED(cmd);
    LV_UNUSED(cmd_size);

    /* ---- 分块写 + 每块抢一次 LCD 总线闸（2026-10-02）----
     * 采样窗静默要求"采样窗内 FSMC 一次都不动"，而一次 flush（320×48px≈9ms）
     * 比采样窗（2.048ms）长得多 ⇒ 只有切成 `APP_LCD_CHUNK_PX` 像素一块、每块
     * **开写前**抢闸，engine 才能在块边界把总线拿过去。抢不到就在块边界阻塞，
     * 等采样窗结束再接着写（flush 被推迟 ≤2ms，屏刷新率本就不敏感）。
     * 闸与理由见 bsp_lcd.h 的"LCD 总线闸"、project_config.h 的 APP_LCD_CHUNK_PX。
     * ⚠️ 多次调 bsp_lcd_write_pixels 会重复发 0x2C —— 无害（控制器接着当前
     *    GRAM 指针继续写），只是每块多一次命令开销。 */
    const uint32_t chunk = APP_LCD_CHUNK_PX * 2U; /* RGB565：2 B/px */
    uint32_t       off   = 0;
    while (off < (uint32_t)param_size) {
        uint32_t n = (uint32_t)param_size - off;
        if (n > chunk) {
            n = chunk;
        }
        (void)bsp_lcd_bus_take(APP_LCD_BUS_WAIT_MS);
        bsp_lcd_write_pixels(param + off, n);
        bsp_lcd_bus_give();
        off += n;
    }
    s_flush_px += (uint32_t)param_size / 2U;

    /* ★ 必须回这一句：LVGL 的 flush 是**异步握手** —— 它发完一块就等
     *   `lv_display_flush_ready()`，等到才继续下一块（见 LVGL 源码
     *   `drivers/display/lcd/lv_lcd_generic_mipi.c` 里那句 note）。
     *   本工程的 `bsp_lcd_write_pixels` 是**同步**写（CPU 直写 FSMC，写完才返回），
     *   所以紧接着调就是正确的。
     *   ⚠️ 2026-10-02 上板实测漏了它的现象：屏上只出现**一条 48 行的白带**
     *      （= 一个 draw buffer），其余保持上电雪花、界面永不出现。
     *      本文件头第 9 行早就写了这条，只是漏了实现 —— 注释不是实现。 */
    lv_display_flush_ready(disp);
}

/* 每秒一次：结算窗口内的像素吞吐 → 等效 fps（0.1 定点，UI 直接显示）。
 * 等效 fps = 像素吞吐 / 全帧像素（320×480）。静屏时为 0（没有重绘没有耗电），
 * 滑页/动画时逼近 60 —— 这是"带宽用满没有"的诚实口径，不是渲染节拍。 */
uint32_t app_lvgl_fps_x10(uint32_t now_ms) {
    static uint32_t s_win_ms = 0;    /* 窗口起点（避免与 app_lvgl_poll 的同名局部撞 -Wshadow） */
    static uint32_t s_win_px = 0;
    static uint32_t s_fps_x10 = 0;
    static uint8_t  s_init = 0;

    if (!s_init) {
        s_init = 1;
        s_win_ms = now_ms;
        s_win_px = s_flush_px;
        return 0U;
    }
    const uint32_t dt = now_ms - s_win_ms;
    if (dt < 1000U) {
        return s_fps_x10;
    }
    const uint32_t px = s_flush_px - s_win_px;
    s_fps_x10 = (uint32_t)((uint64_t)px * 10000U /
                           ((uint64_t)dt * BOARD_LCD_WIDTH * BOARD_LCD_HEIGHT));
    s_win_ms = now_ms;
    s_win_px = s_flush_px;
    return s_fps_x10;
}

/* ------------------------------------------------------------------ 触摸回调 */

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
    bsp_touch_point_t pt;

    LV_UNUSED(indev);

    if (bsp_touch_read(&pt) == 1) {
        data->point.x = pt.x;
        data->point.y = pt.y;
        data->state = LV_INDEV_STATE_PRESSED;

        /* 诊断浮层：记下坐标、把红点挪过去（本回调跑在 lv_timer_handler 里，
         * 调 LVGL API 是安全的）。 */
        s_tp_x = pt.x;
        s_tp_y = pt.y;
        s_tp_down = 1;
        if (s_dbg_dot != NULL) {
            lv_obj_set_pos(s_dbg_dot, (int32_t)pt.x - 9, (int32_t)pt.y - 9);
            lv_obj_remove_flag(s_dbg_dot, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        /* 读不到（I2C 错）与"没按"都算抬起 —— 不把 I2C 抖动变成假点击。 */
        data->state = LV_INDEV_STATE_RELEASED;
        s_tp_down = 0;
        if (s_dbg_dot != NULL) {
            lv_obj_add_flag(s_dbg_dot, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* ------------------------------------------------------------------ 占位界面 */
/* ⚠️ 这只是"链路通不通"的证据，不是最终 UI。
 *    真正的界面（4 个页面 + 曲线）等屏点亮之后再搬进来 —— 否则屏不亮时
 *    分不清是"屏没驱动起来"还是"UI 写错了"。 */
/* ---------------------------------------------------------- 诊断浮层 ----
 * 帧率 + 触控坐标 + 触点标记。
 *
 * ★ 为什么建在 `lv_layer_top()` 上：top 层浮在**所有页面之上**，切页面不会丢，
 *   而且业务 UI（app_ui 的四页）不用知道它存在 —— 加/去都不动业务代码。
 *   ⚠️ 同时把 top 层的 CLICKABLE 去掉，否则它会把触摸事件全吃掉、界面点不动。
 *
 * ★ 两个数怎么读：
 *   · `fps` —— 等效帧率（每秒刷屏像素 / 整帧像素，0.1 定点）。**静屏时为 0.0
 *     是正常的**（没有重绘就没有耗电）；滑页/动画时应逼近 60。
 *   · `touch x,y` —— **LVGL 坐标系**（= 显示旋转后的坐标系，横屏下是 480×320）。
 *     若红点位置与手指对不上，要改的是 `bsp_touch_read()` 的坐标映射，**不是这里**。
 *     一直显示 `--,--` ⇒ 触摸没通（看 RTT 里 [TOUCH] 那几行）。
 * （静态变量声明在文件顶部，因为触摸回调也要用。） */
static void dbg_overlay_create(void) {
    lv_obj_t *top = lv_layer_top();
    lv_obj_remove_flag(top, LV_OBJ_FLAG_CLICKABLE); /* 别把触摸事件吃掉 */

    s_dbg_label = lv_label_create(top);
    lv_obj_set_style_text_color(s_dbg_label, lv_color_hex(0x20E070), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_dbg_label, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_dbg_label, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_dbg_label, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(s_dbg_label, 4, LV_PART_MAIN);
    lv_label_set_text(s_dbg_label, "fps  --.-   touch --,--");
    lv_obj_align(s_dbg_label, LV_ALIGN_TOP_LEFT, 4, 4);

    /* 触点标记：一个空心圆，按下时移到触点、抬起时藏起来。 */
    s_dbg_dot = lv_obj_create(top);
    lv_obj_remove_style_all(s_dbg_dot);
    lv_obj_set_size(s_dbg_dot, 18, 18);
    lv_obj_set_style_radius(s_dbg_dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_dbg_dot, lv_color_hex(0xFF3820), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_dbg_dot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_dbg_dot, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_dbg_dot, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_add_flag(s_dbg_dot, LV_OBJ_FLAG_HIDDEN);
}

#if !APP_LVGL_USE_DEMO
/* ⚠️ 只在业务 UI 模式编译：demo 模式下这里没有调用点（零告警口径）。 */
static void build_placeholder_ui(void) {
    lv_obj_t *scr = lv_screen_active();

    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "G2026  LVGL 9.5 ok");
    lv_obj_set_style_text_color(title, lv_color_hex(0x4CC9F0), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);

    lv_obj_t *hint = lv_label_create(scr);
    /* ⚠️ 写的是**逻辑分辨率**（旋转后的），不是面板原生 320×480 ——
     *    显示已设成横屏（见 app_lvgl_init 里的 lv_display_set_rotation）。 */
    lv_label_set_text(hint, "ST7796 / 8080-FSMC\n480x320（横屏）\n点一下屏幕试试");
    lv_obj_set_style_text_color(hint, lv_color_hex(0xE0E0E0), LV_PART_MAIN);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 0);
}
#endif /* !APP_LVGL_USE_DEMO */

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

    /* ⚠️ 本函数跑在**调度器启动前**（app_init 里），下面这句是那次踩坑的代价
     *    （2026-10-02 上板实测，缺它就起不来）：
     *    `lv_st7796_create()` 的初始化序列里有 `lv_delay_ms()`，而它默认等的是
     *    `lv_tick_inc()` 推进的内部计数 —— 那要调度器起来后的任务来推 ⇒ 死等。
     *    把 tick 源接到 Device 层的毫秒时基（TIM6），调度器前后都能走。
     *    （`lv_tick_inc` 随之成为空操作，留着无害，见 app_lvgl_task。）
     *    ⚠️ 这里**不再**需要清 BASEPRI：lv_conf.h 已把 LV_USE_OS 定为 LV_OS_NONE，
     *       LVGL 不再建/取 FreeRTOS 互斥量，也就不会把中断钉住。 */
    lv_tick_set_cb(bsp_tick_ms);

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

    /* ★★ 面板反显：必须发一条 INVON(0x21)，否则整屏颜色取反。
     *    how：2026-10-02 用 J-Link 绕过 LVGL 直接往面板 GRAM 写已知值，实测
     *    "写进去的是补色"—— 写 0xFFFF(白)→显黑、写 0xF800(红)→显青、
     *    写 0x07E0(绿)→显洋红、写 0x001F(蓝)→显黄，4/4 全中；
     *    同一时刻把 LVGL 的 draw buffer 抓出来在 PC 上还原，画面是**正常的浅色 UI**
     *    ⇒ 渲染/取数/字节序都对，坏的只有"面板显示极性"这一处。
     *    面板上电默认 INVOFF（`lv_lcd_generic_mipi_create` 不碰 INV），
     *    这块模块要 INVON ⇒ 补这一条。
     *    ⚠️ 只改这里：不要去 bsp_lcd 或 MADCTL 里动颜色（会让问题变成两个来源）。
     *    ⚠️ 若拿到的模块批次相反（补这条后反而取反了），把 true 改成 false。 */
    lv_st7796_set_invert(s_disp, true);

    /* ④ 颜色格式：屏要**大端** RGB565，用 SWAPPED 让 LVGL 在绘制时顺手换掉。 */
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565_SWAPPED);

    /* ⑤ draw buffer：PARTIAL 模式（只重绘脏区，320×480 全帧 300KB 装不下）。 */
    lv_display_set_buffers(s_disp, s_draw_buf, NULL, sizeof(s_draw_buf),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    /* ★ 显示方向 = **横屏 480×320**（面板 MADCTL 硬件旋转 90°，不花 CPU、不动绘制缓冲）。
     *   why 横屏：官方 widgets demo（本项目轻量版）本身就是按 **480×320 横向**排的版。
     *   2026-10-02 上板实测教训：把这里改成竖屏 320×480，demo 那套控件在 320 宽里装不下，
     *   LVGL 会把超宽内容自动**滚动**起来 —— 屏上表现为"整幅画被往右下角扯 + 撕裂"，
     *   很容易误判成显示坏了（实际是**版式放不下**，不是驱动/面板问题）。
     *   ⚠️ 业务 UI（app_ui.c）目前是**按竖屏 320×480 排的版** ⇒ 横屏下会被裁。
     *      · 想快速验证业务 UI：这一行换 LV_DISPLAY_ROTATION_0（其余不动）；
     *      · 想产品走横屏：要做的是把 app_ui.c 重排成 480×320（视觉重构那一步的事）。
     *   ⚠️ 换方向时**只改这一处**，别去动 MADCTL / bsp_lcd，否则问题变成两个来源。
     *   ⚠️ 触摸坐标映射要跟着方向走 —— 见 bsp_touch_read() 里的说明。 */
    lv_display_set_rotation(s_disp, LV_DISPLAY_ROTATION_90);

    /* ⑥ 触摸：FT6336 走 I2C2。初始化失败也继续 —— 只显示不触摸是可用的降级。 */
    if (bsp_touch_init() == 0) {
        s_indev = lv_indev_create();
        lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(s_indev, touch_read_cb);
    } else {
        LOG_W("[LVGL] 触摸未就绪，本次只做显示");
    }

#if APP_LVGL_USE_DEMO
    /* ★ 官方 widgets demo（开关见 project_config.h）：一整套控件，用来验证
     *   显示 + 触摸。验证完把 `APP_LVGL_USE_DEMO` 改回 0 就回到业务 UI。
     *   ⚠️ demo 是 9.2 版（从 docs/ 的 ESP32 例程拷来），主版本相同、API 未变。 */
    lv_demo_widgets();

    /* ★★ 诊断（2026-10-02，测完可留）：demo 建完后的 LVGL 堆余量。
     *    why：LVGL 的 `lv_malloc` 失败只按 **INFO** 级打印，而本工程日志级 = WARN
     *    ⇒ 堆耗尽在日志里是**静默**的（屏上缺块/错乱，一个字都不报）。 */
    {
        lv_mem_monitor_t mon;
        lv_mem_monitor(&mon);
        LOG_I("[LVGL] 堆：free=%u B（maxblk=%u B, frag=%u%%, total=%u B, used=%u%%）",
              (unsigned)mon.free_size, (unsigned)mon.free_biggest_size,
              (unsigned)mon.frag_pct, (unsigned)mon.total_size, (unsigned)mon.used_pct);
    }
#else
    build_placeholder_ui();
#endif
    dbg_overlay_create(); /* 诊断浮层：帧率 + 触控坐标 + 触点标记 */

    bsp_lcd_backlight(1); /* 到这一步屏已经初始化完，可以开背光了 */
    /* ⚠️ 打的是 **LVGL 实际生效的分辨率**，不是 board_config 的常量 ——
     *    两者在"设了旋转"之后会不一样（旋转会把逻辑分辨率换成 480×320）。
     *    2026-10-02 之前这里打的是常量，于是"旋转到底生效没有"从日志上看不出来。 */
    LOG_I("[LVGL] 就绪：%ldx%ld · draw buffer %u 行（%u B）· 池 %u KB @CCM",
          (long)lv_display_get_horizontal_resolution(s_disp),
          (long)lv_display_get_vertical_resolution(s_disp),
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

    /* （2026-10-02 的"每 8 秒换一个方向"诊断已撤：根因是**面板反显**，不是方向。
     *   留档：轮换期间四个角度现象一致，恰是"颜色整体取反"的表现 —— 见上面的 INVON 说明。） */

    /* 跑一次 LVGL 的定时器：该重绘的重绘、该读触摸的读触摸。
     * 返回"距离下次该调用还有多少毫秒"，本层忽略它 —— 节拍由 Task 层定。 */
    (void)lv_timer_handler();

    /* ---- 诊断浮层刷新（帧率 + 触控坐标 + 当前方向）----
     * 放在 `lv_timer_handler()` **之后**：这一拍重绘了多少像素，fps 才统计得准。
     * `R%u` 就是当前方向（度）—— 它同时也是"这一版渲染对不对"的判据。 */
    if (s_dbg_label != NULL) {
        const uint32_t fps_x10 = app_lvgl_fps_x10(now_ms);
        if (s_tp_down != 0) {
            lv_label_set_text_fmt(s_dbg_label, "fps %lu.%lu   touch %u,%u  DOWN  R%u",
                                  (unsigned long)(fps_x10 / 10U), (unsigned long)(fps_x10 % 10U),
                                  (unsigned)s_tp_x, (unsigned)s_tp_y, (unsigned)s_rot_deg);
        } else {
            lv_label_set_text_fmt(s_dbg_label, "fps %lu.%lu   touch --,--  up  R%u",
                                  (unsigned long)(fps_x10 / 10U), (unsigned long)(fps_x10 % 10U),
                                  (unsigned)s_rot_deg);
        }
    }
}
