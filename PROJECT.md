# PROJECT

阶段: 产品线 ③ EDA 落图 ✓（原理图三页完成）→ ④ PCB 设计（**布局布线首版已完成（用户，2026-10-06）**，
        待按 `hardware/50` 复核整改 + 铺铜）→ ⑤ 投板与 bring-up

交付物: 原理图三页（AFE-前端模拟 / INT-核心板与接口 / PWR-电源与背光）✓ `EDA 工程 ai`（`Board1` = `G2026-底板原理图` ↔ `PCB1`）,
        落图体检报告 ✓ `hardware/38-底板设计状态体检-20261004.md`,
        布局约束与投板检查单 ✓ `hardware/37-PCB布局要点与投板检查单-20261003.md`,
        **PCB 布局布线首版 ✓**（320 段走线 / 57 过孔；EVK1 已翻底层=三层堆叠方案 B，
        **待整改**：33Ω 贴错端 21 只 / R1·R6·R7 位置 / 铺铜——全部见 `hardware/50`）,
        布线复核与铺铜策略 ✓ `hardware/50-PCB布局布线复核与铺铜策略-20261005.md`,
        固件四层工程 ✓ `firmware/`（553 TU 复编零告警）,
        **上板 bring-up ✗**（装置尚未制造，三态口径一律"未验证"）,
        **设计报告 ✗**

门禁基线: env=0 build=0 test=0 lint=0 size=0 selftest=-
          （2026-10-04 本机复跑：`fw.py env`=0 · `fw.py lint`=0 · `fw.py doccheck`=0 · `fw.py test`=5/5=0 ·
           `fw.py sizecheck`=0（FLASH 508492 B / 1 MB = 48.49%）· build 走等价路径
           `python ../_archive/_review3/compile_all.py`（**553 TU · 失败 0 · 告警 0 · 31s**，本沙箱 ninja 跑不动）；
           `selftest` 本会话未跑，最后一次记录见 `hardware/33`）
          EDA 侧基线（2026-10-04 活体）：`sch check` AFE=2 / INT=47 / PWR=1（**全 WARN，0 ERROR**）·
           `sch drc` 三页 0 致命 / 0 错误 / 1 警告（**默认模式判 passed；`--strict` 判 FAIL**）·
           `sch nets` 68 网（无同轨异名 / 无单引脚网）·
           `sch gate --strict` **三页 FAIL**（阻塞项恒为 `layout-lint: sheet-check unavailable` —— 三页无图纸边框图元）·
           `pcb drc` **FAIL 191 条（全是 Connection Error = 未布线）** · `pcb stackup` copperLayerCount=2

待办: 1. `hardware/50` P1 落实：**33Ω×21 迁位到 EVK1 侧 3~7mm**（或拍板接受负载端阻尼折中并记录）、
        R1 距 SMA ≤3mm、R6/R7 距 U1.6 ≤3mm（C10/JP2 已迁 ✓）
      2. 按 `hardware/50 §4` 铺铜：AGND 岛 + GND 底铜分界、电感下方挖空、SMA/ADC 包地、
        J5.29 ≥4 过孔、缝合栅栏延到东半区
      3. **BOM 采购缺口**：补 2×20 双排排母 ×2（直插 2.54mm、中心距 35.6mm，见 `49 §5-④`）；`R11`(旧R8) 补 LCSC C 号
      4. 屏模块实物 3 秒确认：玻璃与排针异面（`49 §4.6` 装配前提）+ IM=010 复核 + DE/PCLK/VSYNC/HSYNC 锡桥
      5. 板框确认/绘制 → `pcb drc` 复跑到 passed（当前 191 条 Connection Error 为未布线旧数，布线后未复跑）
      6. 确认实物是 VET6 还是 VGT6（FLASH 余量 48% vs ~1%）
      7. 上电前：`49 §4` 8 针万用表核对表 + `38` §3.1 上板实测

回退点: 2026-10-04 14:56 · **回滚后（= 优化前口径，已逐项验收一致）** · `_archive/_review5/G2026_after_rollback_20261004.epro2`
        （sha256 `f42b5895806f0343756016222afa863b2df05d488ed897189d6c580571194d85`，1,958,713 B）
        2026-10-04 14:03 · **优化前**整包 · `_archive/_review5/G2026_before_opt_20261004.epro2`
        （sha256 `6aa5d45c96be75e1cffb4fa9936c2f6b5b024d7e3648bed552d5af020ef32f90`，1,953,499 B）
        2026-10-04 14:17 · **§A+§C-1 已落、destagger 之前** · `_archive/_review5/G2026_after_A_C1_before_destag.epro2`
        （sha256 `374a9352df9a508f1edde48083ef104ac5c5677f13745b4383e7789591a6b24b`，1,960,899 B）
        2026-10-04 12:33 · **J5 引脚映射改图前**整包 · 锚点 `_archive/_review5/G2026_before_J5fix_20261004.epro2`
        （sha256 `b129352df7a01567e758718082d14afd87ddd1b410dece1acbeb958425e07d5f`，1,959,342 B）
        更早：`_archive/_review3/post_20261002.epro2`（10-02 21:13）· git `be8ea6d`（工作流重构 批次 4/4：三区迁移）

下一步判据: `easyeda pcb drc --doc 9346501af55b5204` 返回 `passed=true`（0 条 `Connection Error`），
            且 `easyeda pcb outline-get` 的 `segments > 0`、`easyeda pcb check` 为 `0 ERROR`。
