/* bsp_spi —— SPI1 + 板载 W25Q128（16MB SPI Flash）的实现。
 *
 * 引脚 / 时钟 / 复用**不在本文件**：那些由 CubeMX 生成的
 * `Core/Src/spi.c` 的 `HAL_SPI_MspInit` 配（docs/10 §B 第 5 条 —— 两边都写
 * 会 multiple definition）。本文件只做"命令层 + 片选时序"。
 *
 * 参考：资料包 `第03章/代码例程/014硬件SPI(flash).zip`（官方**寄存器版**），
 *      逻辑一致，这里改写成 HAL 并补了超时与错误返回。 */
#include "bsp_spi.h"

#include "board_config.h"
#include "bsp_gpio.h"
#include "log.h"
#include "spi.h" /* hspi1 —— CubeMX 生成 */
#include "stm32f4xx_hal.h"

/* ---- W25Q128 指令（数据手册 §8.1 指令表）----
 * ⚠️ 读 ID 用 **0x9F（JEDEC ID）**，不是官方例程的 0x90：
 *      0x90 → 制造商 + **设备 ID**（W25Q128 得 0xEF17）
 *      0x9F → 制造商 + 存储类型 + **容量码**（得 0xEF / 0x40 / 0x18）
 *    两个命令都对，但 0x9F 能判出容量，换 Flash 型号时判据更硬。 */
#define CMD_WRITE_ENABLE 0x06U
#define CMD_READ_STATUS1 0x05U
#define CMD_READ_DATA 0x03U
#define CMD_PAGE_PROGRAM 0x02U
#define CMD_SECTOR_ERASE 0x20U
#define CMD_CHIP_ERASE 0xC7U
#define CMD_JEDEC_ID 0x9FU

#define SR1_BUSY 0x01U /* 状态寄存器 1 的 bit0 = 忙 */

/* 超时（ms）。SPI 单字节本身是微秒级，这些值只为防"硬件坏了导致死等"。 */
#define XFER_TIMEOUT_MS 100U
#define BUSY_TIMEOUT_MS 5000U        /* 单扇区擦除典型 45ms，给足余量 */
#define CHIP_ERASE_TIMEOUT_MS 90000U /* 整片擦除典型 ~40s */

/* 软件片选 PA4。端口/引脚号取自 board_config.h（唯一来源，本层不写魔数）。 */
static const bsp_gpio_t s_cs = {(bsp_gpio_port_t)BOARD_SPI_FLASH_CS_PORT,
                                BOARD_SPI_FLASH_CS_PIN};

static inline void cs_low(void) { bsp_gpio_write(s_cs, 0); }
static inline void cs_high(void) { bsp_gpio_write(s_cs, 1); }

/* 全双工收发一个字节（发 0xFF 即"只读"）。SPI 是全双工，收发同时发生。 */
static uint8_t spi_xfer(uint8_t tx) {
    uint8_t rx = 0U;
    (void)HAL_SPI_TransmitReceive(&hspi1, &tx, &rx, 1U, XFER_TIMEOUT_MS);
    return rx;
}

/* 等 BUSY 位清零。返回 0 = 空闲，-1 = 超时。 */
static int flash_wait_busy(uint32_t timeout_ms) {
    uint32_t t0 = HAL_GetTick();
    for (;;) {
        cs_low();
        spi_xfer(CMD_READ_STATUS1);
        uint8_t sr = spi_xfer(0xFFU);
        cs_high();
        if ((sr & SR1_BUSY) == 0U) {
            return 0;
        }
        if ((HAL_GetTick() - t0) >= timeout_ms) {
            return -1;
        }
    }
}

static void write_enable(void) {
    cs_low();
    spi_xfer(CMD_WRITE_ENABLE);
    cs_high();
}

uint16_t bsp_spi_flash_read_id(void) {
    cs_low();
    spi_xfer(CMD_JEDEC_ID);
    uint8_t mfr = spi_xfer(0xFFU); /* 制造商：0xEF = Winbond */
    (void)spi_xfer(0xFFU);         /* 存储类型：0x40 */
    uint8_t cap = spi_xfer(0xFFU); /* 容量码：0x18 = 128Mbit */
    cs_high();
    return (uint16_t)(((uint16_t)mfr << 8) | cap);
}

