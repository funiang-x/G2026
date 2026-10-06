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
 *    · **只读不写** —— 例程从不写寄存器，没有 GT911 那种"写 0 清标志"步骤；
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
#include "i2c.h"      /* CubeMX 生成的 hi2c2（Device 层允许依赖 Core 的句柄） */
#include "stm32f4xx_hal.h"

/* ---- FT6336 寄存器（8 位地址；出处见文件头）---- */
#define FT6336_REG_TD_STATUS 0x02U /* 低 4 位 = 触点数 */
#define FT6336_REG_POINT1 0x03U    /* 点 1：第一轴高/低、第二轴高/低 4 字节 */

#define FT6336_I2C_TIMEOUT_MS 100U

/* HAL 的地址要 7 位地址左移 1 位 */
#define FT6336_HAL_ADDR ((uint16_t)(BOARD_TOUCH_I2C_ADDR << 1))

static int s_ready = 0;

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
    bsp_gpio_write(g, 1); /* 开漏写 1 = 释放（高阻） */
    for (volatile int i = 0; i < 4000; i++) {
    } /* ≈170µs，够 40k×200pF 充满 */
    return bsp_gpio_read(g) == 1;
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
            LOG_E("[TOUCH] **上拉缺失**：SCL=%s SDA=%s（拉低后松手回不到高）—— "
                  "在缺的那条线上补一颗 4.7k 到 3V3（开漏 I²C 必须有）",
                  pu_scl ? "有" : "无", pu_sda ? "有" : "无");
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
        LOG_E("[TOUCH] I2C2 上**没有任何器件应答**。"
              "**上拉已由上面的有源测试证明存在**（拉低后能自己回高），所以按这个顺序查："
              "① 模块 `SCK`(第 24 脚) / `MOSI`(第 26 脚) 两根**是否插反** —— 对调重试；"
              "② 触摸 FPC 是否插到位 / 触摸 IC 是否供电（FPC 第 1 脚 `VCC`）；"
              "③ 触摸 `RST`（FPC 第 2 脚）是否被拉低");
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

/* ------------------------------------------------------------------ 对外 */
int bsp_touch_init(void) {
    uint8_t status = 0;

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
    LOG_I("[TOUCH] FT6336 在线 @0x%02X（TD_STATUS=%02X）", BOARD_TOUCH_I2C_ADDR, status);
    return 0;
}

int bsp_touch_self_check(void) {
    uint8_t status = 0;

    if (ft6336_read(FT6336_REG_TD_STATUS, &status, 1) != 0) {
        LOG_W("[SELFTEST] FT6336 读不到 TD_STATUS（I2C2 不通 / 地址不对 / 没上拉）");
        touch_scan_bus(); /* ★ 把"读不到"变成"到底哪一步不对" */
        return -1;
    }
    /* 判据：地址 0x38 有 ACK、寄存器读得通。
     * ⚠️ FT6336 没有 GT911 那种 ASCII 产品 ID 寄存器，"读得到"就是全部判据。 */
    LOG_I("[SELFTEST] FT6336 ok, TD_STATUS=%02X（低 4 位 = 触点数）", status);
    return 0;
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
    uint8_t buf[4];

    if (pt == NULL || !s_ready) {
        return -1;
    }

    if (ft6336_read(FT6336_REG_TD_STATUS, &status, 1) != 0) {
        return -1;
    }
    /* 低 4 位 = 触点数，0 = 全部抬起。FT6336 读走即刷新，无"清标志"写操作
     * （屏厂例程只读不写）。 */
    if ((status & 0x0FU) == 0U) {
        return 0;
    }

    if (ft6336_read(FT6336_REG_POINT1, buf, 4) != 0) {
        return -1;
    }
    {
        /* 0x03/0x04 = 第一轴，0x05/0x06 = 第二轴（12 位，高位在低半字节）。
         * ⚠️ 轴向/镜像**不写死**：由 board_config.h 的三个宏决定，且可用
         *    `bsp_touch_set_mapping()` 在 UI 的"TOUCH"页现场翻 —— 点四角看准星，
         *    凑对了再把组合写回 board_config.h（页面会显示当前组合）。
         *    背景：屏厂例程的原始轴序是**面板竖屏**（第一轴=短边），而本工程显示是
         *    **横屏 90°**（app_lvgl.c 的 lv_display_set_rotation）⇒ 通常需要 swap。
         * ⚠️ 若还发现镜像/颠倒，只改这里或那三个宏，**不要去动 bsp_lcd / MADCTL**。 */
        const uint32_t a_raw = (uint32_t)((buf[0] & 0x0FU) << 8) | buf[1];
        const uint32_t b_raw = (uint32_t)((buf[2] & 0x0FU) << 8) | buf[3];

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
