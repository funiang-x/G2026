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
 *    · **正常路径只读** —— 例程从不写寄存器，没有 GT911 那种"写 0 清标志"步骤；
 *      唯一例外是 **`DEVICE_MODE(0x00) ← Working`**：空闲保活 + 读到非法值时唤醒
 *      （2026-10-13 五轮实测加，为什么必须写见 touch_wake 的注释）；
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

#include "bsp_gpio.h" /* 读 I2C 两条线的空闲电平（见 touch_scan_bus） */
#include "bsp_tick.h" /* bsp_tick_ms()：失败退避与"垃圾读数"汇总日志的时间基 */
#include "i2c.h"      /* CubeMX 生成的 hi2c2（Device 层允许依赖 Core 的句柄） */
#include "stm32f4xx_hal.h"

/* ---- FT6336 寄存器（8 位地址；出处见文件头）---- */
#define FT6336_REG_TD_STATUS 0x02U /* 低 4 位 = 触点数（合法值只有 0..5）*/
#define FT6336_REG_POINT1 0x03U    /* 点 1：第一轴高/低、第二轴高/低 4 字节 */

/* ★ 2026-10-14 八轮：**"写 DEVICE_MODE=Working"的唤醒/保活整条删掉了**。
 *   当时的假设：芯片空闲后会掉进 monitor 态（`TD_STATUS` 恒读 0xFF），写一下能拽回来。
 *   板上实测把它证伪 —— 日志原文：`写 DEVICE_MODE=Working（唤醒）后 TD_STATUS=FF
 *   （仍非法 —— 写不管用，等触摸唤醒）`：写进去不改变读数，而**有触摸时它自己会回来**。
 *   留着它的代价是实打实的：每条路径都多发一次 I2C 写 + 回读（抖动时 5~15ms），
 *   还引入了本驱动唯一一处"写寄存器"。
 *   ⇒ 判据不变：**认 `TD_STATUS` 低 4 位 ≤5 才算有效**，读到非法值按"没按"处理
 *     （`touch_bad_log()` 仍计数并 30s 汇总）。要恢复实验：从 git 历史取
 *     `touch_wake()` / `ft6336_write()`。 */

/* ⚠️ 超时**必须短**（2026-10-13 四轮，板上实测的教训）：
 *   本读法跑在 LVGL 的 indev 回调里（每 ~16ms 一次），而板上 I2C 读失败率实测
 *   ≈45%（5s 内 140 次）。原 100ms 超时 ⇒ **每次失败把 UI 任务按在 HAL 里 100ms**
 *   ⇒ 一秒里 UI 的"预算"全耗在超时上：现象就是**屏像冻住、触摸没反应、日志断档**
 *   （2026-10-13 RTT 实测）。
 *   一次正常读只要 ≈0.6ms（100kHz × 5 字节）⇒ 5ms 已是 8 倍余量。 */
/* ★★ 2026-10-14：**这个值不许改小** —— 试过收到 2 ms，实测**更糟并已回退**：
 *   读均 4.5 ms → **9.9~22.8 ms**、失败 8~17 次/秒、触摸直接不能用。
 *   机理：中途掐断会把 I2C 总线**留在坏状态** ⇒ 重试也失败 ⇒ 触发退避(100ms)+恢复流程
 *   ⇒ 每轮 10~23 ms，比省下的 3 ms 贵 3~7 倍。**缩短它会"慢"变"坏"。**
 *   （完整账与再压它的两条路见 `docs/03_LVGL刷屏优化评估-20261014.md §8.1 D`。）*/
#define FT6336_I2C_TIMEOUT_MS 5U

/* 读失败后的退避窗：别拿 UI 的时间反复去撞一条卡住的总线。
 * 退避期内 `bsp_touch_read` 直接返回 -1（= 没按），一次 I2C 都不发。 */
#define FT6336_BACKOFF_MS 100U

/* ★ 连续读失败到这么多次 ⇒ 试一次**运行期总线解锁**（2026-10-13 四轮）。
 *   现场实测（RTT）：触摸正常若干秒后，I2C **100% 读失败且永不恢复**，直到重启
 *   —— 这正是"触摸几秒后失效"的真相（从机被一次噪声/半截传输卡住，一直按住
 *   SDA ⇒ 主设备再也做不出 START）。工程里那套 "9 个时钟 + STOP" 解锁原来只在
 *   **开机诊断**里跑，运行期没有 ⇒ 这里补上。 */
#define FT6336_UNSTICK_AFTER 4U
/* 两次解锁之间的最小间隔：别在一条真死的线上反复折腾（解锁本身很快，但这属于
 * "有副作用的动作"，要限频）。 */
#define FT6336_UNSTICK_GAP_MS 500U

/* HAL 的地址要 7 位地址左移 1 位 */
#define FT6336_HAL_ADDR ((uint16_t)(BOARD_TOUCH_I2C_ADDR << 1))

static int      s_ready = 0;
static uint32_t s_init_try_ms = 0; /* 上次"重试初始化"的时刻（见 bsp_touch_read）*/
static uint32_t s_retry_until = 0; /* 总线退避截止时刻（0 = 不退避）*/
static uint32_t s_fail_run = 0;    /* 连续读失败计数（到阈值触发解锁）*/
static uint32_t s_unstick_ms = 0;  /* 上一次总线解锁的时刻 */
static uint32_t s_bad_cnt = 0;     /* 窗口内"无效数据"次数 */
static uint32_t s_bad_ms = 0;      /* 上一次汇总打印的时刻 */

