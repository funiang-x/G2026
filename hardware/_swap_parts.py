# -*- coding: utf-8 -*-
"""Phase 2b：按坐标原位替换错料。换件后若引脚几何变化，自动把网络端口迁到新端点。"""
import json, sys
sys.path.insert(0, r"C:/Users/funiang/Desktop/Project/projects/G2026/hardware")
from _eda import run

LIB = "0819f05c4eef4c71ace90d822a990e87"
P_AFE = "fe840e6f545a2753"; P_PWR = "0c870c063661db6c"

# page, x, y, 新器件搜索词, 说明
JOBS = [
    (P_AFE, 200, 950,  "SRV05-4_C338781",     "D1  TVS  -> SRV05-4 (Cj~1.5pF)"),
    (P_AFE, 1020, 1050, "XRCD54-4R7M",         "L1  3.9uH -> 4.7uH"),
    (P_AFE, 1260, 1050, "CD54-220M",           "L3  15uH  -> 22uH"),
    (P_AFE, 1500, 1050, "CD54-220M",           "L5  18uH  -> 22uH"),
    (P_AFE, 1140, 950,  "0805CG562J500NT",     "C2  4.3nF U2J -> 5.6nF C0G"),
    (P_AFE, 1380, 950,  "0805CG103J500NT",     "C4  7.5nF X8R -> 10nF C0G"),
    (P_AFE, 1620, 950,  "0805CG103J500NT",     "C6  6.8nF X7R -> 10nF C0G"),
    (P_AFE, 1860, 950,  "0805CG182J500NT",     "C8  1.6nF 0402 -> 1.8nF C0G 0805"),
    (P_AFE, 2400, 1050, "0805W8F1000T5E",      "R8  1k -> 100R"),
    (P_PWR, 1330, 1000, "1206W4F390JT5E",      "R14 330mR -> 39R"),
]

JS = r'''
await eda.dmt_EditorControl.openDocument("%s");
const all = await eda.sch_PrimitiveComponent.getAll();
let old = null;
for (const c of all) {
  if (c.getState_ComponentType() === "part" && c.getState_X() === %d && c.getState_Y() === %d) old = c;
}
if (!old) return {err: "target not found"};
const x = old.getState_X(), y = old.getState_Y(), r = old.getState_Rotation();
const des = old.getState_Designator() || null;
const op = old.getState_OtherProperty() || {};
const oldVal = op["Value"] || op["LCSC Part Name"] || null;
const oldPins = (await old.getAllPins()).map(p => ({n: p.getState_PinNumber(), x: p.getState_X(), y: p.getState_Y()}));
const oldEnds = oldPins.map(p => p.x + "," + p.y);

const r0 = await eda.lib_Device.search(%s);
let cand = (r0 || []);
if (!cand.length) return {err: "search empty", q: %s};
const nc = await eda.sch_PrimitiveComponent.create(cand[0], x, y, "", r, false, true, true);
if (!nc) return {err: "create failed"};
await eda.sch_PrimitiveComponent.modify(nc.getState_PrimitiveId(), {designator: des});
await eda.sch_PrimitiveComponent.delete(old.getState_PrimitiveId());

const newPins = (await nc.getAllPins()).map(p => ({n: p.getState_PinNumber(), x: p.getState_X(), y: p.getState_Y()}));
const newEnds = newPins.map(p => p.x + "," + p.y);

const all2 = await eda.sch_PrimitiveComponent.getAll();
const moved = [];
for (const c of all2) {
  const ct = c.getState_ComponentType();
  if (ct !== "netport" && ct !== "netflag") continue;
  const px = c.getState_X(), py = c.getState_Y();
  const k = px + "," + py;
  const idx = oldEnds.indexOf(k);
  if (idx < 0) continue;
  const target = newEnds[idx];
  if (!target || target === k) continue;
  const t = target.split(",").map(Number);
  const a = c.toAsync(); a.setState_X(t[0]); a.setState_Y(t[1]); await a.done();
  moved.push({net: c.getState_Net() || null, from: k, to: target});
}
const np2 = nc.getState_OtherProperty() || {};
return {des: des, oldVal: oldVal, newName: (cand[0].name || null),
        newVal: np2["Value"] || np2["LCSC Part Name"] || null,
        newTC: np2["Temperature Coefficient"] || null,
        newFP: np2["Supplier Footprint"] || null,
        pinGeomChanged: JSON.stringify(oldEnds) !== JSON.stringify(newEnds),
        portsMoved: moved};
'''

for pg, x, y, q, desc in JOBS:
    try:
        r = run(JS % (pg, x, y, json.dumps(q, ensure_ascii=False), json.dumps(q, ensure_ascii=False)), timeout=120)
    except Exception as e:
        print(f"[FAIL] {desc}: {e}"); continue
    if r.get("err"):
        print(f"[FAIL] {desc}: {r}"); continue
    print(f"[OK] {desc}")
    print(f"      {r['des']}: {str(r['oldVal'])[:30]}  ->  {str(r['newName'])[:40]} | val={r['newVal']} tc={r['newTC']} fp={r['newFP']}")
    if r["pinGeomChanged"]:
        print(f"      引脚几何变化，迁移端口 {len(r['portsMoved'])} 个: {r['portsMoved'][:6]}")
