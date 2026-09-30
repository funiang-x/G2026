# -*- coding: utf-8 -*-
"""只读：从 AFE .esch2 源码里找 Value/Designator ATTR 记录模板（经 Designator ATTR 反查 parentId）。"""
import json
import sys

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

SRC = "_review/final-20260930/src_AFE_after_fix.esch2"
src = open(SRC, encoding="utf-8").read()
parts = src.split("|\n")

recs = []
for i, part in enumerate(parts):
    hdr, _, data = part.partition("||")
    try:
        h = json.loads(hdr)
        d = json.loads(data)
    except Exception:
        continue
    recs.append((i, h, d))

# 1) 打印一个 COMPONENT 记录原文（看字段名）
for i, h, d in recs:
    if h.get("type") == "COMPONENT":
        print("COMPONENT 样例:")
        print(json.dumps(h, ensure_ascii=False), "||", json.dumps(d, ensure_ascii=False)[:400])
        break

# 2) Designator ATTR: value -> parentId
des2pid = {}
for i, h, d in recs:
    if h.get("type") == "ATTR" and d.get("key") == "Designator":
        des2pid[str(d.get("value"))] = d.get("parentId")
print("\nDesignator 数:", len(des2pid))

# 3) 打印 C13 / C1 / R3 的全部 ATTR 原文
for want in ("C13", "C1", "C9"):
    pid = des2pid.get(want)
    print("\n===== %s  parentId=%s =====" % (want, pid))
    for i, h, d in recs:
        if h.get("type") == "ATTR" and d.get("parentId") == pid:
            print(json.dumps(h, ensure_ascii=False) + "||" + json.dumps(d, ensure_ascii=False))

# 4) 顺带看 COMPONENT 记录与 pid 的对照（C13 的 comp 记录原文）
pid13 = des2pid.get("C13")
for i, h, d in recs:
    if h.get("type") == "COMPONENT" and h.get("id") == pid13:
        print("\nC13 COMPONENT 原文:")
        print(json.dumps(h, ensure_ascii=False) + "||" + json.dumps(d, ensure_ascii=False))
