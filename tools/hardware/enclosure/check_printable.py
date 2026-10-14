# -*- coding: utf-8 -*-
"""校验 out/ 导出的 STL 可打印性（封闭性、法线、尺寸、退化面）—— v4 版（相对 v3 只改路径）。"""

import os
import sys

import numpy as np
import trimesh

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "out")
PLATE = 180.0  # 拓竹 A1 mini 打印板

files = [f for f in ("case_bottom.stl", "case_top.stl") if os.path.exists(os.path.join(OUT, f))]
if not files:
    print("未找到 STL，请先运行 build_case_v4.py")
    sys.exit(1)

ok_all = True
for fn in files:
    m = trimesh.load(os.path.join(OUT, fn), force="mesh")
    vol = m.volume / 1000.0
    ext = m.extents
    uniq, counts = np.unique(m.edges_sorted, axis=0, return_counts=True)
    boundary = int((counts == 1).sum())
    nonmanifold = int((counts > 2).sum())
    degenerate = int((m.area_faces < 1e-9).sum())

    ok = (bool(m.is_watertight) and bool(m.is_winding_consistent)
          and boundary == 0 and nonmanifold == 0 and max(ext[0], ext[1]) <= PLATE)
    ok_all = ok_all and ok

    print(f"── {fn}")
    print(f"   三角面数 {len(m.faces)} · 包围盒 {ext[0]:.2f} × {ext[1]:.2f} × {ext[2]:.2f} mm · 体积 {vol:.2f} cm³")
    print(f"   封闭 {m.is_watertight} · 法线一致 {m.is_winding_consistent} · "
          f"边界边 {boundary} · 非流形边 {nonmanifold} · 退化面 {degenerate}")
    print(f"   结论 {'✓ 可直接切片打印' if ok else '✗ 需修正'}\n")

print("总体：" + ("✓ 两个件都可直接导入 Bambu Studio 切片" if ok_all else "✗ 存在需修正项"))
sys.exit(0 if ok_all else 1)