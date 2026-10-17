# 第三方组件与许可

本仓库的原创代码与文档采用根目录 [`LICENSE`](LICENSE)（MIT）。
下列目录为第三方组件，版权归各自所有者，使用时请遵守其原始许可；本仓库不改变其许可。

| 目录 / 文件 | 组件 | 许可 | 说明 |
|---|---|---|---|
| `firmware/Drivers/STM32F4xx_HAL_Driver/` | STM32F4 HAL Driver | BSD-3-Clause | STMicroelectronics，随 CubeMX 固件包分发 |
| `firmware/Drivers/CMSIS/` | ARM CMSIS | Apache-2.0 | ARM Limited |
| `firmware/Middlewares/Third_Party/FreeRTOS/` | FreeRTOS | MIT | Amazon / FreeRTOS 项目 |
| `firmware/Middlewares/LVGL/` | LVGL 9.5 | MIT | LVGL Kft. |
| `firmware/Middlewares/RTT/` | SEGGER RTT | SEGGER 许可 | 版权归 SEGGER；仅用于调试日志 |
| `firmware/Middlewares/ST/ARM/DSP/` | CMSIS-DSP 预编译库 | Apache-2.0 | ARM Limited |
| `firmware/Middlewares/LVGL/src/font/` | Montserrat / 思源黑体子集 | OFL-1.1 | 字体版权归各自作者 |

## 未纳入仓库的厂商资料

以下资料体积大、且版权归厂商，未上传，仅在本地使用：

- 立创·梁山派·天空星 STM32F407VET6 开发板资料（标准库）
- TK035F5687 / TK035F9629 屏与转接板资料（含 SPI 及并口版）

需要时请从厂商官方渠道获取。

## 引用说明

若在论文、答辩或衍生项目中使用本仓库的电路参数、算法口径或实测数据，请注明来源：
`G2026 — 周期信号测量分析装置`，并附仓库地址。
