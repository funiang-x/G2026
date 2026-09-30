# -*- coding: utf-8 -*-
"""只读：提取题面 PDF 文本 -> 控制台。"""
import sys

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

from pypdf import PdfReader

path = sys.argv[1]
r = PdfReader(path)
print("PAGES =", len(r.pages))
for i, pg in enumerate(r.pages):
    print("=" * 30, "PAGE", i + 1, "=" * 30)
    print(pg.extract_text())
