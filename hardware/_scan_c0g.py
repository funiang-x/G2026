# -*- coding: utf-8 -*-
"""扫描立创库中实际存在的 C0G 贴片电容档位（0805CG / 1206CG 命名族）。"""
import json, sys, time
sys.path.insert(0, r"C:/Users/funiang/Desktop/Project/projects/G2026/hardware")
from _eda import run

MANT = ["152", "162", "182", "202", "222", "272", "332", "392", "432", "472",
        "512", "562", "622", "682", "752", "822", "912", "103", "123"]
JS = r'''
const r = await eda.lib_Device.search(%s);
return (r || []).slice(0, 4).map(d => d.name || null);
'''
found = {}
for pkg in ["0805CG", "1206CG"]:
    print("=" * 70)
    print("###", pkg)
    for m in MANT:
        q = pkg + m
        try:
            r = run(JS % json.dumps(q, ensure_ascii=False), timeout=45)
        except Exception:
            r = []
        hit = [x for x in r if x and x.startswith(pkg)]
        if hit:
            print(f"   {q:<12} -> {hit[0]}")
            found[q] = hit[0]
        time.sleep(0.1)
json.dump(found, open(r"C:/Users/funiang/Desktop/Project/projects/G2026/hardware/_review/c0g_avail.json", "w", encoding="utf-8"), ensure_ascii=False, indent=1)