/* 读到**非法**数据（TD_STATUS 越界 / (0,0) 假点）时计数，30s 汇总一行。
 * ⚠️ 2026-10-13 五轮：**本屏实测未触摸时 TD_STATUS 就可能读成 FF**（不是 0x00），
 *   所以这条**不代表坏** —— 判"触摸能不能用"要看**按下去有没有 `[TOUCH] 按下 (x,y)`**；
 *   只要按得出来，这条汇总可以完全忽略（它只统计"没触摸时的读数长什么样"）。
 * ⚠️ 为什么不能当"按下"：LVGL 拿到一个按下的指针就会持续拖 tileview
 *    （每帧全屏重绘 + 每次跨页触发重测）—— 板上现象就是"帧率掉 / CPU 满 /
 *    触摸没反应"（2026-10-13 实测：开机那条 TD_STATUS=FF 就造了个 (0,0) 假按下）。 */
static void touch_bad_log(void) {
    const uint32_t now = bsp_tick_ms();
    s_bad_cnt++;
    if (s_bad_ms == 0U || (uint32_t)(now - s_bad_ms) >= 30000U) {
        LOG_W("[TOUCH] 近 30s 读到 %lu 次无效数据（TD_STATUS 越界 / (0,0) 假点）——"
              "**未触摸时本屏 TD_STATUS 就可能读 FF，属常态**；判据是「按下 (x,y)」"
              "能不能报出来（能报就没人管这条）",
              (unsigned long)s_bad_cnt);
        s_bad_cnt = 0U;
        s_bad_ms = now;
    }
}

/* ------------------------------------------------------------------ 内部 */

/* 扫一遍 I2C2 总线（7 位地址 0x08~0x77），把有应答的地址打出来。
 *
 * ★ 为什么需要它（2026-10-02 加，起因是"屏亮了但完全没触控"）：
 *   屏厂《液晶屏通用说明》明说电容触摸分 **GT911** 与 **FT5XXX/FT6XXX** 两大派系，
 *   而本屏到底是哪一颗**一直没落实**（`hardware/04 §6.10-②` 挂着"请读一下丝印
 *   型号"；实物上那块 IC 被 FPC 压着，照片读不出丝印）。
 *   两派地址完全不同（GT911 = 0x5D/0x14，FT6336 = 0x38），寄存器宽度也不同
 *   （16 位 vs 8 位）⇒ **0x38 无应答时，扫一遍就能区分两种完全不同的故障**：
 *     · 扫到 0x5D / 0x14  ⇒ 是 GT911：改地址 + 改 16 位寄存器读法，**不是接线问题**；
 *     · 一个都没有        ⇒ 总线问题：上拉 / SCL·SDA 接线 / 屏供电。
 *   没有这一步，两种现象都是"读不到"，只能靠猜。
 *
 * 代价：最坏 112 个地址 × 5ms ≈ 0.6s（只在自检里跑一次，可接受）。 */
/* 判"这条线上**到底有没有上拉**" —— 唯一可靠的**有源**测法。
 *
 * 为什么不能断电测电阻：这条路径上有 **ESD 保护二极管**（触摸 IC 的、MCU 的），
 * 电阻档在路测半导体时读数**随表笔极性变**。2026-10-02 用户实测：模块 `SCK`↔3V3
 * 红黑互换得 **8.4k / 14k** 两个不同的值 ⇒ 那压根不是电阻，读数无意义。
 *
 * 正确的判据是上拉的**定义**：把线拉低后**松开，看它能不能自己回到高**。
 *   · 有上拉 ⇒ 松手后立刻回 1；
 *   · 没上拉 ⇒ 停在 0（或极慢地漂）。
 * 这也顺带解释了为什么"读空闲电平"不能当判据 —— 悬空脚也常读 1，只有
 * "松手后能自己回高"才证明有源在拉它。
 *
 * ⚠️ 本函数会把引脚改成普通开漏输出，**调用后必须重新 MX_I2C2_Init() 恢复 AF**。 */
static int line_has_pullup(bsp_gpio_t g) {
    bsp_gpio_config(g, BSP_GPIO_OUT_OD, 0U);
    bsp_gpio_write(g, 0); /* 拉低 */
    for (volatile int i = 0; i < 4000; i++) {
    }
    /* ★ **先判"能不能拉下去"**（2026-10-12 新增，补掉一个致命盲区）：
     *   只看"松手能否回高"是不够的 —— 线被**硬顶到 3V3**（0Ω 上拉 / 焊桥 / 外部推挽）
     *   时它照样"能回高"，测试通过，但主设备**永远做不出 START**：
     *   现象 = 扫遍全地址无人应答 + `HAL_TIMEOUT`（而不是 NACK）、`SR1.SB` 从不置位。
     *   2026-10-12 实测：本板 **SCL 拉不下去、SDA 能拉下去** ⇒ 触摸整条链全废，
     *   而当时的测试两条线都报"有上拉"。 */
    const int pulled_low = (bsp_gpio_read(g) == 0);
    bsp_gpio_write(g, 1); /* 开漏写 1 = 释放（高阻） */
    for (volatile int i = 0; i < 4000; i++) {
    } /* ≈170µs，够 40k×200pF 充满 */
    if (pulled_low == 0) {
        LOG_E("[TOUCH] **这条线拉不下去**：开漏驱动后仍读到 1 ⇒ 被硬顶到 3V3"
              "（0Ω 上拉 / 焊桥 / 外部推挽）—— I2C 主设备做不出 START，"
              "现象是「扫遍全地址无人应答 + HAL_TIMEOUT」。"
              "查 R15(SCL)/R16(SDA) 两个上拉位与触摸座附近的 0Ω 选件");
        return 0;
    }
    return bsp_gpio_read(g) == 1;
}

