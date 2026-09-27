#ifndef BSP_SPI_H
#define BSP_SPI_H
/* bsp_spi —— SPI1 + 板载 W25Q128（16MB SPI Flash）。
 * 上层只说"读/写/擦哪一段"，SPI_HandleTypeDef、GPIO_AF5 这类芯片符号
 * 都挡在本层内部。
 *
 * 接线（见 docs/guides/01）：PA5=SCK / PA6=MISO / PA7=MOSI，
 * **PA4 = 软件片选**（由本层自己拉，不是 SPI1 的硬件 NSS）。 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 板载 U5 的容量与粒度。W25Q128 = 16MB = 256 块 × 64KB = 4096 扇区 × 4KB。 */
#define BSP_SPI_FLASH_SIZE_BYTES (16UL * 1024UL * 1024UL)
#define BSP_SPI_FLASH_SECTOR_SIZE 4096UL /* 最小擦除单位 */
#define BSP_SPI_FLASH_PAGE_SIZE 256UL    /* 单次写入的上限（跨页要自己拆）*/

/* 期望的 JEDEC ID：0xEF = Winbond，0x18 = 128Mbit(16MB) ⇒ 拼成 0xEF18。
 *
 * ⚠️ 这是**判据不是配置** —— 读回来对不上，说明 Flash 没接好、型号不对，
 *    或者 SPI 模式配错。改它不会让硬件变对。
 *
 * ⚠️ 官方例程用的是 0x90 命令，读到 0xEF17 —— 那是"设备 ID"，不是 JEDEC ID。
 *    两个数都对，只是含义不同；本层用标准的 0x9F（JEDEC ID），
 *    因为它能区分容量（0x18=16MB），换 Flash 型号时判据更硬。详见 bsp_spi.c。 */
#define BSP_SPI_FLASH_JEDEC_ID_EXPECTED 0xEF18U

/* 初始化：CS 拉高、读一次 ID 自检。
 * 返回 0 = 成功且 ID 匹配；-1 = ID 不匹配（接线/型号/SPI 模式有问题）。
 * ⚠️ 本函数**不配引脚也不开时钟** —— 那两件事由 CubeMX 生成的
 *    `Core/Src/spi.c` 的 `HAL_SPI_MspInit` 做（docs/10 §B 第 5 条）。 */
int bsp_spi_flash_init(void);

/* 读 JEDEC ID（高字节 = 制造商，低字节 = 容量码）。 */
uint16_t bsp_spi_flash_read_id(void);

/* 读数据。返回 0 成功，-1 = 参数越界。 */
int bsp_spi_flash_read(uint32_t addr, uint8_t *buf, uint32_t len);

/* 写**一段不跨页**的数据。
 *
 * ⚠️ 本层**只做单页写，不负责拆分** —— 拆分是纯计算，属于 `Operation/`
 *    （`op_flash_split_pages()`）；而 **R1 禁止 Device 调 Operation**
 *    （CMake 里 `device` 目标根本不链 `operation`，include 了也编不过）。
 *    组合的活由组合根 `Task/Src/app.c` 干，样板见 `app_flash_write()`。
 *
 * ⚠️ `len` 超过"从 addr 到本页末尾"的剩余字节数时返回 -1。
 *    这是与裸 Flash 命令的**关键差异**：硬件层跨页会**回卷覆盖**且不报错，
 *    本层替你把它变成显式错误。
 *
 * ⚠️ **不自动擦除** —— Flash 只能把 1 写成 0，要写回 1 必须先擦整个扇区。
 *    所以"改一小段"的正确姿势是：读整个扇区 → 内存里改 → 擦扇区 → 写回。
 *    直接对已有数据调本函数，结果是"按位与"，不是覆盖。
 *
 * 返回 0 成功，-1 = 参数非法 / 跨页 / 超时。 */
int bsp_spi_flash_write_page(uint32_t addr, const uint8_t *buf, uint32_t len);

/* 擦除 addr 所在的 4KB 扇区（addr 向下对齐到扇区边界）。返回 0 成功。 */
int bsp_spi_flash_erase_sector(uint32_t addr);

/* 整片擦除。⚠️ W25Q128 典型耗时 **~40 秒**，别在任务里干等 ——
 * 本函数是阻塞的，调用方自己决定放哪个上下文。返回 0 成功。 */
int bsp_spi_flash_erase_chip(void);

#ifdef __cplusplus
}
#endif
#endif /* BSP_SPI_H */
