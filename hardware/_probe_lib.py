# -*- coding: utf-8 -*-
"""只读：查库器件 CC0805KRX7R9BB104（100nF 0805 X7R）的 uuid 与权威 Value。"""
import json
import sys
import urllib.request

BASE = "http://localhost:49620"

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass


def run(code, timeout=120):
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
    "const r = await eda.lib_Device.search(\"CC0805KRX7R9BB104\");"
    "if (!r || !r.length) return JSON.stringify({empty: true});"
    "const c = r[0];"
    "const out = { keys: Object.keys(c), name: c.name, uuid: c.uuid,"
    "  libraryUuid: c.libraryUuid, libraryType: c.libraryType,"
    "  footprintName: c.footprintName, symbolName: c.symbolName,"
    "  description: c.description };"
    "const g = await eda.lib_Device.get([c.uuid]);"
    "const gg = Array.isArray(g) ? g[0] : g;"
    "out.getKeys = gg ? Object.keys(gg) : null;"
    "const p = (gg && gg.property) || {};"
    "out.otherProperty = p.otherProperty || null;"
    "return JSON.stringify(out);"
)

print(run(CODE))