/* ★ I²C 总线恢复（2026-10-13 新增）。
 *
 * 从机若在传输**中途**被复位/卡住，会**一直按住 SDA**（实测：SDA 5000 次采样全为低）。
 * 之后主设备再也做不出 START ⇒ 现象就是"扫遍全地址无人应答"。
 * 标准解法：主机只放 SCL，送 **9 个时钟脉冲**（把从机残留的一位走完），再补一个 **STOP**。
 *
 * 本函数把恢复前后的线电平打进日志 —— 两种故障能一眼分开：
 *   · **能脉冲、SDA 随后释放** ⇒ 是从机挂起（本次恢复已解决）；
 *   · **SCL 根本拉不下去 / SDA 一直不放** ⇒ 是**外部硬件把线钉死**（焊桥 / 0Ω / 推挽驱动），
 *     软件无法恢复。
 * ⚠️ 调用后引脚已被改成普通开漏输出，调用者必须重新 `MX_I2C2_Init()` 恢复 AF。 */
static void touch_bus_recover(void) {
    const bsp_gpio_t scl = {(bsp_gpio_port_t)BOARD_TOUCH_I2C_SCL_PORT,
                            (uint8_t)BOARD_TOUCH_I2C_SCL_PIN};
    const bsp_gpio_t sda = {(bsp_gpio_port_t)BOARD_TOUCH_I2C_SDA_PORT,
                            (uint8_t)BOARD_TOUCH_I2C_SDA_PIN};
    unsigned pulses = 0U;

    bsp_gpio_config(scl, BSP_GPIO_OUT_OD, 0U);
    bsp_gpio_config(sda, BSP_GPIO_OUT_OD, 0U);
    bsp_gpio_write(scl, 1); /* 释放两条线 */
    bsp_gpio_write(sda, 1);
    for (volatile int i = 0; i < 4000; i++) {
    }
    const int busy_scl = bsp_gpio_read(scl);
    const int busy_sda = bsp_gpio_read(sda);

    for (unsigned i = 0U; i < 9U; i++) {
        bsp_gpio_write(scl, 0);
        for (volatile int k = 0; k < 400; k++) {
        }
        if (bsp_gpio_read(scl) != 0) {
            break; /* 拉不下去 ⇒ 外面钉死，脉冲无意义 */
        }
        bsp_gpio_write(scl, 1);
        for (volatile int k = 0; k < 400; k++) {
        }
        if (bsp_gpio_read(scl) == 0) {
            break; /* 放不开 ⇒ 同样是被钉死 */
        }
        pulses++;
        if (bsp_gpio_read(sda) == 1) {
            break; /* 从机已放线，收工 */
        }
    }

    /* STOP：SCL 保持高，SDA 由低 → 高 */
    bsp_gpio_write(sda, 0);
    for (volatile int k = 0; k < 400; k++) {
    }
    bsp_gpio_write(scl, 1);
    for (volatile int k = 0; k < 400; k++) {
    }
    bsp_gpio_write(sda, 1);
    for (volatile int k = 0; k < 400; k++) {
    }

    LOG_W("[TOUCH] 总线恢复：脉冲 %u/9；忙线时 SCL=%d SDA=%d，恢复后 SCL=%d SDA=%d"
          "（SCL 拉不下去或 SDA 一直不放 ⇒ 外部硬件钉死，软件救不了）",
          pulses, busy_scl, busy_sda, bsp_gpio_read(scl), bsp_gpio_read(sda));
}

