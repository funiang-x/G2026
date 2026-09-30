# -*- coding: utf-8 -*-
"""回读核对：源码解析(port/net) + 实时引脚 dump + 端点几何匹配 → 逐网核对。
输出：report_check.md；差异=0 才算通过。"""
import json, urllib.request, importlib.util, sys, re
from collections import defaultdict

spec = importlib.util.spec_from_file_location("pp", r"C:/Users/funiang/Desktop/Project/projects/G2026/hardware/place_parts.py")
pp = importlib.util.module_from_spec(spec); sys.modules["pp"] = pp; spec.loader.exec_module(pp)
BASE = "http://localhost:49620"
HW = r"C:/Users/funiang/Desktop/Project/projects/G2026/hardware"
def ex(code):
    req = urllib.request.Request(BASE+"/execute", data=json.dumps({"code":code}).encode(), headers={"Content-Type":"application/json"})
    return json.loads(urllib.request.urlopen(req, timeout=110).read())

PAGES = {"AFE": "fe840e6f545a2753", "PWR": "0c870c063661db6c", "INT": "63accad121e053cf"}
KEYPOS = {}
for (k,q,m,nt,pg,x,y,rot) in pp.M: KEYPOS[(pg,x,y)] = k
KEYPOS[("63accad121e053cf",750,1040)] = "EVK1"
KEYPOS[("63accad121e053cf",900,1250)] = "h4"
KEYPOS[("63accad121e053cf",1450,1300)] = "j2"
KEYPOS[("fe840e6f545a2753",760,1400)] = "u2b"

def endpoint(p):
    d = {0:(1,0), 90:(0,1), 180:(-1,0), 270:(0,-1)}[p["rot"] % 360]
    return (p["x"] + d[0]*p["len"], p["y"] + d[1]*p["len"])

# ---- 0. 先解析源码（端口/旗 id 集合，用于排除） ----
SRC_IDS = {}; SRC_NETS = []
for name in PAGES:
    txt = open(rf"{HW}/source_{name}.txt", encoding="utf-8").read()
    comps = {}; pid2net = {}
    for rec in txt.replace("||","").split("|"):
        rec = rec.strip()
        if not rec or "" not in rec: continue
        try:
            hs, ds = rec.split("",1); h = json.loads(hs); d = json.loads(ds)
            if h.get("type")=="COMPONENT": comps[h["id"]] = (d.get("partId",""), d.get("x"), d.get("y"))
            elif h.get("type")=="ATTR" and d.get("key")=="Name" and d.get("value"): pid2net[d["parentId"]] = d["value"]
        except Exception: pass
    SRC_IDS[name] = set(cid for cid,(pid,x,y) in comps.items() if pid.startswith("pid") and cid in pid2net)
    for cid, net in pid2net.items():
        SRC_NETS.append((name, net, comps[cid][1], -comps[cid][2]))

# ---- 1. 实时 dump：每页 comps + pins ----
page_pins = {}   # page -> {key: [(num,name,ex,ey)]}
for name, pg in PAGES.items():
    ex(f'await eda.dmt_EditorControl.openDocument("{pg}");')
    comps = ex('const ids = await eda.sch_PrimitiveComponent.getAllPrimitiveId(); const cs = await eda.sch_PrimitiveComponent.get(ids); return cs.map(c => ({id: c.primitiveId, x: c.x, y: c.y}));')["result"]
    pinjs = 'const ids = ' + json.dumps([c["id"] for c in comps]) + '; const out = []; for (const id of ids) { const pins = await eda.sch_PrimitiveComponent.getAllPinsByPrimitiveId(id); out.push({id: id, pins: (pins||[]).map(p => ({num: p.pinNumber, name: p.pinName, x: p.x, y: p.y, rot: p.rotation, len: p.pinLength}))}); } return out;'
    pinmap = {p["id"]: p["pins"] for p in ex(pinjs)["result"]}
    kp = {}
    for c in comps:
        if c["id"] in SRC_IDS[name]: continue
        k = KEYPOS.get((pg, c["x"], c["y"]))
        if k: kp[k] = [(p["num"], p["name"], *endpoint(p)) for p in pinmap.get(c["id"], [])]
    page_pins[name] = kp

# ---- 2. 解析源码：ports/netflags (net, x, y, page) ----
ports = SRC_NETS

