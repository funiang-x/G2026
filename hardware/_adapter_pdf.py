# -*- coding: utf-8 -*-
"""带坐标提取转接板 PDF 文本，便于按几何邻近还原连接。"""
import sys
from pypdf import PdfReader

P = r"c:\Users\funiang\Desktop\Project\projects\G2026\docs\TK035F5687_9629_整理资料_(含SPI及并口版)\05_硬件转接板资料\通用亮度转接板原理图及PCB\TK035F5501_转接板.pdf"

items = []
def visitor(text, cm, tm, font_dict, font_size):
    t = (text or "").strip()
    if t:
        items.append((round(tm[4], 1), round(tm[5], 1), t))

r = PdfReader(P)
for pi, page in enumerate(r.pages):
    items.clear()
    page.extract_text(visitor_text=visitor)
    out = [f"===== PAGE {pi+1} ({len(items)} items) ====="]
    for x, y, t in sorted(items, key=lambda z: (-z[1], z[0])):
        out.append(f"{x:8.1f} {y:8.1f}  {t}")
    fn = f"_adapter_pdf_p{pi+1}.txt"
    open(fn, "w", encoding="utf-8").write("\n".join(out))
    print("wrote", fn, len(items))
