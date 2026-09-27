/* bsp_sd —— 板载 TF 卡座（SDIO 4-bit）的实现。
 *
 * ⚠️ 本文件**不写** `HAL_SD_MspInit` —— 引脚 / 时钟 / 复用由 CubeMX 生成的
 *    `Core/Src/sdio.c` 配（docs/10 §B 第 5 条：两边都定义会 multiple definition）。
 *
 * ⚠️ **已知缺陷（2026-09-23 记录，未修）**：
 *    `MX_SDIO_SD_Init()`（在 main.c 里上电就调）中 `HAL_SD_Init()` 失败时会走
 *    `Error_Handler()` → `app_panic()` → **停机**。
 *    ⇒ **上电时没插 TF 卡，整块板子就不启动了**（只打一行 [AI_FAIL] 然后停）。
 *    对"模板工程"这是硬伤：插不插卡不该决定板子能不能跑。
 *
 *    修法：把 SDIO 从 `.ioc` 的 `ProjectManager.functionlistsort` 里去掉
 *    （CubeMX 仍会生成 `MX_SDIO_SD_Init()` 函数，只是不再自动调用），
 *    改由本文件的 `bsp_sd_init()` 在"检测到卡"之后再初始化。
 *    ⚠️ 那属于改 `.ioc`，按 docs/12 要先问人 —— 所以这里只记录，没动手。 */
#include "bsp_sd.h"

#include "board_config.h"
#include "bsp_gpio.h"
#include "log.h"
#include "sdio.h" /* hsd —— CubeMX 生成 */
#include "stm32f4xx_hal.h"

/* 卡检测 PD3：板上经 R20(47k) 上拉到 3V3，插卡接地 ⇒ 低电平。 */
static const bsp_gpio_t s_det = {(bsp_gpio_port_t)BOARD_SDIO_DET_PORT,
                                 BOARD_SDIO_DET_PIN};

/* SD 操作超时（ms）。读写单块通常几十微秒，这个值只为防死等。 */
#define SD_OP_TIMEOUT_MS 5000U

int bsp_sd_is_detected(void) {
    int level = bsp_gpio_read(s_det);
    if (level < 0) {
        return 0; /* 参数非法当"没插卡"，别让调用方拿 -1 当布尔用 */
    }
    return (level == 0) ? 1 : 0; /* 低电平 = 插着 */
}

int bsp_sd_init(void) {
    if (!bsp_sd_is_detected()) {
        LOG_W("TF 卡未插入（PD3 为高）—— 跳过 SDIO 初始化");
        return -1;
    }
    if (HAL_SD_Init(&hsd) != HAL_OK) {
        LOG_E("HAL_SD_Init 失败（卡不响应 / 接触不良 / 非 SD 卡）");
        return -1;
    }
    /* ⚠️ CubeMX 生成的 `MX_SDIO_SD_Init()` 里 `hsd.Init.BusWide` 是 **1-bit**，
     *    这是对的：SD 卡必须先 1-bit 识别，再切宽总线。这里补上切换。
     *    切换失败不致命（1-bit 也能读写，只是慢），所以只警告不返回错误。 */
    if (HAL_SD_ConfigWideBusOperation(&hsd, SDIO_BUS_WIDE_4B) != HAL_OK) {
        LOG_W("切 4-bit 总线失败 —— 降级 1-bit 继续（能读写，速度低）");
    }
    bsp_sd_info_t info;
    if (bsp_sd_get_info(&info) == 0) {
        LOG_I("TF 卡就绪：%lu KB（%lu 块 × %lu B）", (unsigned long)info.capacity_kb,
              (unsigned long)info.block_count, (unsigned long)info.block_size);
    }
    return 0;
}

int bsp_sd_get_info(bsp_sd_info_t *info) {
    if (info == NULL) {
        return -1;
    }
    HAL_SD_CardInfoTypeDef ci;
    if (HAL_SD_GetCardInfo(&hsd, &ci) != HAL_OK) {
        return -1;
    }
    info->block_count = ci.LogBlockNbr;
    info->block_size = ci.LogBlockSize;
    /* 用 64 位算再截断：16GB 卡的 块数×块大小 会溢出 uint32 */
    info->capacity_kb =
        (uint32_t)(((uint64_t)ci.LogBlockNbr * (uint64_t)ci.LogBlockSize) / 1024ULL);
    info->card_type = (uint8_t)ci.CardType; /* HAL 的位掩码原样带出 */
    return 0;
}

int bsp_sd_read_blocks(uint32_t block, uint8_t *buf, uint32_t count) {
    if (buf == NULL || count == 0U) {
        return -1;
    }
    if (HAL_SD_ReadBlocks(&hsd, buf, block, count, SD_OP_TIMEOUT_MS) != HAL_OK) {
        LOG_E("SD 读失败 @块 %lu × %lu", (unsigned long)block, (unsigned long)count);
        return -1;
    }
    /* 卡从 RX 回到 TRANSFER 才算真的完事 */
    return (HAL_SD_GetCardState(&hsd) == HAL_SD_CARD_TRANSFER) ? 0 : -1;
}

int bsp_sd_write_blocks(uint32_t block, const uint8_t *buf, uint32_t count) {
    if (buf == NULL || count == 0U) {
        return -1;
    }
    if (HAL_SD_WriteBlocks(&hsd, (uint8_t *)buf, block, count, SD_OP_TIMEOUT_MS) !=
        HAL_OK) {
        LOG_E("SD 写失败 @块 %lu × %lu", (unsigned long)block, (unsigned long)count);
        return -1;
    }
    return (HAL_SD_GetCardState(&hsd) == HAL_SD_CARD_TRANSFER) ? 0 : -1;
}