# ---- 3. 端点 → 网名（几何匹配 ±1） ----
end_net = defaultdict(set)   # (page, x, y) -> {net}
for (pg, net, x, y) in ports:
    end_net[(pg, x, y)].add(net)

def net_of(pg, x, y):
    hits = set()
    for (p2, nx, ny), nets in end_net.items():
        if p2 == pg and abs(nx-x) <= 1 and abs(ny-y) <= 1: hits |= nets
    return hits

# ---- 4. 期望网表 ----
E = defaultdict(list)  # net -> [(page, key, selector)]
def addV(net, pg, key, top=None, bot=None):
    ps = sorted(page_pins[pg][key], key=lambda p: -p[3])
    if top: E[net].append((pg, key, "pin%s" % ps[0][0]))
    if bot: E[net].append((pg, key, "pin%s" % ps[1][0]))
def addH(net, pg, key, left=None, right=None):
    ps = sorted(page_pins[pg][key], key=lambda p: p[2])
    if left: E[net].append((pg, key, "pin%s" % ps[0][0]))
    if right: E[net].append((pg, key, "pin%s" % ps[1][0]))
def addN(net, pg, key, name):
    for p in page_pins[pg][key]:
        if (p[1] or "").upper().replace(" ","") == name: E[net].append((pg, key, "pin%s" % p[0]))
def addNum(net, pg, key, num):
    E[net].append((pg, key, "pin%s" % num))

