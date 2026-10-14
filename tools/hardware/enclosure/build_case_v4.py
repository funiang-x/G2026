# -*- coding: utf-8 -*-
"""G2026 底板外壳 v4 —— 适配「屏 TK035F9629 + 转接板 TK035F5501(98.70×56.00)」真实模块
（2026-10-11；相对 v3 的修正：新板框 80.25×100.25、真实屏/转接板几何、3 个可用螺柱）

坐标约定（板内 mm）：原点 = 板框 bbox 左下角，X 右、Y 上、Z 从外壳外底起算。
数据来源（全部可复算）：
  · 板框 / 螺柱孔 / J5 / J1 / EVK1：活体快照 `_work/enclosure_v4/pcb_live.json`
    （2026-10-11 10:23 从 EasyEDA 拉取，脚本 probe_live.py）
  · 转接板外形 / 排针位置 / 屏在板上的位置：转接板 Altium 源文件（probe_adapter3.py，
    路径见 hardware/58 §6）；屏外形 55.05×84.50×2.06 / 玻璃 52.96×81.34：屏规格书 P.4/P.5
  · ⚠️ 未知量（不能当定稿）：转接板板厚、排针啮合高度、FPC 弯折空间、核心板吊装高度、
    核心板 Type-C 位置 —— 全部做成参数，见假设表 PROVENANCE。
"""

import os
import json
import time
import numpy as np
import cadquery as cq
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from mpl_toolkits.mplot3d.art3d import Poly3DCollection
from matplotlib.patches import Rectangle

plt.rcParams["font.sans-serif"] = ["Microsoft YaHei", "SimHei", "DejaVu Sans"]
plt.rcParams["axes.unicode_minus"] = False

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "out")
os.makedirs(OUT, exist_ok=True)

# ============================================================
# 实测数据（板坐标，mm）—— 来源见 docstring / provenance()
# ============================================================
BOARD = dict(w=80.25, h=100.25, t=1.60)                 # 活体 bbox（含 10mil 线宽）
SCREWS = [(4.32, 4.50), (75.56, 4.50), (75.56, 94.16)]  # SCREW1/2/4；SCREW3(40.64,46.53) 在屏正下方，弃用
CORE_BB = (1.08, 53.61, 71.31, 98.87)                   # EVK1 核心板吊装区（板下）, x0,y0,x1,y1
J1 = dict(cx=3.56, cy=41.55)                            # SMA-KE 天线座（左缘）
J5 = dict(cx=38.86, cy=82.85)                           # 2×15P 母座中心
ADAPT = dict(w=56.01, l=98.71, cx=38.86, y_min=-11.455)  # 转接板：x 居中于 J5，向 -Y 外伸约 11.5
SCREEN = dict(cx=38.86, cy=37.875, w=55.05, h=84.50,
              glass=(52.96, 81.34), aa=(48.96, 73.44), t=2.06)

