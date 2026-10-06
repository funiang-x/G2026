# -*- coding: utf-8 -*-
"""标签规划 v2：key→id→pins；规则分配网络；生成 labels_body.json"""
import json, urllib.request, importlib.util, sys

spec = importlib.util.spec_from_file_location("pp", r"C:/Users/funiang/Desktop/Project/projects/G2026/hardware/place_parts.py")
pp = importlib.util.module_from_spec(spec); sys.modules["pp"] = pp; spec.loader.exec_module(pp)
BASE = "http://localhost:49620"

def ex(code):
    req = urllib.request.Request(BASE+"/execute", data=json.dumps({"code":code}).encode(), headers={"Content-Type":"application/json"})
    return json.loads(urllib.request.urlopen(req, timeout=120).read())

PAGES = {"AFE": ("fe840e6f545a2753", "pins_AFE.json"), "PWR": ("0c870c063661db6c", "pins_PWR.json"), "INT": ("63accad121e053cf", "pins_INT.json")}
pgkey = {"fe840e6f545a2753": "AFE", "0c870c063661db6c": "PWR", "63accad121e053cf": "INT"}
keypos = {(pg,x,y): k for (k,q,m,nt,pg,x,y,rot) in pp.M}

def endpoint(p):
    rot = p["rot"] % 360
    d = {0:(1,0), 90:(0,1), 180:(-1,0), 270:(0,-1)}[rot]
    return (p["x"] + d[0]*p["len"], p["y"] + d[1]*p["len"])

key_pins, debug = {}, {}
for name, (pg, pf) in PAGES.items():
    comps = ex(f'await eda.dmt_EditorControl.openDocument("{pg}"); const ids = await eda.sch_PrimitiveComponent.getAllPrimitiveId(); const cs = await eda.sch_PrimitiveComponent.get(ids); return cs.map(c => ({{id: c.primitiveId, x: c.x, y: c.y}}));')["result"]
    pinjs = 'const ids = ' + json.dumps([c["id"] for c in comps]) + '; const out = []; for (const id of ids) { const pins = await eda.sch_PrimitiveComponent.getAllPinsByPrimitiveId(id); out.push({id: id, pins: (pins||[]).map(p => ({num: p.pinNumber, name: p.pinName, x: p.x, y: p.y, rot: p.rotation, len: p.pinLength}))}); } return out;'
    pinmap = {p["id"]: p["pins"] for p in ex(pinjs)["result"]}
    for c in comps:
        k = keypos.get((pg, c["x"], c["y"]))
        if k:
            key_pins[k] = [(p["num"], p["name"], *endpoint(p)) for p in pinmap.get(c["id"], [])]
        elif pg == "63accad121e053cf" and (c["x"], c["y"]) == (750, 1040):
            key_pins["EVK1"] = [(p["num"], p["name"], *endpoint(p)) for p in pinmap.get(c["id"], [])]
        elif pg == "fe840e6f545a2753" and (c["x"], c["y"]) == (760, 1400):
            key_pins["u2b"] = [(p["num"], p["name"], *endpoint(p)) for p in pinmap.get(c["id"], [])]
        elif pg == "63accad121e053cf" and (c["x"], c["y"]) == (900, 1250):
            key_pins["h4"] = [(p["num"], p["name"], *endpoint(p)) for p in pinmap.get(c["id"], [])]

DEBUG_KEYS = ["bnc","u2","u4","j3","j2","h3","h4","j1","u1","q1","led1","d1","tp1","EVK1"]
for k in DEBUG_KEYS:
    if k in key_pins: print("==", k, key_pins[k][:12] if k=="EVK1" else key_pins[k])

