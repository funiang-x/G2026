# -*- coding: utf-8 -*-
"""从 EDA 导出当前工程为 .epro2（用于喂 easyeda-viewer 看真正的图纸）。
走"页面内 blob 下载"路径：把 File 对象挂 a[download] 点击，落到 EDA 客户端的下载目录。
"""
import json
import sys
import urllib.request

BASE = "http://localhost:49620"

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


CODE = (
    "const f = await eda.sys_FileManager.getProjectFile(undefined, undefined, 'epro2');"
    "if (!f) return JSON.stringify({err: 'getProjectFile returned nothing'});"
    "const url = URL.createObjectURL(f);"
    "const a = document.createElement('a');"
    "a.href = url; a.download = 'G2026_board.epro2';"
    "document.body.appendChild(a); a.click();"
    "await new Promise(r => setTimeout(r, 800));"
    "return JSON.stringify({name: f.name, size: f.size, type: f.type});"
)

print(run(CODE))
