/* op_flash —— SPI Flash 页写拆分的纯逻辑。设计说明见 op_flash.h。
 *
 * ⚠️ 本文件被 **PC 侧单测**直接编译（`Test/CMakeLists.txt` 的 `OP_SRC`）。
 *    这里一旦混进 HAL / FreeRTOS 头，`fw.py test` 会当场链接失败 ——
 *    那是 docs/01 规矩 2 的第二道墙，别想着"临时 include 一下"。 */
#include "op_flash.h"

/* 从 addr 起，本页还剩多少字节可用（addr 落在页边界时 = 整页）。 */
static uint32_t page_left(uint32_t addr, uint32_t page_size) {
    return page_size - (addr % page_size);
}

int op_flash_page_count(uint32_t addr, uint32_t len, uint32_t page_size) {
    if (page_size == 0U || len == 0U) {
        return -1;
    }
    if (addr > UINT32_MAX - len) {
        return -1; /* addr + len 溢出：宁可失败，也别算出一个假地址 */
    }

    int n = 0;
    uint32_t done = 0U;
    while (done < len) {
        uint32_t chunk = page_left(addr + done, page_size);
        uint32_t remain = len - done;
        if (chunk > remain) {
            chunk = remain;
        }
        done += chunk;
        n++;
    }
    return n;
}

int op_flash_split_pages(uint32_t addr, uint32_t len, uint32_t page_size,
                         op_flash_seg_t *out, int out_cap) {
    if (out == NULL || out_cap <= 0) {
        return -1;
    }
    int total = op_flash_page_count(addr, len, page_size);
    if (total < 0) {
        return -1;
    }
    if (total > out_cap) {
        return -1; /* 装不下就整体失败，不返回"半截结果" —— 半个拆分比没有更危险 */
    }

    int n = 0;
    uint32_t done = 0U;
    while (done < len) {
        uint32_t chunk = page_left(addr + done, page_size);
        uint32_t remain = len - done;
        if (chunk > remain) {
            chunk = remain;
        }
        out[n].offset = done;
        out[n].len = chunk;
        n++;
        done += chunk;
    }
    return n;
}
