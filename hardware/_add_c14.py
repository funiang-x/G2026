# -*- coding: utf-8 -*-
"""C14 落件（U1 电源去耦 100nF 0805 X7R，CC0805KRX7R9BB104）。
按 T-42/T-48 配方：create(libraryType:DEVICE) + toAsync 位号 + 值按需源码注入 + 扇出。

用法：
    python _add_c14.py probe     # 只读：确认位号空闲 + 周边空间
    python _add_c14.py step1     # create + 位号 C14（不调 modify）
    python _add_c14.py step2     # 回读：pid / 引脚 / 源码里 C14 的 Value 状态
    python _add_c14.py step3     # 值注入（若 step2 显示空/缺，则删空+追加 "100nF"）
    python _add_c14.py step4     # 扇出：+5V_A（上方）/ AGND（下方）旗标 + 短线
"""
import json
import os
import sys
import urllib.request

BASE = "http://localhost:49620"
AFE_UUID = "fe840e6f545a2753"
X, Y, ROT = 880, 1200, 90
LIB_NAME = "CC0805KRX7R9BB104"
STATE = "_review/c14_state.json"

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


def read_source():
    n = run("return (await eda.sys_FileManager.getDocumentSource()).length;")
    parts = []
    i = 0
    while i < n:
        parts.append(run(
            "const s=await eda.sys_FileManager.getDocumentSource();"
            "return s.slice(%d,%d);" % (i, i + 40000)))
        i += 40000
    src = "".join(parts)
    assert len(src) == n
    return src


OPEN = ("const g=(o,fn)=>{try{return fn();}catch(e){return null;}};"
        "await eda.dmt_EditorControl.openDocument(\"%s\");"
        "await new Promise(r=>setTimeout(r,1800));" % AFE_UUID)

PROBE = OPEN + (
    "const all = await eda.sch_PrimitiveComponent.getAll();"
    "const des = [];"
    "for (const c of all) {"
    "  if ((c.componentType||'') !== 'part') continue;"
    "  const d = g(c,()=>c.getState_Designator());"
    "  if (d && d.indexOf('C1') === 0) des.push(d);"
    "}"
    "return JSON.stringify(des.sort());"
)

STEP1 = OPEN + (
    "const r = await eda.lib_Device.search(\"%s\");"
    "if (!r || !r.length) return JSON.stringify({err:'lib empty'});"
    "const c = r[0];"
    "const comp = { libraryType: 'DEVICE', libraryUuid: c.libraryUuid, uuid: c.uuid };"
    "const nc = await eda.sch_PrimitiveComponent.create(comp, %d, %d, \"\", %d, false, true, true);"
    "if (!nc) return JSON.stringify({err:'create failed'});"
    "const pid = nc.getState_PrimitiveId();"
    "const a = nc.toAsync(); a.setState_Designator(\"C14\"); await a.done();"
    "await new Promise(r=>setTimeout(r,1000));"
    "return JSON.stringify({pid: pid, name: g(nc,()=>nc.getState_Name())});"
) % (LIB_NAME, X, Y, ROT)

STEP2 = OPEN + (
    "const all = await eda.sch_PrimitiveComponent.getAll();"
    "let hit = null;"
    "for (const c of all) {"
    "  if ((c.componentType||'') !== 'part') continue;"
    "  if (g(c,()=>c.getState_Designator()) === 'C14') hit = c;"
    "}"
    "if (!hit) return JSON.stringify({found:false});"
    "const out = {found:true, pid: hit.getState_PrimitiveId(),"
    "  x: hit.getState_X(), y: hit.getState_Y(), rot: g(hit,()=>hit.getState_Rotation()),"
    "  name: g(hit,()=>hit.getState_Name())};"
    "const pins = [];"
    "try { const ps = await hit.getAllPins();"
    "  for (const p of ps) pins.push([String(p.getState_PinNumber()), p.getState_X(), p.getState_Y()]);"
    "} catch(e) {}"
    "out.pins = pins;"
    "return JSON.stringify(out);"
)


def cmd_probe():
    print("现有 C1x 位号:", run(PROBE))


def cmd_step1():
    r = run(STEP1)
    print("step1:", r)
    if isinstance(r, str):
        r = json.loads(r)
    if r.get("pid"):
        st = json.load(open(STATE, encoding="utf-8")) if os.path.exists(STATE) else {}
        st["pid"] = r["pid"]
        json.dump(st, open(STATE, "w", encoding="utf-8"))
        print("pid 已存 ->", STATE)


def _find_c14_pid():
    r = json.loads(run(STEP2))
    return r


def cmd_step2():
    r = _find_c14_pid()
    print("step2:", json.dumps(r, ensure_ascii=False))
    if not r.get("found"):
        return
    pid = r["pid"]
    src = read_source()
    parts = src.split("|\n")
    val_recs = []
    comp_rec = None
    for part in parts:
        hdr, _, data = part.partition("||")
        try:
            h = json.loads(hdr)
            d = json.loads(data)
        except Exception:
            continue
        if h.get("type") == "COMPONENT" and h.get("id") == pid:
            comp_rec = part
        if h.get("type") == "ATTR" and d.get("parentId") == pid and d.get("key") == "Value":
            val_recs.append((part, d.get("value")))
    print("源码 COMPONENT 记录:", "有" if comp_rec else "无")
    print("Value ATTR 条数:", len(val_recs))
    for p, v in val_recs:
        print("  value=%r" % v)
    st = json.load(open(STATE, encoding="utf-8")) if os.path.exists(STATE) else {}
    st["pid"] = pid
    st["pins"] = r.get("pins")
    json.dump(st, open(STATE, "w", encoding="utf-8"))


