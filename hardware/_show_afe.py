# -*- coding: utf-8 -*-
"""只读：打印 afe_flags_live.json 的旗标 / 关键器件引脚 / 导线几何。"""
import json
import sys

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

d = json.load(open("_review/afe_flags_live.json", encoding="utf-8"))

print("=== FLAGS (net @ x,y) ===")
for f in sorted(d["flags"], key=lambda r: (r["net"] or "", r["x"])):
    print("  %-16s @ (%d,%d)  ct=%s name=%s id=%s" % (
        str(f["net"]), f["x"], f["y"], f["ct"], f["name"], f["id"]))

print()
print("=== PARTS of interest ===")
want = {"U1", "R2", "R3", "R5", "R6", "R7", "R8", "C1", "C9", "C11", "C12",
        "D1", "R1", "R9", "R10", "C13", "J1", "JP1", "JP2", "JP3", "C10"}
for p in d["parts"]:
    if p["des"] in want:
        print("  %-4s @ (%d,%d) %s" % (p["des"], p["x"], p["y"], p["name"]))
        for pin in p["pins"]:
            print("       pin %3s @ (%s,%s)" % (pin[0], pin[1], pin[2]))

print()
print("=== WIRES (net | points) ===")
for w in d["wires"]:
    print("  net=%-12s id=%s  %s" % (str(w["net"]), w["id"], w["P"]))