# ============================================================
# 设计参数（P=结构；ASSUME=待实测假设）
# ============================================================
P = dict(
    clearance=0.4,       # 板与内壁单边间隙
    wall=2.5,            # 侧壁厚
    floor=2.5,           # 底厚
    lid_t=2.5,           # 顶板厚
    corner_r=4.0,        # 竖边圆角
    boss_od=7.0,         # 螺丝柱外径
    boss_hole=2.6,       # M3 自攻底孔
    boss_depth=7.0,      # 柱内盲孔深
    screw_hole=3.4,      # 上盖 M3 过孔
    lip_t=2.0,           # 唇边伸入深度
    lip_w=2.0,           # 唇边环宽（v4 由实心板改为环——实心会压到屏）
    lip_gap=0.25,        # 唇边与内壁间隙
    foot_d=10.0,         # 脚垫凹槽直径
    foot_h=1.2,          # 脚垫凹槽深
    vent_d=2.4,          # 右壁散热孔
)
ASSUME = dict(
    bot_depth=14.0,      # 板底腔高：核心板吊装（排母~8.5 + 板1.6 + 底部元件）→ 待实测
    adapter_z=12.0,      # 板上净空第一段：板顶→转接板底面（J5 座高~10.1 + 排针塑体）→ 待实测
    adapter_t=1.6,       # 转接板板厚（Altium 文件未给）→ 待实测
    fpc_gap=1.5,         # 转接板顶面→屏背面（FPC 折弯空间）→ 待实测
    top_gap=0.5,         # 屏面→上盖底面（装时贴泡棉/双面胶）
    sma_z=4.5,           # SMA 中心高出板顶
    sma_d=8.5,           # SMA 左壁开孔直径
    typec_on=1,          # 核心板 Type-C 左壁开孔（0=不开）
    typec_cy=76.20,      # 开孔中心（Y，板坐标）→ 待实测
    typec_z=-10.0,       # 开孔中心低于板顶 → 待实测
    typec_w=12.0,
    typec_h=8.0,
)
WIN = dict(w=SCREEN["glass"][0] + 1.0, h=SCREEN["glass"][1] + 1.0)  # 玻璃 + 单边 0.5


def geom():
    """由参数推出全部外壳几何。"""
    ax0 = ADAPT["cx"] - ADAPT["w"] / 2
    ax1 = ADAPT["cx"] + ADAPT["w"] / 2
    ay1 = ADAPT["y_min"] + ADAPT["l"]

    in_x0, in_x1 = -P["clearance"], BOARD["w"] + P["clearance"]
    in_y0, in_y1 = ADAPT["y_min"] - 0.5, BOARD["h"] + P["clearance"]
    IN_W, IN_H = in_x1 - in_x0, in_y1 - in_y0
    OUT_W, OUT_H = IN_W + 2 * P["wall"], IN_H + 2 * P["wall"]

    zB = P["floor"] + ASSUME["bot_depth"]                 # 板底
    zT = zB + BOARD["t"]                                 # 板顶
    head = (ASSUME["adapter_z"] + ASSUME["adapter_t"] + ASSUME["fpc_gap"]
            + SCREEN["t"] + ASSUME["top_gap"])
    z_a0 = zT + ASSUME["adapter_z"]                      # 转接板底
    z_a1 = z_a0 + ASSUME["adapter_t"]                    # 转接板顶
    z_s0 = z_a1 + ASSUME["fpc_gap"]                      # 屏背面
    z_s1 = z_s0 + SCREEN["t"]                            # 屏面
    BODY_H = P["floor"] + ASSUME["bot_depth"] + BOARD["t"] + head
    return dict(ax0=ax0, ax1=ax1, ay1=ay1, in_x0=in_x0, in_x1=in_x1, in_y0=in_y0,
                in_y1=in_y1, IN_W=IN_W, IN_H=IN_H, OUT_W=OUT_W, OUT_H=OUT_H,
                cx0=(in_x0 + in_x1) / 2, cy0=(in_y0 + in_y1) / 2,
                zB=zB, zT=zT, head=head, z_a0=z_a0, z_a1=z_a1, z_s0=z_s0, z_s1=z_s1,
                BODY_H=BODY_H, TOTAL_H=BODY_H + P["lid_t"])


G = geom()
L = lambda x, y: (x - G["cx0"], y - G["cy0"])  # 板坐标 → 外壳局部坐标


