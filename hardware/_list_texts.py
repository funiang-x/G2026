# -*- coding: utf-8 -*-
"""只读：列出 AFE 页所有 TEXT 图元（内容 + 坐标 + 字号），用于排查注释重叠。"""
import json
import sys

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

SRC = sys.argv[1] if len(sys.argv) > 1 else "_view/AFE.esch2"
src = open(SRC, encoding="utf-8").read()
texts = []
for part in src.split("|\n"):
    hdr, _, data = part.partition("||")
    try:
        h = json.loads(hdr)
        d = json.loads(data)
    except Exception:
        continue
    if h.get("type") != "TEXT":
        continue
    v = (d.get("value") or "").replace("\n", "\\n")
    texts.append((d.get("x"), d.get("y"), d.get("fontSize"), h.get("id"), v))

# TEXT 的 y 在源码里是负值；转为"页面坐标"便于和几何 dump 对齐
print("共 %d 条 TEXT（按 x 排序）" % len(texts))
for x, y, fs, tid, v in sorted(texts, key=lambda t: (t[0] if t[0] is not None else 0)):
    py = -y if isinstance(y, (int, float)) else y
    print("x=%-6s y=%-6s fs=%-3s id=%s | %s" % (x, py, fs, tid, v[:90]))