int bsp_spi_flash_init(void) {
    cs_high(); /* 先确保不选中（CubeMX 已把 PA4 初值设为高，这里是双保险）*/
    uint16_t id = bsp_spi_flash_read_id();
    if (id != BSP_SPI_FLASH_JEDEC_ID_EXPECTED) {
        LOG_E("W25Q128 ID 不符：读到 0x%04X，期望 0x%04X —— 查接线 / SPI 模式 / 型号",
              id, BSP_SPI_FLASH_JEDEC_ID_EXPECTED);
        return -1;
    }
    LOG_I("W25Q128 就绪（JEDEC ID=0x%04X，16MB）", id);
    return 0;
}

int bsp_spi_flash_read(uint32_t addr, uint8_t *buf, uint32_t len) {
    if (buf == NULL || len == 0U) {
        return -1;
    }
    if (addr + len > BSP_SPI_FLASH_SIZE_BYTES) {
        LOG_E("读越界：addr=%lu len=%lu（容量 %lu）", (unsigned long)addr,
              (unsigned long)len, (unsigned long)BSP_SPI_FLASH_SIZE_BYTES);
        return -1;
    }
    cs_low();
    spi_xfer(CMD_READ_DATA);
    spi_xfer((uint8_t)(addr >> 16));
    spi_xfer((uint8_t)(addr >> 8));
    spi_xfer((uint8_t)addr);
    for (uint32_t i = 0U; i < len; i++) {
        buf[i] = spi_xfer(0xFFU);
    }
    cs_high();
    return 0;
}

int bsp_spi_flash_write_page(uint32_t addr, const uint8_t *buf, uint32_t len) {
    if (buf == NULL || len == 0U) {
        return -1;
    }
    if (addr + len > BSP_SPI_FLASH_SIZE_BYTES) {
        LOG_E("写越界：addr=%lu len=%lu", (unsigned long)addr, (unsigned long)len);
        return -1;
    }

    /* 拒绝跨页。页写命令（0x02）越过页边界后会**回卷到本页开头**、
     * 静默覆盖前面的数据 —— 硬件不报错，所以必须在这里拦。
     * 拆分由组合根用 op_flash_split_pages() 做（见 Task/Src/app.c）。 */
    uint32_t left = BSP_SPI_FLASH_PAGE_SIZE - (addr % BSP_SPI_FLASH_PAGE_SIZE);
    if (len > left) {
        LOG_E("单页写跨页：addr=0x%06lX len=%lu，本页只剩 %lu 字节"
              "（调用方应先用 op_flash_split_pages 拆分）",
              (unsigned long)addr, (unsigned long)len, (unsigned long)left);
        return -1;
    }

    write_enable();
    cs_low();
    spi_xfer(CMD_PAGE_PROGRAM);
    spi_xfer((uint8_t)(addr >> 16));
    spi_xfer((uint8_t)(addr >> 8));
    spi_xfer((uint8_t)addr);
    for (uint32_t i = 0U; i < len; i++) {
        spi_xfer(buf[i]);
    }
    cs_high();

    if (flash_wait_busy(BUSY_TIMEOUT_MS) != 0) {
        LOG_E("页写超时 @0x%06lX", (unsigned long)addr);
        return -1;
    }
    return 0;
}

int bsp_spi_flash_erase_sector(uint32_t addr) {
    uint32_t aligned = addr & ~(BSP_SPI_FLASH_SECTOR_SIZE - 1U); /* 向下对齐 */
    write_enable();
    cs_low();
    spi_xfer(CMD_SECTOR_ERASE);
    spi_xfer((uint8_t)(aligned >> 16));
    spi_xfer((uint8_t)(aligned >> 8));
    spi_xfer((uint8_t)aligned);
    cs_high();
    if (flash_wait_busy(BUSY_TIMEOUT_MS) != 0) {
        LOG_E("扇区擦除超时 @0x%06lX", (unsigned long)aligned);
        return -1;
    }
    return 0;
}

int bsp_spi_flash_erase_chip(void) {
    write_enable();
    cs_low();
    spi_xfer(CMD_CHIP_ERASE);
    cs_high();
    if (flash_wait_busy(CHIP_ERASE_TIMEOUT_MS) != 0) {
        LOG_E("整片擦除超时");
        return -1;
    }
    return 0;
}