def make_bottom():
    s = (cq.Workplane("XY")
         .box(G["OUT_W"], G["OUT_H"], G["BODY_H"], centered=(True, True, False))
         .edges("|Z").fillet(P["corner_r"]))
    s = s.faces(">Z").workplane().rect(G["IN_W"], G["IN_H"]).cutBlind(-(G["BODY_H"] - P["floor"]))

    # 螺丝柱（顶面=板底），柱位=板上 3 个可用 M3 孔
    for hx, hy in SCREWS:
        x, y = L(hx, hy)
        s = s.union(cq.Workplane("XY").workplane(offset=P["floor"])
                    .center(x, y).circle(P["boss_od"] / 2).extrude(ASSUME["bot_depth"]))
        s = s.cut(cq.Workplane("XY")
                  .workplane(offset=G["zB"] - P["boss_depth"])
                  .center(x, y).circle(P["boss_hole"] / 2).extrude(P["boss_depth"]))

    # 左壁：SMA 开孔（圆）
    sx, sy = L(J1["cx"], J1["cy"])
    s = s.cut(cq.Workplane("YZ").circle(ASSUME["sma_d"] / 2).extrude(4 * P["wall"])
              .translate((-G["OUT_W"] / 2 - 2 * P["wall"], sy, G["zT"] + ASSUME["sma_z"])))
    # 左壁：核心板 Type-C 开孔（矩形，位置待实测）
    if ASSUME["typec_on"]:
        tx, ty = L(0.0, ASSUME["typec_cy"])
        s = s.cut(cq.Workplane("XY")
                  .box(4 * P["wall"], ASSUME["typec_w"], ASSUME["typec_h"],
                       centered=(False, True, True))
                  .translate((-G["OUT_W"] / 2 - 2 * P["wall"], ty,
                              G["zT"] + ASSUME["typec_z"])))
    # 右壁散热孔 3×4
    for iy in range(3):
        for iz in range(4):
            s = s.cut(cq.Workplane("YZ").circle(P["vent_d"] / 2).extrude(4 * P["wall"])
                      .translate((G["OUT_W"] / 2 - 2 * P["wall"], -18 + iy * 18, 6 + iz * 3.4)))
    # 底部脚垫凹槽（保持底面平整）
    for sxn in (-1, 1):
        for syn in (-1, 1):
            s = s.cut(cq.Workplane("XY").center(sxn * (G["OUT_W"] / 2 - 12),
                                                syn * (G["OUT_H"] / 2 - 12))
                      .circle(P["foot_d"] / 2).extrude(P["foot_h"]))
    return s


def make_top():
    s = (cq.Workplane("XY")
         .box(G["OUT_W"], G["OUT_H"], P["lid_t"], centered=(True, True, False))
         .edges("|Z").fillet(P["corner_r"]))
    wx, wy = L(SCREEN["cx"], SCREEN["cy"])
    s = s.cut(cq.Workplane("XY")
              .box(WIN["w"], WIN["h"], P["lid_t"] * 4, centered=(True, True, True))
              .translate((wx, wy, P["lid_t"] / 2)))
    for hx, hy in SCREWS:
        x, y = L(hx, hy)
        s = s.cut(cq.Workplane("XY").center(x, y).circle(P["screw_hole"] / 2)
                  .extrude(P["lid_t"] * 4).translate((0, 0, -P["lid_t"] * 1.5)))
    # 唇边＝环（v4 修正：v3 的实心唇会压到屏）
    ring = (cq.Workplane("XY")
            .box(G["IN_W"] - 2 * P["lip_gap"], G["IN_H"] - 2 * P["lip_gap"],
                 P["lip_t"], centered=(True, True, False)))
    inner = (cq.Workplane("XY")
             .box(G["IN_W"] - 2 * P["lip_gap"] - 2 * P["lip_w"],
                  G["IN_H"] - 2 * P["lip_gap"] - 2 * P["lip_w"],
                  P["lip_t"] * 3, centered=(True, True, True))
             .translate((0, 0, P["lip_t"] * 0.5)))
    ring = ring.cut(inner).translate((0, 0, -P["lip_t"]))
    return s.union(ring)


def box_at(cx, cy, z0, w, h, d):
    return (cq.Workplane("XY").box(w, h, d, centered=(True, True, False))
            .translate((cx, cy, z0)))


