#ifndef OP_FLASH_H
#define OP_FLASH_H
/* ============================================================================
 *  op_flash.h —— SPI Flash 页写拆分的**纯逻辑**（Operation 层）
 *
 *  本文件**不许**出现 HAL / FreeRTOS / Device 的任何东西（docs/01 规矩 2）。
 *
 *  ★ 为什么它值得单独一层：
 *    NOR Flash 的"页写"指令（W25Q128 的 0x02）**不能跨页** —— 越过 256B
 *    边界后，地址会**回卷到本页开头**，静默覆盖前面的数据。
 *    所以一次跨页写必须拆成若干页内段。
 *
 *    拆分的边界计算很容易错，而且错了**不报错、只是数据坏**：
 *      · 起始地址未对齐（第一段要短）
 *      · 正好落在页边界（不该多切一段）
 *      · 跨多页（段数 ≠ len / page_size）
 *    ⇒ 这正是该放进 Operation 层、用 PC 单测钉死的东西。
 * ==========================================================================*/
#include <stddef.h> /* NULL —— ⚠️ 别省：host gcc 会间接带上它，但
                     * arm-none-eabi-gcc 不会，少了就是"PC 上编得过、MCU 上编不过"
                     * （2026-09-23 实测：只在 host 跑 `fw.py test` 全绿，
                     *  交叉编译时才报 'NULL' undeclared）。 */
#include <stdint.h>

/* 一段连续的页内写。offset 相对**本次写入的起始地址**（不是绝对地址）。 */
typedef struct {
    uint32_t offset;
    uint32_t len;
} op_flash_seg_t;

/* 把 [addr, addr+len) 按 page_size 拆成若干**不跨页**的段。
 *
 * out    调用方提供的数组；out_cap 是它的容量（能装多少段）。
 * 返回   段数（>= 1）；参数非法 / 溢出 / out 装不下时返回 -1。
 *
 * ⚠️ 判据是"**段内不跨页**"，不是"从 addr 起每 page_size 一段" ——
 *    起始地址未对齐时第一段会短一截，这是最容易写错的地方。
 * ⚠️ `addr + len` 溢出 uint32 时返回 -1（宁可失败也不要算出一个假地址）。 */
int op_flash_split_pages(uint32_t addr, uint32_t len, uint32_t page_size,
                         op_flash_seg_t *out, int out_cap);

/* 拆一次跨页写最少需要几段（不实际拆，只算数）。
 * 用途：调用方可以先问一句，再决定缓冲区开多大 —— 免得猜。
 * 返回段数；参数非法返回 -1。 */
int op_flash_page_count(uint32_t addr, uint32_t len, uint32_t page_size);

#endif /* OP_FLASH_H */
