#ifndef BSP_SD_H
#define BSP_SD_H
/* bsp_sd —— 板载 TF 卡座（SDIO 4-bit）。
 * 上层按"块号 + 缓冲区"读写，SD_HandleTypeDef 挡在本层内。
 *
 * 接线（见 docs/guides/01）：PC12=CLK / PD2=CMD / PC8-PC11=D0-D3，
 * **PD3 = 卡检测**（板上经 20k 上拉，插卡接地 ⇒ 低电平）。 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t block_count; /* 总块数 */
    uint32_t block_size;  /* 每块字节数（SD 卡通常是 512）*/
    uint32_t capacity_kb; /* 容量（KB），只为人读日志方便 */
    uint8_t card_type;    /* 1=SDSC 2=SDHC/SDXC 3=MMC，0=未知 */
} bsp_sd_info_t;

/* 卡检测脚（PD3）当前是否插着卡。1 = 插着，0 = 没插。
 * 纯 GPIO 读，不碰 SDIO —— 没插卡时也能安全调用。 */
int bsp_sd_is_detected(void);

/* 初始化 SD 卡。返回 0 成功，-1 失败（没插卡 / 卡不响应 / 初始化超时）。
 *
 * ⚠️ 会**重新**调 `HAL_SD_Init()`。这是有意的：
 *    `MX_SDIO_SD_Init()`（CubeMX 生成，在 main 里）上电就跑了一次，
 *    那时若没插卡必然失败。本函数让调用方能在"用户插上卡之后"再试一次，
 *    不必重启板子。
 * ⚠️ 调用前请先用 `bsp_sd_is_detected()` 判一下 —— 没卡时它要等超时才返回。 */
int bsp_sd_init(void);

/* 取卡信息（容量 / 块大小 / 卡类型）。需先 init 成功。返回 0 成功。 */
int bsp_sd_get_info(bsp_sd_info_t *info);

/* 按块读写。block 是**块号**不是字节地址（1 块 = block_size 字节）。
 * 返回 0 成功，-1 失败（参数非法 / 卡错误 / 超时）。
 * ⚠️ 阻塞式：单次别传太大的 count，否则会长时间占住调用它的任务。 */
int bsp_sd_read_blocks(uint32_t block, uint8_t *buf, uint32_t count);
int bsp_sd_write_blocks(uint32_t block, const uint8_t *buf, uint32_t count);

#ifdef __cplusplus
}
#endif
#endif /* BSP_SD_H */