AG, P5A, P33, G5, GND = "AGND", "+5V_A", "+3V3", "+5V", "GND"
# AFE
addV("AFE_TERM","AFE","tvs",top=1); addV("AFE_TERM","AFE","rt1",top=1); addN("AFE_TERM","AFE","bnc","5"); addNum("AFE_TERM","AFE","tp1",1)
addH("AFE_B1_AC","AFE","ca1",right=1); addH("AFE_B1_AC","AFE","jp3",left=1)
addH("AFE_B1_IN","AFE","jp3",right=1); addV("AFE_B1_IN","AFE","rb1",top=1); addN("AFE_B1_IN","AFE","u2","INA+"); addNum("AFE_B1_IN","AFE","tp2",1)
addN("AFE_G1_OUT","AFE","u2","OUTA"); addN("AFE_G1_OUT","AFE","u2","INA-"); addH("AFE_G1_OUT","AFE","rs1",left=1)
addH("F1","AFE","rs1",right=1); addH("F1","AFE","l1",left=1)
addH("F2","AFE","l1",right=1); addV("F2","AFE","c1",top=1); addH("F2","AFE","l2",left=1)
addH("F3","AFE","l2",right=1); addV("F3","AFE","c2",top=1); addH("F3","AFE","l3",left=1)
addH("F4","AFE","l3",right=1); addV("F4","AFE","c3",top=1); addH("F4","AFE","l4",left=1)
addH("F5","AFE","l4",right=1); addV("F5","AFE","c4",top=1); addV("F5","AFE","rl",top=1); addH("F5","AFE","ca2",left=1); addNum("F5","AFE","tp3",1)
addH("AFE_G2_IN","AFE","ca2",right=1); addV("AFE_G2_IN","AFE","rb2",top=1); addN("AFE_G2_IN","AFE","u2b","INB+")
addN("AFE_G2_FB","AFE","u2b","INB-"); addH("AFE_G2_FB","AFE","rf",right=1); addV("AFE_G2_FB","AFE","rg",top=1)
addN("AFE_ADC_OUT","AFE","u2b","OUTB"); addH("AFE_ADC_OUT","AFE","rf",left=1); addH("AFE_ADC_OUT","AFE","r100",left=1)
addH("AFE_ADC_MON","AFE","r100",right=1); addV("AFE_ADC_MON","AFE","c470",top=1); addNum("AFE_ADC_MON","AFE","tp4",1)
addH("AFE_ADC_IN","AFE","jp1",right=1); addNum("AFE_ADC_IN","AFE","j3",4)
addN("VREF_3V3","AFE","u4","OUT"); addV("VREF_3V3","AFE","cout",top=1); addV("VREF_3V3","AFE","rdiv1",top=1); addV("VREF_3V3","AFE","jp2",top=1); addNum("VREF_3V3","AFE","tp6",1)
addV("VBIAS_1V65","AFE","rdiv1",bot=1); addV("VBIAS_1V65","AFE","rdiv2",top=1); addV("VBIAS_1V65","AFE","cb",top=1)
addV("VBIAS_1V65","AFE","rb1",bot=1); addV("VBIAS_1V65","AFE","rb2",bot=1); addNum("VBIAS_1V65","AFE","j3",5)
addV("VREF_3V3_EXT","AFE","jp2",bot=1); addNum("VREF_3V3_EXT","AFE","j3",6)
addN(P5A,"AFE","u2","V+"); addN(P5A,"AFE","u2b","V+"); addN(P5A,"AFE","u4","IN"); addV(P5A,"AFE","cin",top=1); addNum(P5A,"AFE","j3",1)
addN(AG,"AFE","u2","V-"); addN(AG,"AFE","u4","GND"); addV(AG,"AFE","cin",bot=1)
addV(AG,"AFE","tvs",bot=1); addV(AG,"AFE","rt1",bot=1)
for k in ["c1","c2","c3","c4","rl","rg","c470","rdiv2","cb"]: addV(AG,"AFE",k,bot=1)
addNum(AG,"AFE","j3",2); addNum(AG,"AFE","j3",3)
# PWR
addH(G5,"PWR","d1",left=1); addNum("+5V_RAW","PWR","j1",1); addN("+5V_RAW","PWR","d1","A")
addN(G5,"PWR","u1","VIN"); addV(G5,"PWR","cin1",top=1); addH(G5,"PWR","rbl",left=1)
addN(P33,"PWR","u1","VOUT"); addV(P33,"PWR","cout1",top=1); addV(P33,"PWR","c100a",top=1); addV(P33,"PWR","r1k",bot=1); addV(P33,"PWR","rim1",bot=1); addV("IM1","PWR","rim1",top=1)
addN(GND,"PWR","u1","GND"); addV(GND,"PWR","cin1",bot=1); addV(GND,"PWR","cout1",bot=1); addV(GND,"PWR","c100a",bot=1)
addNum(GND,"PWR","j1",2); addNum(GND,"PWR","j1",3)
addN("LED_A","PWR","led1","+"); addV("LED_A","PWR","r1k",top=1)
addN(GND,"PWR","led1","-")
addN("Q1_G","PWR","q1","G"); addH("Q1_G","PWR","rgate",right=1); addV("Q1_G","PWR","rgpd",top=1)
addH("LCD_BL_CTL","PWR","rgate",left=1); addV("Q1_G","PWR","rgpd",top=1); addV(GND,"PWR","rgpd",bot=1)
addN("BL_K","PWR","q1","D")
addN(GND,"PWR","q1","S")
addV(P33,"PWR","ri2ca",top=1); addV(P33,"PWR","ri2cb",top=1)
addV("IM0","PWR","rim0",top=1); addV("IM2","PWR","rim2",top=1)
addV(GND,"PWR","rim0",bot=1); addV(GND,"PWR","rim2",bot=1)
addV("LCD_ID","PWR","rid",top=1); addV(GND,"PWR","rid",bot=1)
addV(GND,"PWR","c100b",bot=1); addV(GND,"PWR","c100c",bot=1)
# INT: LCD/触摸/板间
LCD = {"36":"LCD_D0","35":"LCD_D1","72":"LCD_D2","71":"LCD_D3","13":"LCD_D4","16":"LCD_D5","15":"LCD_D6",
       "18":"LCD_D7","17":"LCD_D8","20":"LCD_D9","19":"LCD_D10","22":"LCD_D11","21":"LCD_D12",
       "30":"LCD_D13","29":"LCD_D14","32":"LCD_D15","70":"LCD_RD","69":"LCD_WR","67":"LCD_CS","31":"LCD_RS","14":"LCD_RST"}
H3D = {"17":"LCD_D15","18":"LCD_D14","19":"LCD_D13","20":"LCD_D12","21":"LCD_D11","22":"LCD_D10","23":"LCD_D9",
       "24":"LCD_D8","25":"LCD_D7","26":"LCD_D6","27":"LCD_D5","28":"LCD_D4","29":"LCD_D3","30":"LCD_D2",
       "33":"LCD_RST","34":"LCD_RD","35":"LCD_WR","36":"LCD_RS","37":"LCD_CS"}