labels, flags = [], []
CURPAGE = {"v": "AFE"}
KEY2PAGE = {}
for (k,q,m,nt,pg,x,y,rot) in pp.M: KEY2PAGE[k] = pg
KEY2PAGE.update({"EVK1": "63accad121e053cf", "u2b": "fe840e6f545a2753", "h4": "63accad121e053cf"})
def L(x, y, net): labels.append((x, y, net, CURPAGE["v"]))
def F(t, net, x, y): flags.append((t, net, x, y, CURPAGE["v"]))
def two_pin_vert(k, top_net, bot_net):
    pg = KEY2PAGE[k]; ps = sorted(key_pins[k], key=lambda p: -p[3])
    labels.append((ps[0][2], ps[0][3], top_net, pg)); labels.append((ps[1][2], ps[1][3], bot_net, pg))
def two_pin_horiz(k, left_net, right_net):
    pg = KEY2PAGE[k]; ps = sorted(key_pins[k], key=lambda p: p[2])
    labels.append((ps[0][2], ps[0][3], left_net, pg)); labels.append((ps[1][2], ps[1][3], right_net, pg))
AG, P5A, P33, G5 = "AGND", "+5V_A", "+3V3", "+5V"

CURPAGE["v"] = "fe840e6f545a2753"
# ===== AFE =====
two_pin_vert("tvs", "AFE_TERM", AG); two_pin_vert("rt1", "AFE_TERM", AG)
two_pin_horiz("ca1", "AFE_TERM", "AFE_B1_AC"); two_pin_horiz("jp3", "AFE_B1_AC", "AFE_B1_IN")
two_pin_vert("rb1", "AFE_B1_IN", "VBIAS_1V65")
for (num, name, x, y) in key_pins["u2"]:
    nm = (name or "").upper().replace(" ", "")
    if nm in ("IN1+","1IN+","1+","INA+"): L(x,y,"AFE_B1_IN")
    elif nm in ("IN1-","1IN-","1-","INA-"): L(x,y,"AFE_G1_OUT")
    elif nm in ("OUT1","1OUT","OUTA"): L(x,y,"AFE_G1_OUT")
    elif nm in ("IN2+","2IN+","2+","INB+"): L(x,y,"AFE_G2_IN")
    elif nm in ("IN2-","2IN-","2-","INB-"): L(x,y,"AFE_G2_FB")
    elif nm in ("OUT2","2OUT","OUTB"): L(x,y,"AFE_ADC_OUT")
    elif nm in ("V+","VCC","P","+"): F("Power", P5A, x, y)
    elif nm in ("V-","VEE","VSS","GND","N"): F("AnalogGround", AG, x, y)
    else: print("U2 unmapped:", num, name)
for (num, name, x, y) in key_pins.get("u2b", []):
    nm = (name or "").upper().replace(" ", "")
    if nm in ("OUTB",): L(x,y,"AFE_ADC_OUT")
    elif nm in ("INB-",): L(x,y,"AFE_G2_FB")
    elif nm in ("INB+",): L(x,y,"AFE_G2_IN")
    elif nm in ("V+","VCC","P"): F("Power", "+5V_A", x, y)
    elif nm in ("V-","VEE","VSS","GND","N"): F("AnalogGround", "AGND", x, y)
two_pin_horiz("rs1", "AFE_G1_OUT", "F1")
two_pin_horiz("l1","F1","F2"); two_pin_vert("c1","F2",AG)
two_pin_horiz("l2","F2","F3"); two_pin_vert("c2","F3",AG)
two_pin_horiz("l3","F3","F4"); two_pin_vert("c3","F4",AG)
two_pin_horiz("l4","F4","F5"); two_pin_vert("c4","F5",AG)
two_pin_vert("rl","F5",AG)
two_pin_horiz("ca2","F5","AFE_G2_IN"); two_pin_vert("rb2","AFE_G2_IN","VBIAS_1V65")
two_pin_horiz("rf","AFE_ADC_OUT","AFE_G2_FB"); two_pin_vert("rg","AFE_G2_FB",AG)
two_pin_horiz("r100","AFE_ADC_OUT","AFE_ADC_MON"); two_pin_vert("c470","AFE_ADC_MON",AG)
two_pin_horiz("jp1","AFE_ADC_MON","AFE_ADC_IN")
for (num, name, x, y) in key_pins["u4"]:
    nm = (name or "").upper().replace(" ","")
    if nm in ("IN","VIN"): F("Power", P5A, x, y)
    elif nm in ("OUT","VOUT"): L(x,y,"VREF_3V3")
    elif "GND" in nm: F("AnalogGround", AG, x, y)
    else: print("U4 unmapped:", num, name)