static void touch_scan_bus(void) {
    unsigned found = 0U;

    /* ① **先判上拉有没有**（改引脚，测完恢复）—— 这一步比后面扫地址更能定性：
     *    开漏 I²C 没有上拉，扫多少地址都是白扫。 */
    {
        const bsp_gpio_t scl = {(bsp_gpio_port_t)BOARD_TOUCH_I2C_SCL_PORT,
                                (uint8_t)BOARD_TOUCH_I2C_SCL_PIN};
        const bsp_gpio_t sda = {(bsp_gpio_port_t)BOARD_TOUCH_I2C_SDA_PORT,
                                (uint8_t)BOARD_TOUCH_I2C_SDA_PIN};
        const int pu_scl = line_has_pullup(scl);
        const int pu_sda = line_has_pullup(sda);
        if (pu_scl == 0 || pu_sda == 0) {
            LOG_E("[TOUCH] **上拉缺失 / 线被顶死**：SCL=%s SDA=%s —— "
                  "具体是哪一种看上面 line_has_pullup 那条错误："
                  "「拉不下去」= 被硬顶到 3V3（0Ω/焊桥，主设备做不出 START）；"
                  "「拉下去回不来」= 真缺上拉（补一颗 4.7k 到 3V3）",
                  pu_scl ? "OK" : "异常", pu_sda ? "OK" : "异常");
        } else {
            LOG_I("[TOUCH] 两条线都有上拉（拉低后能自己回高）");
        }
    }

    /* ② **扫之前先把 I²C 外设复位一遍** —— 否则扫描是白扫的：
     *    进本函数前 `bsp_touch_init()` 已试读一次且失败了，那次失败会让外设停在
     *    BUSY 态；此后每次 `HAL_I2C_IsDeviceReady` 都会**立刻**返回 `HAL_BUSY`，
     *    **根本没上总线**，错误码全是 TIMEOUT，看着像"总线时序没形成"。
     *    （2026-10-02 实测踩到：首轮诊断报 HAL=2/ErrorCode=0x20，不可信。）
     *    这一步同时负责把上面改过的引脚**恢复成 AF 开漏**。 */
    (void)HAL_I2C_DeInit(&hi2c2);

    /* ②b **总线恢复**必须在 DeInit 之后：外设超时后会把 SDA/SCL **按在低电平**，
     *     此时（DeInit 前）去做恢复，读到的是"两条线都是 0、脉冲 0/9"，会被误判成
     *     "外部硬件钉死"。DeInit 后引脚交还给 GPIO，恢复才真的作用在总线上。
     *     （2026-10-13 实测：DeInit 前 0/9 且 SCL=0 SDA=0；用调试器停 CPU 后同两条线
     *       却能被拉低/能回高 ⇒ 就是外设自己按住的。）
     *     从机若在传输中途卡住会一直按住 SDA ⇒ 后面扫地址全是白扫，这 9 个时钟是解药。 */
    touch_bus_recover();

    (void)MX_I2C2_Init();

    /* ③ 再看两条线的**空闲电平** —— 只能证伪不能证实：读到 0 说明线被拉死（有价值）；
     *    读到 1 **不能**推出"有上拉"（悬空脚也读 1）—— 判上拉看上面的①。 */
    {
        const bsp_gpio_t scl = {(bsp_gpio_port_t)BOARD_TOUCH_I2C_SCL_PORT,
                                (uint8_t)BOARD_TOUCH_I2C_SCL_PIN};
        const bsp_gpio_t sda = {(bsp_gpio_port_t)BOARD_TOUCH_I2C_SDA_PORT,
                                (uint8_t)BOARD_TOUCH_I2C_SDA_PIN};
        const int l_scl = bsp_gpio_read(scl);
        const int l_sda = bsp_gpio_read(sda);
        if (l_scl != 1 || l_sda != 1) {
            LOG_E("[TOUCH] I2C2 空闲电平不对：SCL=%d SDA=%d（都应为 1）—— "
                  "被拉死 / 短路到地 / 两根接反", l_scl, l_sda);
        } else {
            /* ⚠️ **电平高不能证明有上拉** —— 悬空脚在 STM32 输入上也常读 1。 */
            LOG_I("[TOUCH] I2C2 两条线都没被拉死（SCL=1 SDA=1）—— "
                  "但电平高≠有上拉（悬空脚也会读 1）；判上拉要「短接 GND 后松手看能否回高」");
        }
    }

    /* ② 再逐地址扫。同时记下**第一次失败的原因** —— 这是比"扫不到"更细的一层：
     *    `HAL_ERROR` + ErrorCode 的 `AF`(0x04) ⇒ 地址阶段被 NACK
     *        ⇒ 总线是好的，是**器件没应答**（没供电 / 被复位按住 / 地址不对）；
     *    `BERR`(0x01) / `TIMEOUT`(0x20) ⇒ 总线上根本没形成合法时序
     *        ⇒ **信号完整性**（上拉太弱 + 线太长 ⇒ 上升沿太慢）或线被拉死。
     *    2026-10-02：用户实测 `SCK`→3V3 = **8.4k**（有上拉但偏弱），
     *    台面杜邦线又长 ⇒ 这两条要分开看，不能都归到"缺上拉"。 */
    HAL_StatusTypeDef first_err = HAL_OK;
    uint32_t first_err_code = 0U;

    for (uint8_t a = 0x08U; a < 0x78U; a++) {
        const HAL_StatusTypeDef st =
            HAL_I2C_IsDeviceReady(&hi2c2, (uint16_t)((uint16_t)a << 1), 2U, 5U);
        if (st != HAL_OK) {
            if (first_err == HAL_OK) {
                first_err = st;
                first_err_code = hi2c2.ErrorCode;
            }
            continue;
        }
        const char *note = "";
        if (a == BOARD_TOUCH_I2C_ADDR) {
            note = " ← 就是它（FT6336）";
        } else if (a == 0x5DU || a == 0x14U) {
            note = " ← GT911 系：地址与寄存器宽度都和本工程假设不同";
        }
        LOG_W("[TOUCH] I2C2 应答地址 0x%02X%s", (unsigned)a, note);
        found++;
    }

    if (found == 0U) {
        /* ★ 先把"失败在哪个阶段"说清楚 —— 这决定下一步查硬件还是查信号质量。
         *   HAL 状态码：0=OK 1=ERROR 2=BUSY 3=TIMEOUT；
         *   ErrorCode：0x01=BERR 0x02=ARLO 0x04=AF(NACK) 0x08=OVR 0x20=TIMEOUT。 */
        LOG_W("[TOUCH] 首次失败：HAL=%d ErrorCode=0x%08lX", (int)first_err,
              (unsigned long)first_err_code);
        if ((first_err_code & HAL_I2C_ERROR_AF) != 0U) {
            LOG_W("[TOUCH] ↑ 地址阶段被 **NACK** ⇒ 总线时序是合法的，是**器件没应答**："
                  "查触摸 FPC 是否插到位 / 触摸 IC 供电（FPC 第 1 脚 VCC）/ RST（第 2 脚）"
                  "是否被拉低");
        } else if ((first_err_code & (HAL_I2C_ERROR_BERR | HAL_I2C_ERROR_TIMEOUT)) != 0U) {
            LOG_W("[TOUCH] ↑ **总线时序没形成**（BERR/TIMEOUT）⇒ 信号完整性："
                  "上拉太弱（实测 SCK→3V3 仅 8.4k）+ 杜邦线太长 ⇒ 上升沿太慢。"
                  "先并一颗 4.7k 把上拉压到 ~3k 再试");
        }
        /* ⚠️ 这里**不能**断言"上拉已证明存在"。那个有源测试（拉低后松手看能否自己回高）
         *   在**另一个函数**里（`bsp_touch_init()` 的 `line_has_pullup`）—— 本函数
         *   看不到它的结论，写死一句"已证明存在"就是**假设冒充结论**。
         *   2026-10-09 实测踩到：单核心板（什么都没接）时那条测试报的是
         *   「**上拉缺失**」，而这里照样打「上拉已证明存在」—— 两句自相矛盾，
         *   而且后一句把人往"插反 / 供电"上引，**真因是缺上拉**。
         *   ⇒ 改成让读者**回去看上面那条**，不替它下结论。 */
        LOG_E("[TOUCH] I2C2 上**没有任何器件应答**。按这个顺序查："
              "① **先看上面有没有报「上拉缺失」** —— 报了就先补一颗 4.7k 到 3V3"
              "（台面/没插触摸模组时必然报这条，别跳过它去查接线）；"
              "② 模块 `SCK`(第 24 脚) / `MOSI`(第 26 脚) 两根**是否插反** —— 对调重试；"
              "③ 触摸 FPC 是否插到位 / 触摸 IC 是否供电（FPC 第 1 脚 `VCC`）；"
              "④ 触摸 `RST`（FPC 第 2 脚）是否被拉低");
    } else {
        LOG_W("[TOUCH] I2C2 共 %u 个器件应答（期望 0x%02X）", found,
              (unsigned)BOARD_TOUCH_I2C_ADDR);
    }
}

