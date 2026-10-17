# -*- coding: utf-8 -*-
"""生成 EDA 批量落件的 JS + POST body；随后 dump 引脚。"""
import json

P_AFE = "fe840e6f545a2753"
P_PWR = "0c870c063661db6c"
P_INT = "63accad121e053cf"

# key, q(搜索词), m(名称匹配), not(排除), page, x, y, rot
M = [
 ("bnc","BY-BNC-50KWE",None,None,P_AFE,100,1050,0),
 ("tvs","PESD5V0S1BA","PESD5V0S1BA",None,P_AFE,200,950,90),
 ("rt1","0805W8F499","0805W8F499",None,P_AFE,280,950,90),
 ("ca1","1206CG104","1206CG104",None,P_AFE,400,1050,0),
 ("jp3","0805W8F0000","0805W8F0000",None,P_AFE,540,1050,0),
 ("rb1","0805W8F1002","0805W8F1002",None,P_AFE,620,1150,90),
 ("u2","OPA2197ID","OPA2197ID","GKR",P_AFE,760,1050,0),
 ("rs1","0805W8F499","0805W8F499",None,P_AFE,900,1050,0),
 ("l1","CD54-3R9","3R9",None,P_AFE,1020,1050,0),
 ("c1","4.3nF","C0603X432",None,P_AFE,1140,950,90),
 ("l2","CD54-150","150K",None,P_AFE,1260,1050,0),
 ("c2","7.5nF","X752",None,P_AFE,1380,950,90),
 ("l3","CD54-180","180M",None,P_AFE,1500,1050,0),
 ("c3","6.8nF","0805B682",None,P_AFE,1620,950,90),
 ("l4","CD54-120","120M",None,P_AFE,1740,1050,0),
 ("c4","1.6nF","C0402C162",None,P_AFE,1860,950,90),
 ("rl","0805W8F499","0805W8F499",None,P_AFE,1980,950,90),
 ("ca2","1206CG104","1206CG104",None,P_AFE,2100,1050,0),
 ("rb2","0805W8F1002","0805W8F1002",None,P_AFE,2180,1150,90),
 ("rf","0805W8F3002","3002",None,P_AFE,2260,1050,0),
 ("rg","0805W8F1002","0805W8F1002",None,P_AFE,2300,1250,90),
 ("r100","0805W8F1001","0805W8F1001",None,P_AFE,2400,1050,0),
 ("c470","CL10C471","CL10C471",None,P_AFE,2500,950,90),
 ("jp1","0805W8F0000","0805W8F0000",None,P_AFE,2600,1050,0),
 ("u4","REF3033","AIDBZT","RQ1",P_AFE,300,1250,0),
 ("cin","TAJB226K","025RNJ",None,P_AFE,180,1400,90),
 ("cout","TAJB226K","016RNJ",None,P_AFE,440,1400,90),
 ("rdiv1","0805W8F1002","0805W8F1002",None,P_AFE,560,1290,90),
 ("rdiv2","0805W8F1002","0805W8F1002",None,P_AFE,560,1170,90),
 ("cb","CL21A106","CL21A106",None,P_AFE,660,1230,90),
 ("jp2","0805W8F0000","0805W8F0000",None,P_AFE,760,1290,90),
 ("j3","W10006-206MG0ER1","W10006",None,P_AFE,2700,1050,0),
 ("tp1","test point","TP-",None,P_AFE,340,850,0),
 ("tp2","test point","TP-",None,P_AFE,760,850,0),
 ("tp3","test point","TP-",None,P_AFE,1860,850,0),
 ("tp4","test point","TP-",None,P_AFE,2500,850,0),
 ("tp5","test point","TP-",None,P_AFE,560,850,0),
 ("tp6","test point","TP-",None,P_AFE,760,1450,0),
 ("j1","DC005","DC005",None,P_PWR,150,1100,0),
 ("d1","SS34","SS34_C8678",None,P_PWR,320,1100,0),
 ("u1","AMS1117-3.3","AMS1117-3.3",None,P_PWR,550,1100,0),
 ("cin1","TAJB226K","025RNJ",None,P_PWR,470,1220,90),
 ("cout1","TAJB226K","016RNJ",None,P_PWR,660,1220,90),
 ("c100a","100nF 0805","CC0805KRX7R9BB104",None,P_PWR,760,1220,90),
 ("led1","LED 0805 red","KT-0805Y",None,P_PWR,950,1100,90),
 ("r1k","0805W8F1001","0805W8F1001",None,P_PWR,950,1000,90),
 ("q1","SI2302","SI2302",None,P_PWR,1150,1100,0),
 ("rgate","0805W8F1001","0805W8F1001",None,P_PWR,1090,1000,0),
 ("rgpd","0805W8F1003","0805W8F1003",None,P_PWR,1230,950,90),
 ("rbl","1206W4F330","1206W4F330",None,P_PWR,1330,1000,0),
 ("ri2ca","0805W8F4701","0805W8F4701",None,P_PWR,1450,1000,90),
 ("ri2cb","0805W8F4701","0805W8F4701",None,P_PWR,1560,1000,90),
 ("rim0","0805W8F0000","0805W8F0000",None,P_PWR,1680,1000,90),
 ("rim2","0805W8F0000","0805W8F0000",None,P_PWR,1790,1000,90),
 ("rim1","0805W8F0000","0805W8F0000",None,P_PWR,1900,1000,90),
 ("rid","0805W8F1003","0805W8F1003",None,P_PWR,2010,1000,90),
 ("c100b","100nF 0805","CC0805KRX7R9BB104",None,P_PWR,2120,1000,90),
 ("c100c","100nF 0805","CC0805KRX7R9BB104",None,P_PWR,2210,1000,90),
 ("h3","FH12-45","FH12-45S-0.5SVA",None,P_INT,700,1050,0),
 ("h4","FH12-6","FH12-6S-0.5SH",None,P_INT,700,1250,0),
 ("j2","W10006-206MG0ER1","W10006",None,P_INT,950,950,0),
]

