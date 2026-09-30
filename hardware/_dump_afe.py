# -*- coding: utf-8 -*-
"""只读：dump AFE 页 器件/引脚/旗标/导线 几何 -> _review/afe_flags_live.json
用途：核对 U1A 输出节点 (R3.1,U1.1,U1.2) 为何被并入 +5V_A 网络；以及 R7.1 落点。
"""
import json
import sys
import urllib.request

BASE = "http://localhost:49620"

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


def main():
    # 1) 列页
    pages = json.loads(run(
        'const s = await eda.dmt_Schematic.getAllSchematicsInfo();'
        'return JSON.stringify(s.map(p=>({name:p.name,'
        'pages:(p.page||[]).map(q=>({name:q.name,uuid:q.uuid}))})));'
    ))
    print("PAGES =", json.dumps(pages, ensure_ascii=False))

    afe = None
    for sch in pages:
        for pg in sch.get("pages", []):
            if "AFE" in (pg.get("name") or ""):
                afe = pg
    if not afe:
        raise SystemExit("AFE page not found")

    js = (
        'await eda.dmt_EditorControl.openDocument("%s");'
        'await new Promise(r => setTimeout(r, 1800));'
        'const g = (o, fn) => { try { return fn(); } catch (e) { return null; } };'
        'const R = v => Math.round(v);'
        'const cur = await eda.dmt_SelectControl.getCurrentDocumentInfo();'
        'const out = { page: cur && cur.name, parts: [], flags: [], wires: [] };'
        'const ids = await eda.sch_PrimitiveComponent.getAllPrimitiveId();'
        'const cs = await eda.sch_PrimitiveComponent.get(ids);'
        'for (const c of (Array.isArray(cs) ? cs : [cs])) {'
        '  const des = g(c, () => c.getState_Designator());'
        '  const ct = c.componentType || g(c, () => c.getState_ComponentType());'
        '  const rec = { id: g(c, () => c.getState_PrimitiveId()), ct: ct,'
        '                x: R(c.getState_X()), y: R(c.getState_Y()),'
        '                net: g(c, () => c.getState_Net()),'
        '                name: g(c, () => c.getState_Name()) };'
        '  if (!des) { out.flags.push(rec); continue; }'
        '  rec.des = des; rec.pins = [];'
        '  try { const ps = await c.getAllPins();'
        '    for (const p of (ps || [])) {'
        '      rec.pins.push([String(p.getState_PinNumber()), R(p.getState_X()), R(p.getState_Y())]);'
        '    } } catch (e) {}'
        '  out.parts.push(rec);'
        '}'
        'const wids = await eda.sch_PrimitiveWire.getAllPrimitiveId();'
        'for (const id of (wids || [])) {'
        '  const w = await eda.sch_PrimitiveWire.get(id);'
        '  const o = Array.isArray(w) ? w[0] : w;'
        '  const L = g(o, () => o.getState_Line()) || g(o, () => o.line) || [];'
        '  const P = [];'
        '  for (let i = 0; i + 1 < L.length; i += 2) P.push([R(L[i]), R(L[i + 1])]);'
        '  if (P.length) out.wires.push({ id: id, net: g(o, () => o.getState_Net()), P: P });'
        '}'
        'return JSON.stringify(out);'
    ) % afe["uuid"]

    data = json.loads(run(js))
    print("PAGE =", data.get("page"),
          "| parts:", len(data["parts"]), "| flags:", len(data["flags"]), "| wires:", len(data["wires"]))
    with open("_review/afe_flags_live.json", "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=1)
    print("saved -> _review/afe_flags_live.json")


if __name__ == "__main__":
    main()