two_pin_vert("cin", P5A, AG); two_pin_vert("cout", "VREF_3V3", AG)
two_pin_vert("rdiv1","VREF_3V3","VBIAS_1V65"); two_pin_vert("rdiv2","VBIAS_1V65",AG)
two_pin_vert("cb","VBIAS_1V65",AG); two_pin_vert("jp2","VREF_3V3","VREF_3V3_EXT")
J3NETS = {1:P5A, 2:AG, 3:AG, 4:"AFE_ADC_IN", 5:"VBIAS_1V65", 6:"VREF_3V3_EXT"}
for (num, name, x, y) in key_pins["j3"]:
    net = J3NETS.get(int(num))
    if net is None: continue
    if net == P5A: F("Power", net, x, y)
    elif net == AG: F("AnalogGround", net, x, y)
    else: L(x, y, net)
TPNETS = {"tp1":"AFE_TERM","tp2":"AFE_B1_IN","tp3":"F5","tp4":"AFE_ADC_MON","tp5":"VBIAS_1V65","tp6":"VREF_3V3"}
for k, net in TPNETS.items():
    for (num, name, x, y) in key_pins[k]: L(x,y,net)

CURPAGE["v"] = "0c870c063661db6c"
# ===== 电源页 =====
two_pin_horiz("d1", G5, "+5V_RAW")
two_pin_vert("cin1", G5, "GND"); two_pin_vert("cout1", P33, "GND"); two_pin_vert("c100a", P33, "GND")
for (num, name, x, y) in key_pins["u1"]:
    nm = (name or "").upper().replace(" ","")
    if nm in ("VI","IN","VIN"): F("Power", G5, x, y)
    elif nm in ("VO","OUT","VOUT"): F("Power", P33, x, y)
    elif "GND" in nm: F("Ground", "GND", x, y)
    else: print("U1 unmapped:", num, name)
for (num, name, x, y) in key_pins["j1"]:
    if str(num) == "1": F("Power","+5V_RAW",x,y)
    else: F("Ground","GND",x,y)
ledpins = key_pins["led1"]
led_anode = [p for p in ledpins if (p[1] or "").upper() in ("A","ANODE","1","+")]
led_kath = [p for p in ledpins if (p[1] or "").upper() in ("K","KATHODE","CATHODE","2","C","-")]
if led_anode and led_kath:
    for (num,name,x,y) in led_anode: L(x,y,"LED_A")
    for (num,name,x,y) in led_kath: F("Ground","GND",x,y)
else:
    two_pin_vert("led1", "LED_A", "GND")
two_pin_vert("r1k", "LED_A", P33)
for (num, name, x, y) in key_pins["q1"]:
    nm = (name or "").upper().replace(" ","")
    if nm in ("G","GATE"): L(x,y,"Q1_G")
    elif nm in ("D","DRAIN"): L(x,y,"BL_K")
    elif nm in ("S","SOURCE"): F("Ground","GND",x,y)
    else: print("Q1 unmapped:", num, name)
two_pin_horiz("rgate", "LCD_BL_CTL", "Q1_G")
two_pin_vert("rgpd", "Q1_G", "GND")
two_pin_horiz("rbl", G5, "BL_A")
two_pin_vert("ri2ca", P33, "TP_SCL"); two_pin_vert("ri2cb", P33, "TP_SDA")
two_pin_vert("rim0", "IM0", "GND"); two_pin_vert("rim2", "IM2", "GND"); two_pin_vert("rim1", "IM1", P33)
two_pin_vert("rid", "LCD_ID", "GND")
two_pin_vert("c100b", P33, "GND"); two_pin_vert("c100c", P33, "GND")

