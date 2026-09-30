# -*- coding: utf-8 -*-
"""只读：列出 AFE 页所有分区框（RECTANGLE）的坐标/尺寸/颜色。"""
import json
import sys
import urllib.request

BASE = "http://localhost:49620"
AFE = "fe840e6f545a2753"

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass


def run(code, timeout=180):
    req = urllib.request.Request(
        BASE + "/execute",
        data=json.dumps({"code": code}).encode("utf-8"),
        headers={"Content-Type": "application/json"},
    )
    with urllib.request.urlopen(req, timeout=timeout) as r:
        resp = json.loads(r.read().decode("utf-8"))
    if not resp.get("success"):
        raise RuntimeError(resp.get("error"))
    return resp["result"]


CODE = (
    "const g=(o,fn)=>{try{return fn();}catch(e){return null;}};"
    "await eda.dmt_EditorControl.openDocument(\"%s\");"
    "await new Promise(r=>setTimeout(r,1800));"
    "const rs = await eda.sch_PrimitiveRectangle.getAll();"
    "const out = [];"
    "for (const r of (rs||[])) {"
    "  out.push({id: r.primitiveId, x: g(r,()=>r.getState_X()), y: g(r,()=>r.getState_Y()),"
    "    w: g(r,()=>r.getState_Width()), h: g(r,()=>r.getState_Height()),"
    "    rot: g(r,()=>r.getState_Rotation()), color: g(r,()=>r.getState_Color()),"
    "    keys: Object.keys(r).slice(0,14)});"
    "}"
    "return JSON.stringify(out);"
) % AFE

print(run(CODE))