def checks():
    """机械判据（不依赖 STEP；每条都能在改参后立刻复验）。"""
    errs, notes = [], []
    # ① 转接板完全落进内腔
    for name, v, lo, hi in (("adapter.x0", G["ax0"], G["in_x0"], None),
                            ("adapter.x1", G["ax1"], None, G["in_x1"]),
                            ("adapter.y0", ADAPT["y_min"], G["in_y0"], None),
                            ("adapter.y1", G["ay1"], None, G["in_y1"])):
        m = (v - lo) if hi is None else (hi - v)
        (errs if m < 0.3 else notes).append(f"{name} 余量 {m:.2f} mm")
    # ② 螺丝柱/过孔避开「转接板+屏」投影（含柱半径）
    for hx, hy in SCREWS:
        clash = []
        if G["ax0"] - P["boss_od"] / 2 < hx < G["ax1"] + P["boss_od"] / 2 and \
                ADAPT["y_min"] - P["boss_od"] / 2 < hy < G["ay1"] + P["boss_od"] / 2:
            clash.append("adapter")
        s_l, s_r = SCREEN["cx"] - SCREEN["w"] / 2, SCREEN["cx"] + SCREEN["w"] / 2
        s_b, s_t = SCREEN["cy"] - SCREEN["h"] / 2, SCREEN["cy"] + SCREEN["h"] / 2
        if s_l - P["boss_od"] / 2 < hx < s_r + P["boss_od"] / 2 and \
                s_b - P["boss_od"] / 2 < hy < s_t + P["boss_od"] / 2:
            clash.append("screen")
        if clash:
            errs.append(f"螺柱 ({hx},{hy}) 撞 {clash}")
    # ③ 螺丝柱避开核心板吊装区
    cx0, cy0, cx1, cy1 = CORE_BB
    for hx, hy in SCREWS:
        r = P["boss_od"] / 2
        if not (hx + r < cx0 or hx - r > cx1 or hy + r < cy0 or hy - r > cy1):
            errs.append(f"螺柱 ({hx},{hy}) 落在核心板区")
    # ④ 窗口 vs 玻璃/模块
    vgx = (WIN["w"] - SCREEN["glass"][0]) / 2
    vgy = (WIN["h"] - SCREEN["glass"][1]) / 2
    bmx = (SCREEN["w"] - WIN["w"]) / 2
    bmy = (SCREEN["h"] - WIN["h"]) / 2
    for name, v in (("窗-玻璃 X 单边", vgx), ("窗-玻璃 Y 单边", vgy),
                    ("压边 X", bmx), ("压边 Y", bmy)):
        (errs if v < 0.3 else notes).append(f"{name} {v:.2f} mm")
    # ⑤ 窗口与上盖螺孔距离
    wx, wy = L(SCREEN["cx"], SCREEN["cy"])
    for hx, hy in SCREWS:
        x, y = L(hx, hy)
        dx = max(abs(x - wx) - WIN["w"] / 2, 0)
        dy = max(abs(y - wy) - WIN["h"] / 2, 0)
        d = (dx ** 2 + dy ** 2) ** 0.5
        (errs if d < P["screw_hole"] / 2 + 1.5 else notes).append(f"窗-螺孔({hx:.0f},{hy:.0f}) 距 {d:.1f} mm")
    # ⑥ 唇边环不触碰屏/转接板（Z 向）
    lip_bottom = G["BODY_H"] - P["lip_t"]
    (errs if lip_bottom < G["z_a1"] + 0.5 else notes).append(
        f"唇边底 {lip_bottom:.2f} vs 转接板顶 {G['z_a1']:.2f}")
    return errs, notes


# ---------------------------------------------------------------- 预览
def collect_tris(items, tol=0.6):
    tris = []
    for s in items:
        if isinstance(s, cq.Workplane):
            s = s.val()
        try:
            verts, faces = s.tessellate(tol)
            v = np.array([[p.x, p.y, p.z] for p in verts])
            for f in faces:
                tris.append(v[list(f)[:3]])
        except Exception:
            pass
    return tris


