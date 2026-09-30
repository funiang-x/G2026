# -*- coding: utf-8 -*-
"""全工程合并的原理图 DRC（跨页按网名合并，消除按页跑的误报）。
   规则：① 单端网络 ② 悬空引脚 ③ 位号重复/缺失 ④ 同网络多网名(短路) ⑤ 无封装 ⑥ 同点重复端口"""
import json, sys, os, collections
sys.path.insert(0, r"C:/Users/funiang/Desktop/Project/projects/G2026/hardware")
from _eda import run

REV = r"C:/Users/funiang/Desktop/Project/projects/G2026/hardware/_review"
PAGES = {"fe840e6f545a2753": "AFE", "0c870c063661db6c": "PWR", "63accad121e053cf": "INT"}

API = r'''
await eda.dmt_EditorControl.openDocument("__PG__");
const all = await eda.sch_PrimitiveComponent.getAll();
const pins = [], ports = [], parts = [];
for (const c of all) {
  const ct = c.getState_ComponentType();
  const x = c.getState_X(), y = c.getState_Y();
  if (ct === "part") {
    const ps = await c.getAllPins();
    for (const p of (ps || [])) {
      let L = 0; try { L = p.getState_PinLength(); } catch (e) { L = 0; }
      let NC = false; try { NC = !!p.getState_NoConnected(); } catch (e) { NC = false; }
      const r = ((p.getState_Rotation() % 360) + 360) % 360;
      const d = ({0:[1,0],90:[0,1],180:[-1,0],270:[0,-1]})[r] || [0,0];
      pins.push({k: (p.getState_X()+d[0]*L) + "," + (p.getState_Y()+d[1]*L),
                 ref: (c.getState_Designator()||"?") + "." + p.getState_PinNumber(),
                 nc: NC});
    }
    let FP = false; try { FP = !!c.getState_Footprint(); } catch (e) { FP = false; }
    let PCB = true; try { PCB = !!c.getState_AddIntoPcb(); } catch (e) { PCB = true; }
    parts.push({d: c.getState_Designator() || "?", fp: FP, pcb: PCB, id: c.getState_PrimitiveId()});
  } else if (ct === "netport" || ct === "netflag") {
    const ps = await c.getAllPins();
    const pt = (ps && ps.length) ? [ps[0].getState_X(), ps[0].getState_Y()] : [x, y];
    ports.push({k: pt[0] + "," + pt[1], net: c.getState_Net() || null});
  }
}
return {pins: pins, ports: ports, parts: parts};
'''

SRC = r'''
await eda.dmt_EditorControl.openDocument("__PG__");
return await eda.sys_FileManager.getDocumentSource();
'''


def segs_of(src):
    out = []
    for line in src.splitlines():
        if "||" not in line:
            continue
        h, b = line.split("||", 1)
        h = h.rstrip("|"); b = b.rstrip("|")
        try:
            H = json.loads(h); B = json.loads(b)
        except Exception:
            continue
        if H.get("type") == "LINE":
            out.append(((B["startX"], -B["startY"]), (B["endX"], -B["endY"])))
    return out


class DSU:
    def __init__(self):
        self.p = {}

    def find(self, a):
        self.p.setdefault(a, a)
        r = a
        while self.p[r] != r:
            r = self.p[r]
        while self.p[a] != r:
            self.p[a], a = r, self.p[a]
        return r

    def union(self, a, b):
        ra, rb = self.find(a), self.find(b)
        if ra != rb:
            self.p[rb] = ra


def k(x, y):
    return f"{round(x,2)},{round(y,2)}"


dsu = DSU()
pins, ports, parts = [], [], []
for pg, nm in PAGES.items():
    api = run(API.replace("__PG__", pg), timeout=240)
    src = run(SRC.replace("__PG__", pg), timeout=150)
    for (a, b) in segs_of(src):
        dsu.union(k(*a), k(*b))
    for p in api["pins"]:
        p["pg"] = nm; pins.append(p)
    for p in api["ports"]:
        p["pg"] = nm; ports.append(p)
    for p in api["parts"]:
        p["pg"] = nm; parts.append(p)

# 跨页：同名端口合并
byname = collections.defaultdict(list)
for p in ports:
    if p["net"]:
        byname[p["net"]].append(dsu.find(p["k"]))
for net, roots in byname.items():
    for r in roots[1:]:
        dsu.union(roots[0], r)

pinnet = collections.defaultdict(list)
portnet = collections.defaultdict(list)
for p in pins:
    pinnet[dsu.find(p["k"])].append(p)
for p in ports:
    portnet[dsu.find(p["k"])].append(p)

issues = []
for r in set(pinnet) | set(portnet):
    ps = pinnet.get(r, [])
    prs = portnet.get(r, [])
    names = {q["net"] for q in prs if q["net"]}
    if len(names) > 1:
        issues.append(("E", "同网络多网名(短路)", f"{sorted(names)} 引脚={[p['ref'] for p in ps]}"))
    if len(ps) == 1:
        if names:
            issues.append(("W", "单端网络", f"{sorted(names)} 只有 {ps[0]['pg']}:{ps[0]['ref']}"))
        elif not ps[0]["nc"]:
            issues.append(("E", "悬空引脚(无NC标志)", f"{ps[0]['pg']}:{ps[0]['ref']} @{ps[0]['k']}"))

cnt = collections.Counter((x["pg"], x["d"]) for x in parts)
for (pg, d), n in cnt.items():
    if n > 1:
        issues.append(("E", "位号重复", f"{pg}:{d} × {n}"))
    if d in ("?", ""):
        issues.append(("E", "位号缺失", f"{pg}:{d}"))
for x in parts:
    if not x["fp"]:
        issues.append(("W", "无封装", f"{x['pg']}:{x['d']}"))
    if not x["pcb"]:
        issues.append(("W", "未转 PCB", f"{x['pg']}:{x['d']}"))

pc = collections.Counter(p["k"] for p in ports)
for kk, n in pc.items():
    if n > 1:
        nm2 = sorted({p["net"] for p in ports if p["k"] == kk})
        pg2 = sorted({p["pg"] for p in ports if p["k"] == kk})
        issues.append(("W", "同点重复端口", f"{pg2} {kk} × {n} {nm2}"))

print("=" * 92)
print("全工程 DRC（跨页合并后）")
print("统计:", dict(collections.Counter(t for (_, t, _) in issues)))
print()
for sev, t, msg in sorted(issues, key=lambda z: (z[1], z[2])):
    print(f"  [{sev}] {t:<20} {msg}")
json.dump(issues, open(os.path.join(REV, "drc_issues.json"), "w", encoding="utf-8"), ensure_ascii=False, indent=1)
