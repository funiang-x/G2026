# -*- coding: utf-8 -*-
"""AFE 页注释订正（内容确定过期/缺失的 5 条 TEXT）。
干跑默认；--apply 执行；改后回读自证。
"""
import json
import sys
import urllib.request

BASE = "http://localhost:49620"
AFE = "fe840e6f545a2753"

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

JOBS = [
    ("b5a143a6f5ac8352",
     "R2 10k 把 VBIAS_1V65 注入同相端 → 输入中点 1.65V",
     "R2 10k 把 VBIAS 注入同相端 → 输入中点 = VREF/2"),
    ("4611e31c6c7da822",
     "U1B(OPA2197) 同相放大：R6 30k 上支路反馈、R7 10k 下支路对 AGND",
     "U1B(OPA2197) 同相放大：R6 30k 上支路反馈、R7 10k 下支路接 VBIAS"),
    ("0320b1cea64a402d",
     "R8 100Ω + C10 470pF 组成 RC 抗混叠/驱动网络 → JP2 断点 → J2",
     "R8 100Ω + C10 470pF 组成 RC 驱动网络 → JP2 断点 → 核心板 PA0"),
    ("083b23a7324c6eea",
     "TP1 端接节点 · TP2 LC入 · TP3 LC出 · TP4 ×4出 · TP5 1.65V轨 · TP6 VREF",
     "TP1 端接 · TP2 LC入 · TP3 LC出 · TP4 ×4出 · TP5 VBIAS · TP6 VREF"),
    ("70572692971c718f",
     "C11=+5V_A 储能 · C12=VREF 去耦 · C13=VBIAS；JP3 可断开 VREF",
     "C11=+5V_A 储能 · C12=VREF 去耦 · C13=VBIAS · C14=U1 电源去耦；JP3 可断开 VREF"),
]


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


# 1) 干跑：读回每条 TEXT 的现内容
READ = (
    "await eda.dmt_EditorControl.openDocument(\"%s\");"
    "await new Promise(r=>setTimeout(r,1800));"
    "const out = [];"
    "for (const id of %s) {"
    "  const t = await eda.sch_PrimitiveText.get([id]);"
    "  const o = Array.isArray(t) ? t[0] : t;"
    "  out.push({id: id, v: o ? (o.getState_Value ? o.getState_Value() : null) : 'NOTFOUND'});"
    "}"
    "return JSON.stringify(out);"
) % (AFE, json.dumps([j[0] for j in JOBS]))

# 2) 写回（逐条 modify content）
APPLY = (
    "await eda.dmt_EditorControl.openDocument(\"%s\");"
    "await new Promise(r=>setTimeout(r,1800));"
    "const out = [];"
    "for (const j of %s) {"
    "  try {"
    "    await eda.sch_PrimitiveText.modify(j.id, {content: j.v});"
    "    out.push([j.id, 'ok']);"
    "  } catch (e) { out.push([j.id, String(e.message||e)]); }"
    "}"
    "return JSON.stringify(out);"
) % (AFE, json.dumps([{"id": a, "v": c} for a, b, c in JOBS], ensure_ascii=False))


def verify():
    """直接读源码比对（比 getState_Value 可靠——T-37 说 text 的 value 读不到）"""
    n = run("return (await eda.sys_FileManager.getDocumentSource()).length;")
    parts, i = [], 0
    while i < n:
        parts.append(run("const s=await eda.sys_FileManager.getDocumentSource();"
                         "return s.slice(%d,%d);" % (i, i + 40000)))
        i += 40000
    src = "".join(parts)
    for old, new in [(b, c) for a, b, c in JOBS]:
        print("  旧文案还在? %-5s | 新文案已落? %s" % (old[:18] in src, new[:18] in src))


def main():
    apply_ = "--apply" in sys.argv
    print("干跑：读回现状…")
    print(run(READ))
    if apply_:
        print("执行 modify…")
        print(run(APPLY))
        print("回读源码校验：")
        verify()
    else:
        print("（--apply 执行写回）")


if __name__ == "__main__":
    main()