def main():
    t0 = time.time()
    print("=" * 62)
    print("G2026 外壳 v4 建模（屏 + 转接板真实几何）")
    print(f"  板 {BOARD['w']}×{BOARD['h']}×{BOARD['t']}  转接板 {ADAPT['w']}×{ADAPT['l']}")
    print(f"  屏 {SCREEN['w']}×{SCREEN['h']}  窗 {WIN['w']:.2f}×{WIN['h']:.2f}")

    print("\n机械判据：")
    errs, notes = checks()
    for n in notes:
        print(f"  ✓ {n}")
    for e in errs:
        print(f"  ✗ {e}")
    if errs:
        raise SystemExit(f"存在 {len(errs)} 条硬错误，先修参数再导出")

    print("\n建模 ...", flush=True)
    bottom = make_bottom()
    top = make_top()

    print("\n导出 ...", flush=True)
    for shp, nm in ((bottom, "case_bottom"), (top, "case_top")):
        cq.exporters.export(shp, os.path.join(OUT, f"{nm}.step"))
        cq.exporters.export(shp, os.path.join(OUT, f"{nm}.stl"))
        print(f"  out/{nm}.step / .stl")

    meta = dict(
        version="v4-20261011",
        board=BOARD, screws=SCREWS, core_bb=CORE_BB, J1=J1, J5=J5,
        adapter=dict(w=ADAPT["w"], l=ADAPT["l"], x=(round(G["ax0"], 3), round(G["ax1"], 3)),
                     y=(round(ADAPT["y_min"], 3), round(G["ay1"], 3))),
        screen=SCREEN, window=WIN,
        enclosure=dict(OUT_W=round(G["OUT_W"], 2), OUT_H=round(G["OUT_H"], 2),
                       TOTAL_H=round(G["TOTAL_H"], 2), IN_W=round(G["IN_W"], 2),
                       IN_H=round(G["IN_H"], 2)),
        z=dict(zB=round(G["zB"], 2), zT=round(G["zT"], 2), adapter=(round(G["z_a0"], 2), round(G["z_a1"], 2)),
               screen=(round(G["z_s0"], 2), round(G["z_s1"], 2)), lid_under=round(G["BODY_H"], 2)),
        params=P, assumed=ASSUME,
        volumes_cm3=dict(bottom=round(bottom.val().Volume() / 1000, 2),
                         top=round(top.val().Volume() / 1000, 2)),
        provenance=dict(
            实测=["板框 80.25×100.25(活体 bbox)", "4×M3 孔位(活体)", "J5/J1 位置(活体)",
                  "核心板吊装区(活体 EVK1 bbox)", "转接板 56.01×98.71 与排针/屏相对位置(Altium)",
                  "屏外形/玻璃/AA/厚(规格书 P.4/P.5)"],
            假设=["bot_depth=14 ← 核心板吊装高度未实测", "adapter_z=12 ← 排针啮合高度未实测",
                  "adapter_t=1.6 ← 转接板厚未实测", "fpc_gap=1.5 ← FPC 折弯未实测",
                  "top_gap/sma_z/sma_d/typec_* ← 装配序与实物实测后校"],
            待核实=["转接板排针行距(Altium 文件 5.2mm) vs J5(2.54mm)：实物试插",
                    "SCREW3 位于屏正下方：勿用于锁壳", "窗口公差按屏 ±0.2 复核"]),
    )
    with open(os.path.join(OUT, "design-meta.json"), "w", encoding="utf-8") as f:
        json.dump(meta, f, ensure_ascii=False, indent=2)
    print("  out/design-meta.json")

    # ---------- 预览 ----------
    print("\n渲染预览 ...", flush=True)
    bt = collect_tris([bottom], tol=0.5)
    tt = collect_tris([top], tol=0.5)
    board_b = box_at(*L(BOARD["w"] / 2, BOARD["h"] / 2), G["zB"],
                     BOARD["w"], BOARD["h"], BOARD["t"])
    adap_b = box_at(L(ADAPT["cx"], (ADAPT["y_min"] + G["ay1"]) / 2)[0],
                    L(ADAPT["cx"], (ADAPT["y_min"] + G["ay1"]) / 2)[1],
                    G["z_a0"], ADAPT["w"], ADAPT["l"], ASSUME["adapter_t"])
    scr_b = box_at(L(SCREEN["cx"], SCREEN["cy"])[0], L(SCREEN["cx"], SCREEN["cy"])[1],
                   G["z_s0"], SCREEN["w"], SCREEN["h"], SCREEN["t"])
    core_b = box_at(L((CORE_BB[0] + CORE_BB[2]) / 2, (CORE_BB[1] + CORE_BB[3]) / 2)[0],
                    L((CORE_BB[0] + CORE_BB[2]) / 2, (CORE_BB[1] + CORE_BB[3]) / 2)[1],
                    G["zB"] - ASSUME["bot_depth"] + P["floor"] + 1.0,
                    CORE_BB[2] - CORE_BB[0], CORE_BB[3] - CORE_BB[1], 2.0)
    pt = (collect_tris([board_b], tol=0.8) + collect_tris([adap_b], tol=0.8)
          + collect_tris([scr_b], tol=0.8) + collect_tris([core_b], tol=0.8))

    fig = plt.figure(figsize=(16.5, 6.2))
    R = max(G["OUT_W"], G["OUT_H"]) / 2 * 1.15
    ZT = G["TOTAL_H"] + 20

    ax1 = fig.add_subplot(1, 3, 1, projection="3d")
    ax1.add_collection3d(Poly3DCollection(bt, facecolor="#85B7EB", edgecolor="none", alpha=0.2))
    ax1.add_collection3d(Poly3DCollection(pt[:12], facecolor="#D85A30", edgecolor="none"))
    ax1.add_collection3d(Poly3DCollection(pt[12:24], facecolor="#EF9F27", edgecolor="none", alpha=0.75))
    ax1.add_collection3d(Poly3DCollection(pt[24:36], facecolor="#35B2C8", edgecolor="none", alpha=0.85))
    ax1.set_axis_off(); ax1.set_title("① 下盖(半透明) + 板/转接板/屏/核心板", fontsize=10.5, pad=2)
    ax1.view_init(30, -58); ax1.set_box_aspect((1, 1, 0.6))
    ax1.set_xlim(-R, R); ax1.set_ylim(-R, R); ax1.set_zlim(-6, ZT)

    ax2 = fig.add_subplot(1, 3, 2, projection="3d")
    ax2.add_collection3d(Poly3DCollection(bt, facecolor="#85B7EB", edgecolor="none", alpha=0.2))
    ax2.add_collection3d(Poly3DCollection([t + np.array([0, 0, 6]) for t in pt[:12]],
                                          facecolor="#D85A30", edgecolor="none"))
    ax2.add_collection3d(Poly3DCollection([t + np.array([0, 0, 16]) for t in pt[12:24]],
                                          facecolor="#EF9F27", edgecolor="none", alpha=0.8))
    ax2.add_collection3d(Poly3DCollection([t + np.array([0, 0, 22]) for t in pt[24:36]],
                                          facecolor="#35B2C8", edgecolor="none", alpha=0.9))
    ax2.add_collection3d(Poly3DCollection([t + np.array([0, 0, G["BODY_H"] + 4]) for t in tt],
                                          facecolor="#B4B2A9", edgecolor="none", alpha=0.6))
    ax2.set_axis_off(); ax2.set_title("② 爆炸视图", fontsize=10.5, pad=2)
    ax2.view_init(26, -58); ax2.set_box_aspect((1, 1, 0.6))
    ax2.set_xlim(-R, R); ax2.set_ylim(-R, R); ax2.set_zlim(-6, ZT)

    # ③ 纵剖面（沿屏中心 X 剖；标注各段高度）
    ax3 = fig.add_subplot(1, 3, 3)
    ax3.set_aspect("equal"); ax3.set_axis_off()
    ax3.set_title("③ 剖面（沿屏中心线）", fontsize=10.5, pad=2)
    ax3.add_patch(Rectangle((-G["OUT_W"] / 2, 0), G["OUT_W"], G["BODY_H"], fill=False, ec="#185FA5", lw=1.3))
    ax3.add_patch(Rectangle((-G["IN_W"] / 2, P["floor"]), G["IN_W"], G["BODY_H"] - P["floor"],
                            fill=False, ec="#185FA5", lw=0.7, ls="--"))
    ax3.add_patch(Rectangle((-BOARD["w"] / 2, G["zB"]), BOARD["w"], BOARD["t"], fc="#F0997B", ec="#993C1D", lw=0.9))
    ax3.add_patch(Rectangle((L(G["ax0"], 0)[0], G["z_a0"]), ADAPT["w"], ASSUME["adapter_t"],
                            fc="#EF9F27", ec="#8a5a00", lw=0.9))
    ax3.add_patch(Rectangle((L(SCREEN["cx"] - SCREEN["w"] / 2, 0)[0], G["z_s0"]), SCREEN["w"], SCREEN["t"],
                            fc="#35B2C8", ec="#0e5f70", lw=0.9))
    ax3.add_patch(Rectangle((-G["OUT_W"] / 2, G["BODY_H"]), G["OUT_W"], P["lid_t"], fc="#D3D1C7", ec="#5F5E5A", lw=1.0))
    wx = L(SCREEN["cx"], 0)[0]
    ax3.add_patch(Rectangle((wx - WIN["w"] / 2, G["BODY_H"]), WIN["w"], P["lid_t"], fc="white", ec="#A32D2D", lw=0.9))
    for hx, hy in SCREWS:
        x = L(hx, hy)[0]
        ax3.add_patch(Rectangle((x - P["boss_od"] / 2, P["floor"]), P["boss_od"], ASSUME["bot_depth"],
                                fc="#B5D4F4", ec="#185FA5", lw=0.7))
        ax3.add_patch(Rectangle((x - P["screw_hole"] / 2, G["BODY_H"]), P["screw_hole"], P["lid_t"],
                                fc="white", ec="#0F6E56", lw=0.6))
    for zz, lab in ((G["zB"], "板底"), (G["z_a0"], "转接板底"), (G["z_s0"], "屏背面"), (G["z_s1"], "屏面"), (G["BODY_H"], "上盖底")):
        ax3.plot([-G["OUT_W"] / 2 * 0.9, G["OUT_W"] / 2 * 0.9], [zz] * 2, color="#888", lw=0.5, ls=":")
        ax3.annotate(f"{lab} {zz:.1f}", xy=(G["OUT_W"] * 0.32, zz), fontsize=7.5, color="#5F5E5A")
    ax3.text(0, G["TOTAL_H"] + 3, f"外廓 {G['OUT_W']:.1f}×{G['OUT_H']:.1f}×{G['TOTAL_H']:.1f}  "
                                  f"窗 {WIN['w']:.1f}×{WIN['h']:.1f}", ha="center", fontsize=8.5, color="#2c2c2a")
    ax3.set_xlim(-G["OUT_W"] * 0.62, G["OUT_W"] * 0.62)
    ax3.set_ylim(-3, G["TOTAL_H"] + 12)

    png = os.path.join(OUT, "case_v4_preview.png")
    fig.savefig(png, dpi=130, bbox_inches="tight")
    print(f"  out/case_v4_preview.png")
    print("=" * 62)
    print(f"完成（{time.time() - t0:.0f}s）")


if __name__ == "__main__":
    main()