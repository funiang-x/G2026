# -*- coding: utf-8 -*-
"""与嘉立创EDA桥接通信的小工具（只读为主）。"""
import json, urllib.request

BASE = "http://localhost:49620"


def exec_js(code, timeout=60):
    req = urllib.request.Request(
        BASE + "/execute",
        data=json.dumps({"code": code}, ensure_ascii=False).encode("utf-8"),
        headers={"Content-Type": "application/json"},
    )
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode("utf-8"))


def run(code, timeout=60):
    r = exec_js(code, timeout)
    if not r.get("success"):
        raise RuntimeError(r.get("error"))
    return r["result"]
