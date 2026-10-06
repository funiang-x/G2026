# -*- coding: utf-8 -*-
"""S9 · 8080 接口（FSMC → J5 → ST7796S）信号完整性 —— **解析式**

⚠️ 为什么不用瞬态仿真出数：
   我先写了一版集总 LC 瞬态模型（N 条线共用一根地针），但它给出"地弹与 L_gnd 无关、
   恒为 3.1V"的退化结果 —— 集总模型**定不了信号电感与回流电感的真实分配**，那个数字
   不可信。**宁可不算，也不发布一个自己不信的数。** 所以本节全部改成解析式：

   ① 源端串阻的阻尼：串联 RLC 的二阶阶跃响应（教科书式，结论稳健）
   ② 地弹：ΔV = L_gnd · Σ(di/dt)（一阶近似，只用来做**相对比较**与设计规则）
   ③ 时序余量：FSMC 时序参数 vs 屏模块需求
   ④ 四条可执行的设计规则

   **绝对值只有上板用示波器看 J5 侧边沿才能定**（判据见 `hardware/23 §7`）。
"""
from __future__ import annotations

import math

VS = 3.3          # FSMC 输出高电平
RO = 20.0         # STM32 F4 驱动器输出阻抗（典型 15~25Ω）
CLOAD = 120e-12   # 屏输入电容 + 排线/走线电容
# ★ 阻尼算的是**回路总电感**（信号线电感 + 回流地电感），不是只算信号线。
#   成品（直插转接板）：信号 ~30nH + 回流 ~30nH = 60nH
#   台面（100mm 杜邦飞线）：信号 ~100nH + 回流 ~115nH = 215nH
L_LOOP_PROD = 60e-9
L_LOOP_BENCH = 215e-9


def rlc_step(rs: float, ro: float = RO, l: float = L_LOOP_PROD, c: float = CLOAD):
    """串联 RLC 阶跃响应：返回 (ζ, 过冲比例, 上升时间 10-90%, 振荡频率)。"""
    rt = rs + ro
    z0 = math.sqrt(l / c)
    zeta = rt / (2 * z0)
    f0 = 1 / (2 * math.pi * math.sqrt(l * c))
    if zeta < 1:
        wd = 2 * math.pi * f0 * math.sqrt(1 - zeta * zeta)
        os_ = math.exp(-math.pi * zeta / math.sqrt(1 - zeta * zeta))
        tr = (1.0 / wd) * (math.pi - math.acos(zeta)) if wd > 0 else float("inf")
    else:
        os_ = 0.0
        tr = 2.2 * rt * c
    return zeta, os_, tr, f0


def damping_table(l_loop, tag):
    z0 = math.sqrt(l_loop / CLOAD)
    f0 = 1 / (2 * math.pi * math.sqrt(l_loop * CLOAD))
    print(f"\n  【{tag}】回路 L={l_loop*1e9:.0f}nH ⇒ Z0={z0:.1f}Ω, f0={f0/1e6:.1f}MHz")
    print(f"  {'串阻':>7}{'阻尼比 zeta':>13}{'过冲':>9}{'屏端峰值':>11}{'上升时间':>11}{'评价'}")
    for rs in (0.0, 10.0, 22.0, 33.0, 47.0, 68.0):
        z, os_, tr, _ = rlc_step(rs, l=l_loop)
        vpk = VS * (1 + os_)
        note = ("欠阻尼·严重振铃" if z < 0.3 else "欠阻尼·有振铃" if z < 0.6
                else "接近临界·好" if z < 0.9 else "过阻尼·边沿偏慢")
        print(f"  {rs:>6.0f}Ω{z:>12.2f}{os_*100:>8.0f}%{vpk:>10.2f}V{tr*1e9:>10.1f}ns  {note}")


