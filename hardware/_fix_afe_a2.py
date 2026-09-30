# -*- coding: utf-8 -*-
"""A 的第二半：旧折线已删（上一步延迟生效），补建 3 条新线。
逻辑：先读导线总数，仅当 == 47（48-1）时才建，否则中止待查。
"""
import json
import sys
import urllib.request

BASE = "http://localhost:49620"
AFE_UUID = "fe840e6f545a2753"

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass


def run(code, timeout=300):
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
    "const log = [];"
    "const wids0 = (await eda.sch_PrimitiveWire.getAllPrimitiveId())||[];"
    "log.push([\"pre_wire_count\", wids0.length]);"
    "if (wids0.length === 47) {"
    "  const w1 = await eda.sch_PrimitiveWire.create([800,1050,880,1050], \"\");"
    "  const w2 = await eda.sch_PrimitiveWire.create([720,1060,690,1060,690,1140,830,1140,830,1050], \"\");"
    "  const w3 = await eda.sch_PrimitiveWire.create([760,1080,760,1100], \"\");"
    "  log.push([\"built\", !!w1, !!w2, !!w3]);"
    "  await new Promise(r=>setTimeout(r,1200));"
    "  const wids1 = (await eda.sch_PrimitiveWire.getAllPrimitiveId())||[];"
    "  log.push([\"post_wire_count\", wids1.length]);"
    "} else {"
    "  log.push([\"abort\", \"wire count unexpected: \" + wids0.length]);"
    "}"
    "return JSON.stringify(log);"
) % AFE_UUID


def main():
    print("A2 结果:", run(CODE))


if __name__ == "__main__":
    main()