for pos, net in LCD.items():
    addNum(net,"INT","EVK1",pos)
    h3n = [k for k,v in H3D.items() if v == net]
    if h3n: addNum(net,"INT","h3",h3n[0])
addNum("LCD_BL_CTL","INT","EVK1",11); addNum("LCD_RST","INT","EVK1",14)
for pos in ["9"]: addNum("TP_RST","INT","EVK1",pos)
for pos in ["12"]: addNum("TP_INT","INT","EVK1",pos)
addNum("TP_SCL","INT","EVK1",23); addNum("TP_SCL","INT","h4",3)
addNum("TP_SDA","INT","EVK1",24); addNum("TP_SDA","INT","h4",4)
addNum("TP_RST","INT","h4",5); addNum("TP_INT","INT","h4",6)
addNum("IM0","INT","h3",8); addNum("IM1","INT","h3",9); addNum("IM2","INT","h3",10)
addNum("BL_A","INT","h3",44); addNum("LCD_ID","INT","h3",45)
for n in range(38,44): addNum("BL_K","INT","h3",str(n))
addNum("VREF_3V3_EXT","INT","EVK1",2); addNum("VREF_3V3_EXT","INT","j2",6)
addNum("AFE_ADC_IN","INT","EVK1",3); addNum("AFE_ADC_IN","INT","j2",4)
addNum("VBIAS_1V65","INT","EVK1",4); addNum("VBIAS_1V65","INT","j2",5)
addNum("AGND","INT","EVK1",37); addNum(AG,"INT","j2",2); addNum(AG,"INT","j2",3)
addNum(P5A,"INT","j2",1); addNum(G5,"INT","EVK1",40); addNum(G5,"INT","EVK1",80)
addNum(P33,"INT","EVK1",38); addNum(P33,"INT","EVK1",78)
addNum(GND,"INT","EVK1",39); addNum(GND,"INT","EVK1",77); addNum(GND,"INT","EVK1",79)
addH("BL_A","PWR","rbl",right=1)

# ---- 5. 核对 ----
ok, bad, floats = 0, [], []
for net, members in E.items():
    for (pg, key, sel) in members:
        pin = next((p for p in page_pins[pg].get(key, []) if "pin%s" % p[0] == sel), None)
        if pin is None:
            bad.append((net, pg, key, sel, "PIN_MISSING")); continue
        nets = net_of(pg, pin[2], pin[3])
        if net in nets: ok += 1
        else: bad.append((net, pg, key, sel, "NO_MATCH:%s" % (sorted(nets) if nets else "FLOAT")))
# 悬空端口
dang = []
for (pg, net, x, y) in ports:
    touched = False
    for key, pins in page_pins[pg].items():
        for p in pins:
            if abs(p[2]-x) <= 1 and abs(p[3]-y) <= 1: touched = True; break
        if touched: break
    if not touched: dang.append((pg, net, x, y))

rep = []
rep.append("# 阶段三 · 回读核对报告（几何法，2026-09-28）\n")
rep.append("- 期望核对点：%d，通过：%d，失败：%d" % (ok+len(bad), ok, len(bad)))
rep.append("- 画布端口/旗总数：%d（AFE/PWR/INT = %d/%d/%d）" % (len(ports),
    sum(1 for p in ports if p[0]=="AFE"), sum(1 for p in ports if p[0]=="PWR"), sum(1 for p in ports if p[0]=="INT")))
rep.append("- 悬空端口（不接触任何引脚）：%d %s" % (len(dang), dang[:8] if dang else ""))
if bad:
    rep.append("\n## 失败清单")
    for b in bad: rep.append("- %s：%s.%s %s → %s" % (b[0], b[1], b[2], b[3], b[4]))
else:
    rep.append("\n**差异 = 0 ✓（全部期望连接在画布上成立）**")
rep.append("\n> 核对方式：独立于绘图计划，从画布实读元件/引脚/端口坐标重建连通；"
           "网络名读自文档源码 ATTR。网络表软件导出（getNetlist）受桥接 30s 超时限制，以本几何法代替。")
open(rf"{HW}/report_check.md", "w", encoding="utf-8").write("\n".join(rep))
print("\n".join(rep[:12]))
