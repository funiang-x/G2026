# -*- coding: utf-8 -*-
"""从 EDA 导出当前工程 .epro2 —— 桥分块 base64 回传（T-11 姿势）。
用法：python _export_project2.py [out.epro2]
"""
import base64
import json
import sys
import urllib.request

BASE = "http://localhost:49620"
OUT = sys.argv[1] if len(sys.argv) > 1 else "_review/G2026_board.epro2"
CHUNK = 40000

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


# 1) 把工程文件缓存成 window.__pb（base64 大字符串）
CACHE = (
    "const f = await eda.sys_FileManager.getProjectFile(undefined, undefined, 'epro2');"
    "if (!f) return JSON.stringify({err: 'no file'});"
    "const buf = new Uint8Array(await f.arrayBuffer());"
    "let s = ''; const CH = 0x8000;"
    "for (let i = 0; i < buf.length; i += CH) {"
    "  s += String.fromCharCode.apply(null, buf.subarray(i, i + CH));"
    "}"
    "window.__pb = btoa(s);"
    "return JSON.stringify({name: f.name, bytes: buf.length, b64: window.__pb.length});"
)

info = json.loads(run(CACHE, timeout=600))
print("cache:", info)
if info.get("err"):
    raise SystemExit(1)

n = info["b64"]
parts = []
i = 0
while i < n:
    parts.append(run("return window.__pb.slice(%d, %d);" % (i, min(i + CHUNK, n))))
    i += CHUNK
    if (i // CHUNK) % 16 == 0:
        print("  ... %d / %d" % (i, n))
b64 = "".join(parts)
assert len(b64) == n, "length mismatch: %d vs %d" % (len(b64), n)
data = base64.b64decode(b64)
with open(OUT, "wb") as f:
    f.write(data)
print("saved %s (%d bytes)" % (OUT, len(data)))

import zipfile
try:
    z = zipfile.ZipFile(OUT)
    names = z.namelist()
    print("zip ok, entries=%d" % len(names))
    for nm in names[:20]:
        print("   ", nm, z.getinfo(nm).file_size)
except Exception as e:
    print("zip check failed:", e)