static int ft6336_read(uint8_t reg, uint8_t *buf, uint16_t len) {
    return (HAL_I2C_Mem_Read(&hi2c2, FT6336_HAL_ADDR, reg, I2C_MEMADD_SIZE_8BIT, buf, len,
                             FT6336_I2C_TIMEOUT_MS) == HAL_OK)
               ? 0
               : -1;
}

/* 读一次；失败**立刻重试一次**。
 * why 值：板上实测读失败率 ≈45%（写屏时 FSMC 噪声打断 I2C），而失败大多是
 * 一次性的（BERR/超时）—— 重试一次就能救回大半；超时已收到 5ms ⇒ 代价可忽略。 */
static int ft6336_read_retry(uint8_t reg, uint8_t *buf, uint16_t len) {
    if (ft6336_read(reg, buf, len) == 0) {
        return 0;
    }
    return ft6336_read(reg, buf, len);
}

/* ★ 运行期总线解锁（2026-10-13 四轮）：连续读失败后执行的"把从机敲醒"动作。
 *   标准做法 = 主机只放 SCL 送 **9 个时钟**（把从机残留的一位/半截传输走完），
 *   再补一个 **STOP**；随后把 I2C 外设重新初始化回 AF 开漏。
 *   ⚠️ 与开机版 `touch_bus_recover()` 的差别（两条都是必要的）：
 *     · 它跑在**运行中**（LVGL 的 indev 回调里）⇒ 只能干"解锁"这一件事，
 *       绝不扫地址（扫 112 个地址要 0.6s，运行期不能这么干）；
 *     · 它**自己**把外设恢复回 AF（开机版靠调用者后面跟一句 MX_I2C2_Init）；
 *     · 解锁前后各打一行日志 —— "总线到底锁没锁、锁没锁开"要能从日志里看出来。 */
static void touch_unstick(void) {
    const bsp_gpio_t scl = {(bsp_gpio_port_t)BOARD_TOUCH_I2C_SCL_PORT,
                            (uint8_t)BOARD_TOUCH_I2C_SCL_PIN};
    const bsp_gpio_t sda = {(bsp_gpio_port_t)BOARD_TOUCH_I2C_SDA_PORT,
                            (uint8_t)BOARD_TOUCH_I2C_SDA_PIN};

    LOG_W("[TOUCH] I2C 连续读失败 %u 次 —— 总线解锁（9 脉冲 + STOP + 重初始化 I2C2）",
          (unsigned)FT6336_UNSTICK_AFTER);

    /* DeInit 必须在改引脚之前：外设超时后会把 SDA/SCL **按在低电平**，
     * 此时（DeInit 前）去脉冲，读到的是"两条线都是 0、脉冲 0/9"，会被误判成
     * "外部硬件钉死"（2026-10-13 实测踩到过，见 touch_scan_bus 的注释）。 */
    (void)HAL_I2C_DeInit(&hi2c2);

    bsp_gpio_config(scl, BSP_GPIO_OUT_OD, 0U);
    bsp_gpio_config(sda, BSP_GPIO_OUT_OD, 0U);
    bsp_gpio_write(scl, 1); /* 释放两条线 */
    bsp_gpio_write(sda, 1);
    for (volatile int i = 0; i < 2000; i++) {
    }

    unsigned pulses = 0U;
    int sda_free = 0;

    /* 最多 **3 轮 × 9 个时钟**，每轮末尾补一个 STOP。
     * why 不止 9 个：半截传输越深，从机需要的时钟越多 —— 板上实测出现过
     * `脉冲 9/9 · SDA 现在 = 0`（9 个敲不开）⇒ 放大到 27 个时钟的余量；
     * 一旦读到 SDA 已放线就立刻收工（正常情况第 1 个时钟就出来了）。 */
    for (unsigned round = 0U; round < 3U; round++) {
        for (unsigned i = 0U; i < 9U; i++) {
            bsp_gpio_write(scl, 0);
            for (volatile int k = 0; k < 200; k++) {
            }
            bsp_gpio_write(scl, 1);
            for (volatile int k = 0; k < 200; k++) {
            }
            pulses++;
        }
        /* STOP：SCL 高时 SDA 由低 → 高（有些从机认 STOP 才放线）*/
        bsp_gpio_write(sda, 0);
        for (volatile int k = 0; k < 200; k++) {
        }
        bsp_gpio_write(scl, 1);
        for (volatile int k = 0; k < 200; k++) {
        }
        bsp_gpio_write(sda, 1);
        for (volatile int k = 0; k < 200; k++) {
        }
        sda_free = (bsp_gpio_read(sda) == 1) ? 1 : 0;
        if (sda_free != 0) {
            break;
        }
    }

    (void)MX_I2C2_Init(); /* 引脚交回 AF 开漏（否则 I2C 永远起不来）*/

    LOG_W("[TOUCH] 解锁完成：脉冲 %u/27 · SDA 现在 = %d（1 = 从机已放线）", pulses, sda_free);
}

