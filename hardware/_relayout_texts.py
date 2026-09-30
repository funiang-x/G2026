# -*- coding: utf-8 -*-
"""AFE 页注释重排（21 条 TEXT 的坐标修正 + 1 条内容订正）。

规则：
  · 标题与框线留缝（字底距框线 ≥6 单位）；说明组行距 17→21；标题↔首行间距 ≥25
  · ② 组整块从"紫框内部"下移到"蓝框内顶部"（原位置与 ③ 的框交叠）
  · ③ 标题从框内左侧挪到说明块上方（x 对齐）；④/⑤ 标题贴各自框上缘（字底不压线）
  · ⑥ 标题从 (2058,1460) 挪到它自己的说明块上方（原与说明相距 630）
用法：python _relayout_texts.py [--apply]
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

# (id, 现x, 现y, 新x, 新y, 备注)
JOBS = [
    # ① 输入与端接（标题已在上，只微调行距与间距）
    ("8a9d7aa1167cf94e", 48, 1235, 48, 1237, "① 标题：与首行间距 25"),
    ("e1dd3460ea5d0d0a", 48, 1212, 48, 1212, "① 说明1 不动"),
    ("051cc8cb034e804d", 48, 1195, 48, 1191, "① 说明2 行距21"),
    ("d1d9fe3f1164d0f0", 48, 1178, 48, 1170, "① 说明3 行距21"),
    # ② 一级缓冲（整块下移进蓝框内顶部；原位置落在紫框内）
    ("f97d88215666aa04", 488, 1175, 488, 1154, "② 标题：离蓝框线 1160 留缝(框内)"),
    ("64fa4b2272318dfe", 488, 1232, 488, 1129, "② 说明1"),
    ("b5a143a6f5ac8352", 488, 1215, 488, 1108, "② 说明2"),
    ("c634ed22f441282e", 488, 1198, 488, 1087, "② 说明3"),
    # ③ 基准与偏置（标题挪到说明块上方，x 对齐）
    ("d297149715efd39d", -5, 1445, 148, 1594, "③ 标题：移到说明块上方"),
    ("4a43e0cbd96ad811", 148, 1569, 148, 1569, "③ 说明1 不动"),
    ("d4a021845b680bda", 148, 1552, 148, 1548, "③ 说明2 行距21"),
    ("70572692971c718f", 148, 1535, 148, 1527, "③ 说明3 行距21"),
    ("c9deee523b0187e6", 148, 1518, 148, 1506, "③ 说明4 行距21"),
    # ④ 8 阶滤波器（标题贴框上缘不压线；5 条说明统一 x=968、行距 21）
    ("e918f97a5f074474", 968, 1115, 968, 1125, "④ 标题：字底 1106 离框线 1100"),
    ("b16cb07d20fea22c", 968, 1172, 968, 1192, "④ 说明1"),
    ("f14a582511a43928", 968, 1155, 968, 1171, "④ 说明2"),
    ("6d845691ca286af8", 968, 1138, 968, 1150, "④ 说明3"),
    ("7820ff1c123f276c", 950, 1180, 968, 1213, "④ 说明4 统一x"),
    ("5f77433a135271d4", 950, 1210, 968, 1234, "④ 说明5 统一x"),
    # ⑤ 二级增益（标题贴框上缘；说明整组上移、行距 21）
    ("e0427e038b11f0d0", 2058, 1335, 2058, 1345, "⑤ 标题：字底 1326 离框线 1320"),
    ("4611e31c6c7da822", 2058, 1426, 2058, 1370, "⑤ 说明1（最下）"),
    ("26ce6acad012c882", 2058, 1409, 2058, 1391, "⑤ 说明2"),
    ("aa81265d45549ffb", 2058, 1392, 2058, 1412, "⑤ 说明3"),
    ("0320b1cea64a402d", 2058, 1375, 2058, 1433, "⑤ 说明4"),
    ("87d9fa386deef0b1", 2058, 1358, 2058, 1454, "⑤ 说明5（最上）"),
    # ⑥ 测试点（标题从远处挪到自己说明块上方）
    ("932911596d6b335a", 2058, 1460, 2058, 872, "⑥ 标题：与说明合成块"),
    ("8d2b44644141199a", 2050, 860, 2058, 845, "⑥ 说明1"),
    ("083b23a7324c6eea", 2050, 830, 2058, 820, "⑥ 说明2"),
]

# 内容订正（1 条）：BNC → SMA（B11 后 J1 已换 SMA 贴片座）
CONTENT = [
    ("e1dd3460ea5d0d0a",
     "SMA 50Ω 输入（配 BNC 转接头）→ D1(SRV05-4) 低容 TVS 防护"),
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


READ = (
    "await eda.dmt_EditorControl.openDocument(\"%s\");"
    "await new Promise(r=>setTimeout(r,1800));"
    "const out = [];"
    "for (const id of %s) {"
    "  const t = await eda.sch_PrimitiveText.get([id]);"
    "  const o = Array.isArray(t) ? t[0] : t;"
    "  const g=(fn)=>{try{return fn();}catch(e){return 'ERR';}};"
    "  out.push([id, o ? g(()=>o.getState_X()) : 'NOTFOUND', o ? g(()=>o.getState_Y()) : '']);"
    "}"
    "return JSON.stringify(out);"
) % (AFE, json.dumps([j[0] for j in JOBS]))

APPLY = (
    "await eda.dmt_EditorControl.openDocument(\"%s\");"
    "await new Promise(r=>setTimeout(r,1800));"
    "const out = [];"
    "for (const j of %s) {"
    "  try { await eda.sch_PrimitiveText.modify(j.id, {x: j.x, y: j.y}); out.push([j.id, 'ok']); }"
    "  catch (e) { out.push([j.id, String(e.message||e)]); }"
    "}"
    "for (const c of %s) {"
    "  try { await eda.sch_PrimitiveText.modify(c.id, {content: c.v}); out.push([c.id, 'content-ok']); }"
    "  catch (e) { out.push([c.id, 'content:' + String(e.message||e)]); }"
    "}"
    "return JSON.stringify(out);"
) % (AFE,
     json.dumps([{"id": a, "x": d, "y": e} for a, b, c, d, e, f in JOBS]),
     json.dumps([{"id": a, "v": b} for a, b in CONTENT], ensure_ascii=False))


def main():
    apply_ = "--apply" in sys.argv
    print("干跑：读回 21 条现状 (getState_X/Y)…")
    print(run(READ))
    if apply_:
        print("执行 modify(x,y) + 1 条 content…")
        r = run(APPLY)
        print(r)
        ok = sum(1 for x in json.loads(r) if x[1] in ("ok", "content-ok"))
        print("成功 %d / %d" % (ok, len(JOBS) + len(CONTENT)))
    else:
        print("（--apply 执行）")


if __name__ == "__main__":
    main()