def js_page(page):
    return f'await eda.dmt_EditorControl.openDocument("{page}");'

items_js = json.dumps([{"k":k,"q":q,"m":m,"not":nt,"x":x,"y":y,"rot":rot} for (k,q,m,nt,pg,x,y,rot) in M], ensure_ascii=False)

js = """
const M = %s;
const pageOf = {};
const order = [];
for (const it of M) { if (!pageOf[it.p]) { pageOf[it.p] = true; order.push(it.p); } }
const pages = %s;
const out = [];
for (const pg of pages) {
  await eda.dmt_EditorControl.openDocument(pg);
  for (const it of M.filter(i => i.p === pg)) {
    const r = await eda.lib_Device.search(it.q);
    let cand = (r || []);
    if (it.m) cand = cand.filter(d => (d.name || "").includes(it.m));
    if (it.not) cand = cand.filter(d => !(d.name || "").includes(it.not));
    if (!cand.length) { out.push({k: it.k, err: "NOT_FOUND"}); continue; }
    const c = await eda.sch_PrimitiveComponent.create(cand[0], it.x, it.y, "", it.rot, false, true, true);
    out.push({k: it.k, id: c ? c.primitiveId : null, n: (cand[0].name || "").slice(0, 32)});
  }
}
return out;
""" % (json.dumps([{"k":k,"q":q,"m":m,"not":nt,"p":pg,"x":x,"y":y,"rot":rot} for (k,q,m,nt,pg,x,y,rot) in M], ensure_ascii=False), json.dumps([P_AFE, P_PWR, P_INT]))

body = json.dumps({"code": js})
open(r"hardware/place_body.json", "w", encoding="utf-8").write(body)
print("body written, items:", len(M))