/* 读失败的统一出口：退避 + 连续失败到阈值就解锁一次（返回 -1 = "没按/读不到"）*/
static int touch_fail(uint32_t now) {
    s_retry_until = now + FT6336_BACKOFF_MS;
    s_fail_run++;
    if (s_fail_run >= FT6336_UNSTICK_AFTER &&
        (uint32_t)(now - s_unstick_ms) >= FT6336_UNSTICK_GAP_MS) {
        s_fail_run = 0U;
        s_unstick_ms = now;
        touch_unstick();
    }
    return -1;
}

/* ★ 2026-10-14 八轮：**开机 32 次寄存器快照（`touch_reg_dump16`/`touch_dump_once`）删了**。
 *   它当时的作用（2026-10-13 四轮）是把"从机没在驱动数据"和"从机活着但内容不对"
 *   分开 —— 判据是"整片全 0xFF ⇒ 物理层"vs"有结构 ⇒ 病在寄存器/模式"。
 *   这个判据已经用掉（结论落在硬件侧的排查记录里），而它每次开机要跑 32 次 I2C 读、
 *   还要在 `bsp_touch_read()` 的首读失败路径上再挂一次 ⇒ 留着只有"开机多 32 次 I2C"
 *   这一个副作用。要复现同类诊断：临时加 10 行读寄存器循环即可。 */

/* ★ 降速：400k → 100k（2026-10-13 四轮）。I2C 是**边沿触发**的：杜邦线太长时
 *   上升沿根本来不及（实测上拉只 8.4k），时钟越快越容易在"数据还没到"时采样。
 *   触摸带宽需求只有 ≈2.5 kbit/s（60 次/s × 5 字节），降到 100k 对 UI 零影响，
 *   换来的是**时序余量 ×4**。⚠️ 只改 BSP 里的外设参数，不动 `.ioc`
 *   （引脚/时钟的唯一事实源，改它要经人 —— AGENTS.md）。 */
static void touch_i2c_tune(void) {
    static int done = 0;
    if (done != 0) {
        return;
    }
    done = 1;

    const uint32_t old = hi2c2.Init.ClockSpeed;

    /* ⚠️ 硬件数字滤波**在这颗芯片上不存在**，别再试：`stm32f407xx.h` 里
     *    `I2C_TypeDef` 到 `TRISE` 就结束了（没有 `FLTR`/`DNF`）—— 数字噪声滤波是
     *    F3/F0/L4 那一代才加的（试过 `hi2c2.Instance->FLTR`，编译直接报
     *    "'I2C_TypeDef' has no member named 'FLTR'"，2026-10-13 五轮实测）。
     *    ⇒ 抗写屏毛刺只能靠：**降速**（下面这条）+ 读失败重试 + 退避 + 200ms 读失败
     *      宽限窗（`bsp_touch_read` / `app_lvgl.c`）。 */

    /* 降速 400k → 100k：I2C 是**边沿触发**的，杜邦线太长时上升沿根本来不及
     * （实测上拉只 8.4k）⇒ 慢一档 = 时序余量 ×4。触摸只要 ≈2.5 kbit/s。 */
    hi2c2.Init.ClockSpeed = 100000U;

    if (hi2c2.Init.ClockSpeed == old) {
        return;
    }
    if (HAL_I2C_Init(&hi2c2) != HAL_OK) {
        hi2c2.Init.ClockSpeed = old;
        LOG_W("[TOUCH] I2C2 降速到 100k 失败，继续用 %lu", (unsigned long)old);
    } else {
        LOG_I("[TOUCH] I2C2 %lu → 100k（时序余量 ×4，抗写屏毛刺）", (unsigned long)old);
    }
}