CURPAGE["v"] = "63accad121e053cf"
# ===== 接口页 =====
H3G = {"2":"GND","3":"GND","4":"GND","5":"GND","6":"GND","7":"GND","8":"IM0","9":"IM1","10":"IM2",
       "11":P33,"12":P33,"17":"LCD_D15","18":"LCD_D14","19":"LCD_D13","20":"LCD_D12","21":"LCD_D11",
       "22":"LCD_D10","23":"LCD_D9","24":"LCD_D8","25":"LCD_D7","26":"LCD_D6","27":"LCD_D5","28":"LCD_D4",
       "29":"LCD_D3","30":"LCD_D2","33":"LCD_RST","34":"LCD_RD","35":"LCD_WR","36":"LCD_RS","37":"LCD_CS",
       "44":"BL_A","45":"LCD_ID"}
for (num, name, x, y) in key_pins["h3"]:
    n = str(num)
    if n in ("46","47"): F("Ground","GND",x,y); continue
    if n in ("38","39","40","41","42","43"): L(x,y,"BL_K"); continue
    net = H3G.get(n)
    if net is None: continue
    if net == "GND": F("Ground","GND",x,y)
    elif net == P33: F("Power",P33,x,y)
    else: L(x,y,net)
H4 = {1:P33, 2:"GND", 3:"TP_SCL", 4:"TP_SDA", 5:"TP_RST", 6:"TP_INT"}
for (num, name, x, y) in key_pins["h4"]:
    if str(num) in ("7","8"): F("Ground","GND",x,y); continue
    net = H4.get(int(num))
    if net is None: continue
    if net == P33: F("Power",P33,x,y)
    elif net == "GND": F("Ground","GND",x,y)
    else: L(x,y,net)
J2 = {1:P5A, 2:AG, 3:AG, 4:"AFE_ADC_IN", 5:"VBIAS_1V65", 6:"VREF_3V3_EXT"}
for (num, name, x, y) in key_pins["j2"]:
    net = J2.get(int(num))
    if net is None: continue
    if net == P5A: F("Power",net,x,y)
    elif net == AG: F("AnalogGround",net,x,y)
    else: L(x,y,net)
EVK = {"2":"VREF_3V3_EXT","3":"AFE_ADC_IN","4":"VBIAS_1V65","9":"TP_RST","11":"LCD_BL_CTL","12":"TP_INT",
       13:"LCD_D4","14":"LCD_RST","15":"LCD_D6","16":"LCD_D5","17":"LCD_D8","18":"LCD_D7","19":"LCD_D10",
       20:"LCD_D9","21":"LCD_D12","22":"LCD_D11","23":"TP_SCL","24":"TP_SDA","29":"LCD_D14","30":"LCD_D13",
       31:"LCD_RS","32":"LCD_D15","35":"LCD_D1","36":"LCD_D0"}
EVKFLAG = {"37":("AnalogGround","AGND"),"38":("Power","+3V3"),"39":("Ground","GND"),"40":("Power","+5V"),
           77:("Ground","GND"),"78":("Power","+3V3"),"79":("Ground","GND"),"80":("Power","+5V")}
for (num, name, x, y) in key_pins.get("EVK1", []):
    if str(num) in EVK: L(x,y,EVK[str(num)])
    elif str(num) in EVKFLAG: t,net = EVKFLAG[str(num)]; F(t,net,x,y)

json.dump({"labels": labels, "flags": flags}, open(r"C:/Users/funiang/Desktop/Project/projects/G2026/hardware/label_plan.json","w",encoding="utf-8"), ensure_ascii=False)
print("labels:", len(labels), "flags:", len(flags))
