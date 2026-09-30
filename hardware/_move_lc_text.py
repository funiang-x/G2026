# -*- coding: utf-8 -*-
"""把 ④ 滤波器的元件值清单（(950,830) 漂在页脚）挪到 ④ 块顶部 (968,1255)。"""
import json
import sys
import urllib.request

BASE = "http://localhost:49620"
AFE = "fe840e6f545a2753"
TID = "ea07c4d67ac90440"

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
    "await eda.dmt_EditorControl.openDocument(\"%s\");"
    "await new Promise(r=>setTimeout(r,1800));"
    "const t = await eda.sch_PrimitiveText.get([\"%s\"]);"
    "const o = Array.isArray(t)?t[0]:t;"
    "const g=(fn)=>{try{return fn();}catch(e){return 'ERR';}};"
    "const before = [g(()=>o.getState_X()), g(()=>o.getState_Y())];"
    "await eda.sch_PrimitiveText.modify(\"%s\", {x: 968, y: 1255});"
    "await new Promise(r=>setTimeout(r,600));"
    "const t2 = await eda.sch_PrimitiveText.get([\"%s\"]);"
    "const o2 = Array.isArray(t2)?t2[0]:t2;"
    "const after = [g(()=>o2.getState_X()), g(()=>o2.getState_Y())];"
    "return JSON.stringify({before: before, after: after});"
) % (AFE, TID, TID, TID)

print(run(CODE))