/* ------------------------------------------------------------------ 对外 */
int bsp_touch_init(void) {
    uint8_t status = 0;

    touch_i2c_tune(); /* 先调优再探（DNF 滤波 + 降速 100k，见函数注释）*/

    /* 没有复位时序可做：IC 上电自启（屏厂例程不接 RST/INT，TP_RST 已由
     * .ioc 初始为高）。初始化 = 探测一次总线。 */
    if (ft6336_read(FT6336_REG_TD_STATUS, &status, 1) != 0) {
        LOG_W("[TOUCH] FT6336 应答失败（I2C 不通？地址不是 0x%02X？）",
              BOARD_TOUCH_I2C_ADDR);
        /* ★ 把**首次失败**的真实原因打出来 —— 这是全流程里唯一一次没被污染的读
         *   （后面扫描会先复位外设，但那是复位后重试，原因已经变了）。
         *   State：HAL_I2C_STATE_*；ErrorCode：0x01=BERR 0x02=ARLO 0x04=AF(NACK)
         *   0x08=OVR 0x20=TIMEOUT。判读见 touch_scan_bus() 里那段。 */
        LOG_W("[TOUCH] 首次读取失败：State=0x%02X ErrorCode=0x%08lX", (unsigned)hi2c2.State,
              (unsigned long)hi2c2.ErrorCode);
        return -1;
    }
    s_ready = 1;
    if ((status & 0x0FU) > 5U) {
        /* 首读非法：按「芯片空闲/monitor、不报点」处理。**不再尝试写寄存器唤醒**
         * （实测写无效，见文件头）——判"触摸能不能用"看按下去有没有
         * `[TOUCH] 按下 (x,y)`，不看这一条。 */
        LOG_W("[TOUCH] FT6336 在线但首读 TD_STATUS=%02X **非法**（低 4 位 = %u > 5）——"
              " 按「芯片未报点」处理（写 DEVICE_MODE 实测无效，已删）",
              status, (unsigned)(status & 0x0FU));
    } else {
        LOG_I("[TOUCH] FT6336 在线 @0x%02X（TD_STATUS=%02X）", BOARD_TOUCH_I2C_ADDR, status);
    }
    return 0;
}

int bsp_touch_self_check(void) {
    uint8_t status = 0;

    /* ★ 2026-10-13 四轮：判据从"读得到"升级成"**读得对**"。
     *   板上实测：开机读到 TD_STATUS=FF（低 4 位 = 15，而 FT6336 最多 5 点）——
     *   那是总线上的垃圾，而旧的判据（I2C 通就算过）会把"触摸其实不可用"判成
     *   通过（自检表上 TOUCH 亮 PASS，人在屏前却点不动）。
     *   新判据：连读 3 次，只要有一次低 4 位 ≤5 就算通过；三次都越界 ⇒ FAIL。 */
    for (int i = 0; i < 3; i++) {
        if (ft6336_read_retry(FT6336_REG_TD_STATUS, &status, 1) == 0 &&
            (status & 0x0FU) <= 5U) {
            LOG_I("[SELFTEST] FT6336 ok, TD_STATUS=%02X（低 4 位 = 触点数）", status);
            return 0;
        }
    }
    /* 三次都非法 ⇒ 再读一次定"是读不到还是读到垃圾"（八轮前这里还会先写一次
     * Working 唤醒 —— 那是实测无效的动作，已删，见文件头）。 */
    if (ft6336_read_retry(FT6336_REG_TD_STATUS, &status, 1) != 0) {
        LOG_W("[SELFTEST] FT6336 读不到 TD_STATUS（I2C2 不通 / 地址不对 / 没上拉）");
        touch_scan_bus(); /* ★ 把"读不到"变成"到底哪一步不对" */
        return -1;
    }
    LOG_W("[SELFTEST] FT6336 TD_STATUS=%02X **非法**（低 4 位 = %u > 5）—— "
          "总线/上拉可疑，触摸数据不可信",
          status, (unsigned)(status & 0x0FU));
    /* ★ 2026-10-13 四轮：**这里也要跑总线排查**。
     *   why：`touch_scan_bus()` 原来只在"完全读不到"时才跑 —— 而本板的实况是
     *   **"能通但数据是垃圾"**（TD_STATUS=FF，低 4 位 = 15），于是那套最有价值的
     *   结论（**有源判上拉**：拉低后松手看能否自己回高；以及全地址扫描：到底哪个
     *   地址有应答）从来没被打出来过，人只能猜"是不是上拉"。
     *   代价：开机多 ≈0.6s（扫描窗口期），此时界面还没开始刷，可接受。 */
    touch_scan_bus();
    return -1;
}

/* ---- 轴映射开关（默认取 board_config.h；可由 UI 现场翻，见 bsp_touch.h）---- */
static int s_swap_xy = (BOARD_TOUCH_SWAP_XY) ? 1 : 0;
static int s_inv_x = (BOARD_TOUCH_INVERT_X) ? 1 : 0;
static int s_inv_y = (BOARD_TOUCH_INVERT_Y) ? 1 : 0;

void bsp_touch_set_mapping(int swap_xy, int inv_x, int inv_y) {
    s_swap_xy = swap_xy ? 1 : 0;
    s_inv_x = inv_x ? 1 : 0;
    s_inv_y = inv_y ? 1 : 0;
}

void bsp_touch_get_mapping(int *swap_xy, int *inv_x, int *inv_y) {
    if (swap_xy != NULL) {
        *swap_xy = s_swap_xy;
    }
    if (inv_x != NULL) {
        *inv_x = s_inv_x;
    }
    if (inv_y != NULL) {
        *inv_y = s_inv_y;
    }
}

