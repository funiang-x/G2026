# -*- coding: utf-8 -*-
"""断点续摆：按页清点已放件（按坐标对账），只补缺的，分块 ≤8 件防超时。"""
import json, urllib.request, time

BASE = "http://localhost:49620"
P_AFE = "fe840e6f545a2753"; P_PWR = "0c870c063661db6c"; P_INT = "63accad121e053cf"

import importlib.util
spec = importlib.util.spec_from_file_location("pp", r"C:/Users/funiang/Desktop/Project/projects/G2026/hardware/place_parts.py")
pp = importlib.util.module_from_spec(spec)
import sys
sys.modules["pp"] = pp
spec.loader.exec_module(pp)  # 复用它的 M 定义（会重新生成 body 文件，无害）
M = pp.M

def exec_js(code, timeout=60):
    req = urllib.request.Request(BASE + "/execute", data=json.dumps({"code": code}).encode(),
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read())

def page_comps(page):
    js = f'await eda.dmt_EditorControl.openDocument("{page}"); const ids = await eda.sch_PrimitiveComponent.getAllPrimitiveId(); const cs = await eda.sch_PrimitiveComponent.get(ids); return cs.map(c => ({{id: c.primitiveId, x: c.x, y: c.y}}));'
    return exec_js(js)["result"]

def chunks(lst, n):
    for i in range(0, len(lst), n):
        yield lst[i:i+n]

report = {}
for page in [P_AFE, P_PWR, P_INT]:
    placed = page_comps(page)
    have = {(c["x"], c["y"]) for c in placed}
    missing = [it for it in M if it[4] == page and (it[5], it[6]) not in have]
    report[page] = {"placed_on_page": len(placed), "missing": [m[0] for m in missing]}
    for chunk in chunks(missing, 8):
        arr = [{"k": k, "q": q, "m": m, "not": nt, "x": x, "y": y, "rot": rot} for (k, q, m, nt, pg, x, y, rot) in chunk]
        js = ("const M = " + json.dumps(arr, ensure_ascii=False) + "; const out=[];"
              "for (const it of M) { const r = await eda.lib_Device.search(it.q);"
              " let cand = (r||[]);"
              " if (it.m) cand = cand.filter(d => (d.name||'').includes(it.m));"
              " if (it.not) cand = cand.filter(d => !(d.name||'').includes(it.not));"
              " if (!cand.length) { out.push({k: it.k, err: 'NOT_FOUND'}); continue; }"
              " const c = await eda.sch_PrimitiveComponent.create(cand[0], it.x, it.y, '', it.rot, false, true, true);"
              " out.push({k: it.k, id: c ? c.primitiveId : null, n: (cand[0].name||'').slice(0,30)}); } return out;")
        res = exec_js(js)
        time.sleep(0.5)
    # 复核
    placed2 = page_comps(page)
    have2 = {(c["x"], c["y"]) for c in placed2}
    still = [m[0] for m in M if m[4] == page and (m[5], m[6]) not in have2]
    report[page]["after"] = len(placed2)
    report[page]["still_missing"] = still

print(json.dumps(report, ensure_ascii=False, indent=1))
