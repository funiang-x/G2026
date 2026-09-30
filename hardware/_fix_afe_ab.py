# -*- coding: utf-8 -*-
"""修复 AFE 页两处致命缺陷（只动几何，不改设计参数）。

A: U1A 输出节点 (R3.1, U1.1, U1.2) 被一条合并折线经 (760,1100) 与 +5V_A 旗标短接
   -> delete 旧折线；重建 3 条：输出主线 / U1.2 反馈绕行 / U1.8 供电短线
B: R7.1 落在 AGND（U1B 反相端直流增益 4 -> 输出饱和），应为 VBIAS
   -> delete AGND 旗标 @(2140,1290)；原位重建 VBIAS 净标签；短线按需补回

用法：
    python _fix_afe_ab.py            # 干跑：备份源码 + 复核对象 + 打印计划
    python _fix_afe_ab.py --apply-a  # 执行 A
    python _fix_afe_ab.py --apply-b  # 执行 B
"""
import json
import os
import sys
import urllib.request

BASE = "http://localhost:49620"
AFE_UUID = "fe840e6f545a2753"
OLD_WIRE = "579dd15a1bca7acd"          # 含 (830,1100)-(760,1100) 的合并折线
AGND_FLAG = "56330b6c12fc2de0"          # AGND @(2140,1290)
AGND_SHORT = "abc492dfbbfe080d"         # [(2140,1290),(2140,1270)]

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


def backup_source():
    n = run("return (await eda.sys_FileManager.getDocumentSource()).length;")
    parts = []
    i = 0
    while i < n:
        parts.append(run(
            "const s=await eda.sys_FileManager.getDocumentSource();"
            "return s.slice(%d,%d);" % (i, i + 40000)))
        i += 40000
    src = "".join(parts)
    assert len(src) == n, "chunked read mismatch: %d vs %d" % (len(src), n)
    os.makedirs("_review/final-20260930", exist_ok=True)
    path = "_review/final-20260930/src_AFE_pre_fix.txt"
    with open(path, "w", encoding="utf-8") as f:
        f.write(src)
    return path, n, src.count('"key":"NO_CONNECT"')


DRY_JS = (
    "const g=(o,fn)=>{try{return fn();}catch(e){return null;}};"
    "await eda.dmt_EditorControl.openDocument(\"%s\");"
    "await new Promise(r=>setTimeout(r,1800));"
    "const wids = (await eda.sch_PrimitiveWire.getAllPrimitiveId())||[];"
    "const out = { old_wire: wids.includes(\"%s\"), short_wire: wids.includes(\"%s\") };"
    "const f = await eda.sch_PrimitiveComponent.get([\"%s\"]);"
    "const fo = Array.isArray(f) ? f[0] : f;"
    "out.flag_rot = g(fo,()=>fo.getState_Rotation());"
    "out.flag_x = g(fo,()=>fo.getState_X());"
    "out.flag_y = g(fo,()=>fo.getState_Y());"
    "out.flag_net = g(fo,()=>fo.getState_Net());"
    "return JSON.stringify(out);"
) % (AFE_UUID, OLD_WIRE, AGND_SHORT, AGND_FLAG)

APPLY_A = (
    "const g=(o,fn)=>{try{return fn();}catch(e){return null;}};"
    "await eda.dmt_EditorControl.openDocument(\"%s\");"
    "await new Promise(r=>setTimeout(r,1800));"
    "const log = [];"
    "const raw = await eda.sch_PrimitiveWire.delete([\"%s\"]);"
    "log.push([\"del_old_wire\", JSON.stringify(raw)]);"
    "const wids1 = (await eda.sch_PrimitiveWire.getAllPrimitiveId())||[];"
    "const oldGone = !wids1.includes(\"%s\");"
    "log.push([\"old_gone\", oldGone]);"
    "if (oldGone) {"
    "  const w1 = await eda.sch_PrimitiveWire.create([800,1050,880,1050], \"\");"
    "  const w2 = await eda.sch_PrimitiveWire.create([720,1060,690,1060,690,1140,830,1140,830,1050], \"\");"
    "  const w3 = await eda.sch_PrimitiveWire.create([760,1080,760,1100], \"\");"
    "  log.push([\"w1\", !!w1, \"w2\", !!w2, \"w3\", !!w3]);"
    "} else {"
    "  log.push([\"abort\", \"old wire still present, nothing built\"]);"
    "}"
    "return JSON.stringify(log);"
) % (AFE_UUID, OLD_WIRE, OLD_WIRE)

APPLY_B = (
    "const g=(o,fn)=>{try{return fn();}catch(e){return null;}};"
    "await eda.dmt_EditorControl.openDocument(\"%s\");"
    "await new Promise(r=>setTimeout(r,1800));"
    "const log = [];"
    "const f = await eda.sch_PrimitiveComponent.get([\"%s\"]);"
    "const fo = Array.isArray(f) ? f[0] : f;"
    "const flagRot = g(fo,()=>fo.getState_Rotation());"
    "log.push([\"flag_rot\", flagRot]);"
    "const raw = await eda.sch_PrimitiveComponent.delete([\"%s\"]);"
    "log.push([\"del_flag\", JSON.stringify(raw)]);"
    "const wids1 = (await eda.sch_PrimitiveWire.getAllPrimitiveId())||[];"
    "const shortGone = !wids1.includes(\"%s\");"
    "log.push([\"short_gone\", shortGone]);"
    "const np = await eda.sch_PrimitiveComponent.createNetPort(\"BI\",\"VBIAS\",2140,1290,flagRot===null?270:flagRot,false);"
    "log.push([\"new_port\", !!np]);"
    "if (shortGone) {"
    "  const w4 = await eda.sch_PrimitiveWire.create([2140,1270,2140,1290], \"\");"
    "  log.push([\"w4\", !!w4]);"
    "} else {"
    "  log.push([\"w4\", \"skipped(short still there)\"]);"
    "}"
    "return JSON.stringify(log);"
) % (AFE_UUID, AGND_FLAG, AGND_FLAG, AGND_SHORT)


def main():
    apply_a = "--apply-a" in sys.argv
    apply_b = "--apply-b" in sys.argv

    path, n, nc = backup_source()
    print("备份: %s  (%d 字符, NO_CONNECT 计数 %d)" % (path, n, nc))
    print("复核:", run(DRY_JS))

    if apply_a:
        print("执行 A ...")
        print("A 结果:", run(APPLY_A))
    if apply_b:
        print("执行 B ...")
        print("B 结果:", run(APPLY_B))
    if not (apply_a or apply_b):
        print("（干跑完成；--apply-a / --apply-b 执行）")


if __name__ == "__main__":
    main()