def cmd_step3():
    r = _find_c14_pid()
    pid = r["pid"]
    src = read_source()
    parts = src.split("|\n")

    # 找空 Value 记录（若有则摘掉）+ 计算 ticket/zIndex 最大值
    empty_idx = None
    max_ticket = 0
    max_z = 0
    used_ids = set()
    for i, part in enumerate(parts):
        hdr, _, data = part.partition("||")
        try:
            h = json.loads(hdr)
            d = json.loads(data)
        except Exception:
            continue
        used_ids.add(h.get("id"))
        if isinstance(h.get("ticket"), int):
            max_ticket = max(max_ticket, h["ticket"])
        if isinstance(d.get("zIndex"), int):
            max_z = max(max_z, d["zIndex"])
        if h.get("type") == "ATTR" and d.get("parentId") == pid and d.get("key") == "Value":
            if not (d.get("value") or "").strip():
                empty_idx = i

    # 若已有非空 Value -> 无需处理
    for i, part in enumerate(parts):
        hdr, _, data = part.partition("||")
        try:
            h = json.loads(hdr)
            d = json.loads(data)
        except Exception:
            continue
        if h.get("type") == "ATTR" and d.get("parentId") == pid and d.get("key") == "Value":
            if (d.get("value") or "").strip():
                print("已有非空 Value =", repr(d.get("value")), "-> 跳过")
                return

    import secrets
    new_id = secrets.token_hex(8)
    while new_id in used_ids:
        new_id = secrets.token_hex(8)

    rec_h = {"type": "ATTR", "ticket": max_ticket + 1, "id": new_id}
    rec_d = {"x": None, "y": None, "rotation": None, "color": None, "fontFamily": None,
             "fontSize": None, "fontWeight": None, "italic": None, "underline": None,
             "strikeout": False, "align": None, "value": "100nF", "keyVisible": None,
             "valueVisible": None, "key": "Value", "fillColor": None,
             "parentId": pid, "zIndex": max_z + 1}
    rec = json.dumps(rec_h, ensure_ascii=False) + "||" + json.dumps(rec_d, ensure_ascii=False)

    if empty_idx is not None:
        old_rec = parts[empty_idx]
        # 摘掉该记录（连同其一侧分隔符）
        if empty_idx > 0:
            new_src = src.replace("|\n" + old_rec, "", 1)
        else:
            new_src = src.replace(old_rec + "|\n", "", 1)
        action = "replace"
    else:
        new_src = src + "|\n" + rec
        action = "append"

    if new_src == src:
        raise SystemExit("no-op! abort")

    # 若是替换：把新记录追加到末尾（保持"只追加+只删除"的两种已验证动作）
    if action == "replace":
        new_src = new_src + "|\n" + rec

    js = ("const src = %s;"
          "const r = await eda.sys_FileManager.setDocumentSource(src);"
          "return JSON.stringify({ok: r});" % json.dumps(new_src))
    out = run(js, timeout=300)
    print("step3 (%s): %s" % (action, out))

    # 回读验证
    src2 = read_source()
    n_val = src2.count('"parentId":"%s"' % pid)
    has_100n = ('"key":"Value","value":"100nF"' in src2) or ('"value":"100nF","key":"Value"' in src2)
    # 精准验证：找 C14 的 Value
    ok = False
    for part in src2.split("|\n"):
        hdr, _, data = part.partition("||")
        try:
            h = json.loads(hdr)
            d = json.loads(data)
        except Exception:
            continue
        if h.get("type") == "ATTR" and d.get("parentId") == pid and d.get("key") == "Value":
            print("回读 C14 Value =", repr(d.get("value")))
            ok = bool((d.get("value") or "").strip())
    print("回读长度:", len(src2), "| Value 就位:", ok)


def cmd_step4():
    r = _find_c14_pid()
    pid, pins = r["pid"], r["pins"]
    if not pins:
        raise SystemExit("no pins found")
    # pins: [[num, x, y], ...]，取 y 大者为上
    pins_sorted = sorted(pins, key=lambda p: -p[2])
    top, bot = pins_sorted[0], pins_sorted[-1]
    tx, ty = int(round(top[1])), int(round(top[2]))
    bx, by = int(round(bot[1])), int(round(bot[2]))
    f_top = (tx, ty + 20)      # +5V_A 在上方 rot=270
    f_bot = (bx, by - 20)      # AGND 在下方 rot=90
    js = (
        "const g=(o,fn)=>{try{return fn();}catch(e){return null;}};"
        "await eda.dmt_EditorControl.openDocument(\"%s\");"
        "await new Promise(r=>setTimeout(r,1800));"
        "const f1 = await eda.sch_PrimitiveComponent.createNetFlag('Power','+5V_A',%d,%d,270,false);"
        "const w1 = await eda.sch_PrimitiveWire.create([%d,%d,%d,%d], \"\");"
        "const f2 = await eda.sch_PrimitiveComponent.createNetFlag('Ground','AGND',%d,%d,90,false);"
        "const w2 = await eda.sch_PrimitiveWire.create([%d,%d,%d,%d], \"\");"
        "await new Promise(r=>setTimeout(r,800));"
        "return JSON.stringify({f1:!!f1, w1:!!w1, f2:!!f2, w2:!!w2});"
    ) % (AFE_UUID, f_top[0], f_top[1], tx, ty, f_top[0], f_top[1],
         f_bot[0], f_bot[1], bx, by, f_bot[0], f_bot[1])
    print("step4:", run(js))


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "probe":
        cmd_probe()
    elif cmd == "step1":
        cmd_step1()
    elif cmd == "step2":
        cmd_step2()
    elif cmd == "step3":
        cmd_step3()
    elif cmd == "step4":
        cmd_step4()
    else:
        print(__doc__)


if __name__ == "__main__":
    main()