int bsp_touch_read(bsp_touch_point_t *pt) {
    uint8_t status = 0;
    /* ★ 2026-10-14：改成**一次事务读全**（0x02..0x06）⇒ `st[0]=TD_STATUS`、`st[1..4]=点 1`。
     *   原先要读两次（0x02 1 字节 + 0x03 4 字节）⇒ 每次调用两个"5ms 超时"失败窗口。
     *   理由与判据见下面第一次读处的注释。 */
    uint8_t st[5] = {0};
    const uint32_t now = bsp_tick_ms();

    if (pt == NULL) {
        return -1;
    }
    if (!s_ready) {
        /* ★ 每秒重试一次初始化（2026-10-13 四轮）：**开机没探测到 ≠ 永远没戏**。
         *   台面上把触摸线插好、换一颗模块、或给模块断电重上电，这里就能把它
         *   "捡回来"，不必重启整机。配套改动：`app_lvgl_init()` 现在**无论如何
         *   都建 indev**（旧写法是探测失败就不建 ⇒ 连这个重试的机会都没有）。 */
        if ((uint32_t)(now - s_init_try_ms) >= 1000U) {
            s_init_try_ms = now;
            if (bsp_touch_init() == 0) {
                LOG_I("[TOUCH] 触摸回来了（运行期重试初始化成功）");
            }
        }
        return -1;
    }

    /* ---- 总线退避（见 FT6336_BACKOFF_MS）：卡住时不拿 UI 的时间反复去撞 ---- */
    if (s_retry_until != 0U && (int32_t)(now - s_retry_until) < 0) {
        return -1;
    }

    /* ★★ 2026-10-14：**把两次读合并成一次**（0x02 起连读 5 字节 = TD_STATUS + 点 1 四字节）。
     *   why 值：实测手指按住时**一次 `bsp_touch_read` 要 6.28 ms**（`[TOUCH] 1s` 的
     *   `I2C 读均` 探针），×100Hz ≈ **63% CPU** —— 这是"拖动时卡"的**最大单一来源**，
     *   比渲染还大。而 100kHz 读 5 字节的**线时长只有 ~0.45 ms** ⇒ 那 6 ms 几乎全是
     *   **失败后干等超时**：每次调用有两个事务 ⇒ 两个"5ms 超时"窗口
     *   （`ft6336_read_retry` 的说明写着"失败率 ≈45%，超时 5ms ⇒ 代价可忽略" ——
     *    **实测证明"可忽略"这三个字是错的**）。
     *   FT6336 的寄存器地址**自增**（原来那次 4 字节读就已经依赖这一点）⇒ 合并成一次读
     *   在电气与协议上**完全等价**，只是**少一个失败窗口 + 少一次 HAL 起停**。
     * ⚠️ 判据：坐标仍跟手、`失败` 不升、`I2C 读均` 明显下降（预期 ~6.28 → ~3.2 ms）。
     * ⚠️ 下一步（本轮未做，留作可归因的另一刀）：`FT6336_I2C_TIMEOUT_MS` 5→2。 */
    if (ft6336_read_retry(FT6336_REG_TD_STATUS, st, 5) != 0) {
        return touch_fail(now);
    }
    status = st[0];
    s_retry_until = 0U; /* 总线通了：清退避 */
    s_fail_run = 0U;    /* 也清"连续失败"计数 */

    /* 低 4 位 = 触点数。FT6336 读走即刷新，无"清标志"写操作（屏厂例程只读不写）。
     * ★ 合法值只有 **0..5**（FT6336 最多 5 点）；读到 6..15 = 总线上的垃圾
     *   （板上实测开机读到 0xFF ⇒ 15）⇒ 当"没按"处理。若把它当按下，LVGL 会拿
     *   这个假手指持续拖 tileview（每帧全屏重绘 + 每次跨页触发重测）。 */
    const uint8_t npts = (uint8_t)(status & 0x0FU);
    if (npts == 0U) {
        return 0; /* 合法的"零触点"：没按（八轮前这里会写 Working 保活，已删）*/
    }
    if (npts > 5U) {
        touch_bad_log(); /* 非法读数（总线垃圾）：计数 + 30s 汇总，**不当成按下** */
        return 0;
    }

    {
        /* ★ 2026-10-14：点 1 的 4 字节**已经在上面的合并读里拿到了**（`st[1..4]`），
         *   这里不再单独发一次事务 —— 少掉的正是那一整个"5ms 超时"失败窗口。 */
        /* 0x03/0x04 = 第一轴，0x05/0x06 = 第二轴（12 位，高位在低半字节）。
         * ⚠️ 轴向/镜像**不写死**：由 board_config.h 的三个宏决定，且可用
         *    `bsp_touch_set_mapping()` 在 UI 的"TOUCH"页现场翻 —— 点四角看准星，
         *    凑对了再把组合写回 board_config.h（页面会显示当前组合）。
         *    背景：屏厂例程的原始轴序是**面板竖屏**（第一轴=短边），而本工程显示是
         *    **横屏 90°**（app_lvgl.c 的 lv_display_set_rotation）⇒ 通常需要 swap。
         * ⚠️ 若还发现镜像/颠倒，只改这里或那三个宏，**不要去动 bsp_lcd / MADCTL**。 */
        const uint32_t a_raw = (uint32_t)((st[1] & 0x0FU) << 8) | st[2];
        const uint32_t b_raw = (uint32_t)((st[3] & 0x0FU) << 8) | st[4];

        /* 两轴都是 0 = 假点（真实手指不可能**精确**落在 (0,0)）—— 同 TD_STATUS
         * 越界，是总线垃圾的另一种长相，按"没按"处理（板上实测栽过一次）。 */
        if (a_raw == 0U && b_raw == 0U) {
            touch_bad_log();
            return 0;
        }

        uint32_t x = s_swap_xy ? b_raw : a_raw;
        uint32_t y = s_swap_xy ? a_raw : b_raw;
        if (s_inv_x) {
            x = (x < BOARD_TOUCH_WIDTH) ? (BOARD_TOUCH_WIDTH - 1U - x) : 0U;
        }
        if (s_inv_y) {
            y = (y < BOARD_TOUCH_HEIGHT) ? (BOARD_TOUCH_HEIGHT - 1U - y) : 0U;
        }

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