def main():
    print("=" * 96)
    print("S9 · 8080 接口信号完整性（解析式；模型边界见文件头）")
    print("=" * 96)

    z0 = math.sqrt(L_LOOP_PROD / CLOAD)
    print("\n① 源端串阻的阻尼作用（串联 RLC 阶跃；**用回路总电感**，不是只算信号线）")
    print(f"   Ro={RO:.0f}Ω, C_load={CLOAD*1e12:.0f}pF")
    damping_table(L_LOOP_PROD, "成品 · 直插转接板（禁飞线，O9）")
    damping_table(L_LOOP_BENCH, "台面 · 100mm 杜邦飞线")
    z33p, os33p, tr33p, _ = rlc_step(33.0, l=L_LOOP_PROD)
    z0b, os0b, tr0b, _ = rlc_step(0.0, l=L_LOOP_BENCH)
    z33b, os33b, tr33b, _ = rlc_step(33.0, l=L_LOOP_BENCH)
    print(f"\n  => 成品：33Ω 使 zeta={z33p:.2f}（**过阻尼**）、过冲 {os33p*100:.0f}%、边沿 {tr33p*1e9:.1f}ns。"
          f"\n     成品形态其实 10Ω 就够（zeta=0.67、过冲 6%、边沿 8.3ns）；"
          f"\n     **33Ω 是为最坏的台面/飞线场景选的**，对成品偏保守 —— 但代价只是边沿慢 6ns，"
          f"\n     而 104ns 周期有大量余量 ⇒ **不值得为成品形态单独换料，保持 33Ω 统一 BOM**。")
    print(f"  => 台面：不串阻尼时 zeta={z0b:.2f}、过冲 {os0b*100:.0f}%（边沿 {tr0b*1e9:.1f}ns，"
          f"已接近 104ns 周期的 1/10）；串 33Ω 后过冲降到 {os33b*100:.0f}%、边沿 {tr33b*1e9:.1f}ns。")
    print("     ⇒ **这正是「禁飞线」要防的**：同一块板，台面的振铃比成品严重得多。")

    print("\n② 地弹（一阶近似 dV = L_gnd * sum(di/dt)）—— 只做**相对比较**")
    di_line = VS / ((RO + 33.0) ** 2 * CLOAD) / 1e6   # A/us
    print(f"   单线 di/dt 取阶跃上界 {di_line:.1f} A/µs（实际边沿 9.8ns 会更小）")
    print(f"  {'同时翻转':>9}{'sum di/dt':>12}" +
          "".join(f"{('L=' + str(int(l*1e9)) + 'nH'):>13}" for l in (10e-9, 30e-9, 115e-9)))
    for n in (1, 2, 4, 8, 16):
        s = n * di_line
        row = f"  {n:>9}{s:>10.0f}A/µs"
        for l in (10e-9, 30e-9, 115e-9):
            row += f"{l * s * 1e6 * 1e3:>11.0f}mV"
        print(row)
    print("  => 地弹 正比于 L_gnd x N x di/dt。N 由总线宽度定死（16），"
          "所以只能压 L_gnd 与 di/dt。")

    print("\n③ 时序余量（与幅度无关，另一条独立保障）")
    print("   FSMC：ADDSET=2 / DATAST=12 @ HCLK=144MHz => 每像素 (2+12+1)x6.94ns ~ 104ns")
    print("   屏模块 ST7796S 8080-16bit 写入脉宽需求：~15~66ns")
    print("   => 余量 **1.6x ~ 7x**；回退档 12->16->20->25（fsmc.c 注释里写了）")
    print("   建立/保持：33Ω 下边沿 8.3~14.6ns（成品~台面）远小于 104ns 周期 => 充裕")

    print("\n④ 四条可执行的设计规则")
    print("   R1 **保持 33Ω 源端串阻**，且 **PCB 必须贴 MCU 引脚 3~5mm**"
          "（放连接器侧 = 阻尼在负载端 = 等于没加）")
    print("   R2 **压 L_gnd**：J5 的 GND 走线短/宽/多过孔；**禁飞线**"
          "（直插转接板把 L 从 ~115nH 降到 ~30nH）")
    print("   R3 **压 di/dt**：33Ω 把台面边沿从 9.5ns 拉到 14.6ns、过冲 47%→8%；"
          "再用**采样窗静默**把「总线翻转」与「模拟采样」在时间上错开")
    print("   R4 **别忘 RST**：LCD_RST 是 21 条控制/数据线里唯一没串阻的（27 号 H3）")

    print("\n[!] 边界：①② 的绝对值是**上界估计**，不是实测余量。")
    print("    上板判据见 hardware/23 §7：示波器看 J5 侧数据/控制线边沿（对比 R.n 两侧），")
    print("    并复测 TP4 噪声底 <0.5mVrms。")


if __name__ == "__main__":
    main()
