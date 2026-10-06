# -*- coding: utf-8 -*-
"""G2026 · 模拟电路仿真内核（MNA）

为什么自己写而不用 ngspice：
  · 本机**没有 ngspice/LTspice**，且外网（SourceForge / GitHub）在沙箱内不可达；
  · 本工程需要的是**可复现、可审计、能被 CI 跑**的仿真，而不是一次性的 GUI 操作；
  · 本内核只支持本板真正用到的元件集（R/L/C/V/I/VCVS/VCCS/二极管/时变开关），
    足够覆盖 AFE + ADC 前端 + 电源耦合；同时另出 .cir 网表供外部 SPICE 独立复核。

元件 → MNA 装填规则
  R  : 电导 G 直接叠加到 (n1,n2)
  C  : AC→ jωC；瞬态→ 伴随模型（梯形/后向欧拉），需存上一步 v
  L  : AC→ 1/(jωL)；瞬态→ 伴随模型，**引入一个支路电流未知量**
  V  : 支路电流未知量，KVL 行
  I  : RHS
  E  : VCVS，支路电流未知量 + 控制方程
  G  : VCCS，跨导叠加
  D  : 非线性（牛顿迭代，Shockley + Rs）
  S  : 时变电阻（开关）

未知量顺序：[ 节点电压(除地) , 支路电流(V/E/L) ]

作者：AI · 2026-10-03 · 本文件是 G2026 仿真链的单一内核，禁止在调用侧另建求解逻辑。
"""
from __future__ import annotations

import math

import numpy as np

GND = "0"


class Circuit:
    """一个电路。元件以「加进去」的方式登记，节点名用字符串。"""

    def __init__(self, title: str = ""):
        self.title = title
        self._nodemap = {GND: 0}
        self._nodename = [GND]
        self.R = []   # (n1,n2,ohm,label)
        self.C = []   # (n1,n2,farad,label)
        self.L = []   # (n1,n2,henry,label)
        self.V = []   # (np, nn, dc, label, ac_mag, ac_ph)
        self.VWAVE = []  # 与 V 一一对应的时变函数（None = 恒等于 dc）
        self.I = []   # (np, nn, dc, label, ac_mag, ac_ph)
        self.E = []   # (nout_p, nout_n, nctl_p, nctl_n, gain, label)
        self.Gs = []  # (nout_p, nout_n, nctl_p, nctl_n, gm, label)
        self.D = []   # (na, nk, Is, n, Rs, label)
        self.SW = []  # (n1,n2, r_on, r_off, ctrl_fn, label)  ctrl_fn(t)->bool(on)
        self.GT = []  # (nop,non,ncp,ncn, gm, vk, label)  软饱和跨导 I=gm·vk·tanh(vd/vk)

    # ---------- 节点 ----------
    def node(self, name: str) -> int:
        if name not in self._nodemap:
            self._nodemap[name] = len(self._nodename)
            self._nodename.append(name)
        return self._nodemap[name]

    @property
    def nn(self) -> int:
        return len(self._nodename)

    # ---------- 加元件 ----------
    def add_R(self, n1, n2, ohm, label=""):
        self.node(n1); self.node(n2)
        self.R.append((n1, n2, float(ohm), label)); return self

    def add_C(self, n1, n2, f, label=""):
        self.node(n1); self.node(n2)
        self.C.append((n1, n2, float(f), label)); return self

    def add_L(self, n1, n2, h, label=""):
        self.node(n1); self.node(n2)
        self.L.append((n1, n2, float(h), label)); return self

    def add_V(self, np_, nn_, dc=0.0, label="", ac_mag=0.0, ac_ph=0.0, wave=None):
        """wave(t) 给定时，瞬态用 wave(t) 而不是 dc（AC 仍用 ac_mag）。"""
        self.node(np_); self.node(nn_)
        self.V.append((np_, nn_, float(dc), label, float(ac_mag), float(ac_ph)))
        self.VWAVE.append(wave)
        return self

    def add_I(self, np_, nn_, dc=0.0, label="", ac_mag=0.0, ac_ph=0.0):
        self.node(np_); self.node(nn_)
        self.I.append((np_, nn_, float(dc), label, float(ac_mag), float(ac_ph))); return self

    def add_E(self, nop, non, ncp, ncn, gain, label=""):
        for n in (nop, non, ncp, ncn):
            self.node(n)
        self.E.append((nop, non, ncp, ncn, float(gain), label)); return self

    def add_G(self, nop, non, ncp, ncn, gm, label=""):
        for n in (nop, non, ncp, ncn):
            self.node(n)
        self.Gs.append((nop, non, ncp, ncn, float(gm), label)); return self

    def add_D(self, na, nk, Is=1e-14, n=1.0, Rs=0.0, label=""):
        self.node(na); self.node(nk)
        self.D.append((na, nk, float(Is), float(n), float(Rs), label)); return self

    def add_Gtanh(self, nop, non, ncp, ncn, gm, vk, label=""):
        """软饱和跨导：I = gm·vk·tanh(vd/vk)。小信号斜率 = gm；大信号限流 = gm·vk。
        用途：① 运放差分对（同时给出压摆率上限）② 输出级 gm 压缩（IM3 源）。"""
        for n in (nop, non, ncp, ncn):
            self.node(n)
        self.GT.append((nop, non, ncp, ncn, float(gm), float(vk), label)); return self

    def add_SW(self, n1, n2, r_on, r_off, ctrl, label=""):
        self.node(n1); self.node(n2)
        self.SW.append((n1, n2, float(r_on), float(r_off), ctrl, label)); return self

    # ---------- 内部：索引 ----------
    def _branch_list(self, kind):
        """返回需要支路电流未知量的元件序号列表（V/E/L）。"""
        out = []
        for i, _ in enumerate(self.V):
            out.append(("V", i))
        for i, _ in enumerate(self.E):
            out.append(("E", i))
        for i, _ in enumerate(self.L):
            out.append(("L", i))
        return out

    def _size(self):
        return self.nn - 1 + len(self.V) + len(self.E) + len(self.L)

    def _bidx(self):
        """(kind, i) -> 支路电流在未知量里的下标"""
        base = self.nn - 1
        d = {}
        for k, (kind, i) in enumerate(self._branch_list(kind=None)):
            d[(kind, i)] = base + k
        return d

    # ---------- 线性装填（AC / 瞬态共用一个"电导+源"的模板） ----------
    def _stamp_linear(self, A, b, y_R, y_C, z_L, v_src, i_src):
        """
        y_R(n1,n2) -> 复/实电导
        y_C(n1,n2) -> 电容等效导纳
        z_L(n1,n2) -> (等效电阻, 等效电压源串在支路上)  瞬态；AC 时 z_L 返回纯导纳
        v_src(i)   -> 电压源值（复/实）
        i_src(i)   -> 电流源值
        """
        for (n1, n2, ohm, lb) in self.R:
            g = y_R(ohm)
            self._stamp_g(A, n1, n2, g)
        for (n1, n2, f, lb) in self.C:
            g = y_C(f)
            if g is not None:
                self._stamp_g(A, n1, n2, g)
        for (n1, n2, h, lb) in self.L:
            z = z_L(h)
            if z is None:                      # AC：当导纳处理
                continue
            req, veq = z
            self._stamp_L(A, b, n1, n2, req, veq)
        bi = self._bidx()
        for i, (np_, nn_, dc, lb, am, ap) in enumerate(self.V):
            r = bi[("V", i)]
            self._stamp_vsrc(A, b, r, np_, nn_, v_src(i))
        for i, (np_, nn_, dc, lb, am, ap) in enumerate(self.I):
            self._stamp_isrc(b, np_, nn_, i_src(i))
        for i, (nop, non, ncp, ncn, gain, lb) in enumerate(self.E):
            r = bi[("E", i)]
            self._stamp_vsrc(A, b, r, nop, non, 0.0)
            self._stamp_ctrl(A, r, ncp, ncn, gain)
        for i, (nop, non, ncp, ncn, gm, lb) in enumerate(self.Gs):
            self._stamp_vccs(A, nop, non, ncp, ncn, gm)

    def _stamp_g(self, A, n1, n2, g):
        i, j = self.node(n1), self.node(n2)
        if i: A[i - 1, i - 1] += g
        if j: A[j - 1, j - 1] += g
        if i and j:
            A[i - 1, j - 1] -= g
            A[j - 1, i - 1] -= g

    def _stamp_L(self, A, b, n1, n2, req, veq):
        """L 的伴随模型：支路电流未知量 r，方程 v(n1)-v(n2) - req*i = veq"""
        i, j = self.node(n1), self.node(n2)
        k = self._Lslot
        self._Lslot += 1
        r = self.nn - 1 + len(self.V) + len(self.E) + k
        g = 1.0 / req
        if i: A[i - 1, r] += 1.0
        if j: A[j - 1, r] -= 1.0
        A[r, r] -= req
        if i: A[r, i - 1] += 1.0
        if j: A[r, j - 1] -= 1.0
        b[r] += veq
        _ = g

    def _stamp_vsrc(self, A, b, r, np_, nn_, val):
        i, j = self.node(np_), self.node(nn_)
        if i: A[i - 1, r] += 1.0
        if j: A[j - 1, r] -= 1.0
        if i: A[r, i - 1] += 1.0
        if j: A[r, j - 1] -= 1.0
        b[r] += val

    def _stamp_ctrl(self, A, r, ncp, ncn, gain):
        i, j = self.node(ncp), self.node(ncn)
        if i: A[r, i - 1] -= gain
        if j: A[r, j - 1] += gain

    def _stamp_isrc(self, b, np_, nn_, val):
        i, j = self.node(np_), self.node(nn_)
        if i: b[i - 1] += val
        if j: b[j - 1] -= val

    def _stamp_vccs(self, A, nop, non, ncp, ncn, gm):
        i, j = self.node(nop), self.node(non)
        p, q = self.node(ncp), self.node(ncn)
        for (rr, cc, sgn) in ((i, p, +1), (i, q, -1), (j, p, -1), (j, q, +1)):
            if rr and cc:
                A[rr - 1, cc - 1] += sgn * gm

    # ================= AC =================
    def ac(self, freqs, extra_R=None):
        """复数小信号 AC 扫描。返回 {节点名: 复电压数组}（含 '0'=0）。"""
        freqs = np.atleast_1d(np.asarray(freqs, dtype=float))
        N = self._size()
        # tanh 跨导在工作点线性化一次（AC 是小信号，geff 与频率无关）
        gt_eff = []
        if self.GT:
            op = self.dc()
            for (nop, non, ncp, ncn, gm, vk, lb) in self.GT:
                a, c = self.node(ncp), self.node(ncn)
                vd = (op[self._nodename[a]] if a else 0.0) - \
                     (op[self._nodename[c]] if c else 0.0)
                t = math.tanh(vd / vk)
                gt_eff.append((nop, non, ncp, ncn, gm * (1.0 - t * t)))
        out = {nm: np.zeros(len(freqs), dtype=complex) for nm in self._nodename}
        for m, f in enumerate(freqs):
            w = 2 * np.pi * f
            A = np.zeros((N, N), dtype=complex)
            b = np.zeros(N, dtype=complex)
            self._Lslot = 0
            self._stamp_linear(
                A, b,
                y_R=lambda ohm: 1.0 / ohm,
                y_C=lambda c: 1j * w * c if w > 0 else None,
                # AC 下 L 走**支路**形式（v = jωL·i）—— 不能用导纳：
                # 支路未知量已在矩阵尺寸里，若该行留空就是奇异矩阵。
                z_L=lambda h: (1j * w * h if w > 0 else 1e-6, 0.0),
                v_src=lambda i: self.V[i][4] * np.exp(1j * self.V[i][5]),
                i_src=lambda i: self.I[i][4] * np.exp(1j * self.I[i][5]),
            )
            if extra_R:
                for (n1, n2, g) in extra_R:
                    self._stamp_g(A, n1, n2, g)
            if self.GT:
                for (nop, non, ncp, ncn, geff) in gt_eff:
                    self._stamp_vccs(A, nop, non, ncp, ncn, geff)
            x = np.linalg.solve(A, b)
            for k, nm in enumerate(self._nodename):
                out[nm][m] = 0.0 if k == 0 else x[k - 1]
        return out

    # ================= DC =================
    def dc(self, gmin=1e-12):
        """直流工作点（L=短路，C=开路，二极管/tanh 跨导线性化迭代）。"""
        N = self._size()
        vd = np.zeros(len(self.D))
        vgt = np.zeros(len(self.GT))
        x = np.zeros(N)
        for _ in range(120):
            A = np.zeros((N, N)); b = np.zeros(N)
            self._Lslot = 0
            self._stamp_linear(A, b,
                               y_R=lambda ohm: 1.0 / ohm,
                               y_C=lambda c: None,
                               z_L=lambda h: (1e-6, 0.0),
                               v_src=lambda i: self.V[i][2],
                               i_src=lambda i: self.I[i][2])
            self._stamp_diodes(A, b, vd, gmin)
            self._stamp_gtanh(A, b, vgt)
            self._stamp_switches(A, 1e-9, 0.0, True)
            xn = np.linalg.solve(A, b)
            if not self.D and not self.GT:
                x = xn; break
            dv = np.max(np.abs(np.array([self._dv(xn, d) for d in self.D]) - vd)) if self.D else 0.0
            dg = np.max(np.abs(np.array([self._gv(xn, e) for e in self.GT]) - vgt)) if self.GT else 0.0
            x = xn
            vd = np.array([self._dv(xn, d) for d in self.D]) if self.D else vd
            vgt = np.array([self._gv(xn, e) for e in self.GT]) if self.GT else vgt
            if max(dv, dg) < 1e-10:
                break
        res = {}
        for k, nm in enumerate(self._nodename):
            res[nm] = 0.0 if k == 0 else x[k - 1]
        self._dc_x = x.copy()
        self._dc_vgt = vgt.copy()
        return res

    def _dv(self, x, d):
        na, nk, Is, nn_, Rs, lb = d
        i, j = self.node(na), self.node(nk)
        v = (x[i - 1] if i else 0.0) - (x[j - 1] if j else 0.0)
        return v

    def _stamp_gtanh(self, A, b, vgt):
        """tanh 跨导的牛顿线性化：I(vd) ≈ geff·vd + Ieq。"""
        for m, (nop, non, ncp, ncn, gm, vk, lb) in enumerate(self.GT):
            vd = vgt[m]
            t = math.tanh(vd / vk)
            geff = gm * (1.0 - t * t)
            i0 = gm * vk * t
            ieq = i0 - geff * vd
            self._stamp_vccs(A, nop, non, ncp, ncn, geff)
            i, j = self.node(nop), self.node(non)
            if i: b[i - 1] -= ieq
            if j: b[j - 1] += ieq

    def _gv(self, x, e):
        nop, non, ncp, ncn, gm, vk, lb = e
        a, c = self.node(ncp), self.node(ncn)
        return (x[a - 1] if a else 0.0) - (x[c - 1] if c else 0.0)

    def _stamp_diodes(self, A, b, vd, gmin):
        for m, (na, nk, Is, nn_, Rs, lb) in enumerate(self.D):
            v = vd[m]
            vt = nn_ * 0.025852
            # 软限幅（SPICE pnjlim 的连续版）：把 exp 的自变量压到溢出区之外，
            # 牛顿才不会在 5V 那一步直接跳成 2353A 再把解震荡出去。
            vcrit = vt * math.log(vt / (math.sqrt(2.0) * Is))
            if v > vcrit:
                arg = 1.0 + (v - vcrit) / vt
                vlim = vcrit + vt * math.log(arg) if arg > 0 else vcrit
            else:
                vlim = v
            ex = math.exp(min(vlim / vt, 80.0))
            id_ = Is * (ex - 1.0)
            gd = Is * ex / vt
            ge = gd / (1.0 + gd * Rs)
            ieq = id_ - ge * vlim          # ★ 必须用限幅后的电压，否则线性化点不自洽
            self._stamp_g(A, na, nk, ge + gmin)
            i, j = self.node(na), self.node(nk)
            if i: b[i - 1] -= ieq
            if j: b[j - 1] += ieq

    def _stamp_switches(self, A, t, dt, is_dc=False):
        for (n1, n2, r_on, r_off, ctrl, lb) in self.SW:
            on = ctrl(t) if callable(ctrl) else bool(ctrl)
            self._stamp_g(A, n1, n2, 1.0 / (r_on if on else r_off))

    # ================= 瞬态 =================
    def tran(self, tstop, dt, tstart=0.0, method="trap", ic_vc=None, ic_il=None,
             ic_vnode=None):
        """
        瞬态。method: 'trap'（梯形）| 'be'（后向欧拉）。
        ic_vc  : {电容 label: 初始电压}   —— 覆盖 DC 初值（ADC 保持电容等）
        ic_il  : {电感 label: 初始电流}
        ic_vnode: {节点名: 初始电压}      —— 直接给节点初值（最省事的用法）
        返回 (t数组, {节点名: 电压数组})。
        """
        nstep = int(round((tstop - tstart) / dt))
        N = self._size()
        # 状态
        vC = np.zeros(len(self.C)); iC = np.zeros(len(self.C))
        iL = np.zeros(len(self.L)); vL = np.zeros(len(self.L))
        vD = np.zeros(len(self.D))
        vGT = np.zeros(len(self.GT))
        d0 = self.dc()
        x = getattr(self, "_dc_x", np.zeros(N)).copy()
        if self.GT:
            vGT = np.array([self._gv(x, e) for e in self.GT])
        if ic_vnode:
            for nm, val in ic_vnode.items():
                k = self.node(nm)
                if k:
                    x[k - 1] = val
        for m, (n1, n2, f, lb) in enumerate(self.C):
            i, j = self.node(n1), self.node(n2)
            vC[m] = (x[i - 1] if i else 0.0) - (x[j - 1] if j else 0.0)
        if ic_vc:
            for m, (n1, n2, f, lb) in enumerate(self.C):
                if lb in ic_vc:
                    vC[m] = ic_vc[lb]
        if ic_il:
            for m, (n1, n2, h, lb) in enumerate(self.L):
                if lb in ic_il:
                    iL[m] = ic_il[lb]
        else:
            for m in range(len(self.L)):
                r = self.nn - 1 + len(self.V) + len(self.E) + m
                iL[m] = x[r]

        ts = np.zeros(nstep + 1)
        vs = {nm: np.zeros(nstep + 1) for nm in self._nodename}
        for k, nm in enumerate(self._nodename):
            vs[nm][0] = 0.0 if k == 0 else x[k - 1]

        prev_switch = None
        for s in range(1, nstep + 1):
            t = tstart + s * dt
            ts[s] = t
            # 开关状态变化 → 该步用后向欧拉（避免梯形对不连续量的振铃）
            sw_now = tuple(bool(c(t) if callable(c) else c) for (_, _, _, _, c, _) in self.SW)
            # 第一步恒用后向欧拉：初值处导数可能不连续（阶跃 / 开关闭合），
            # 梯形法对不连续量会引入 O(h) 的启动误差。
            use_be = (method == "be") or (s == 1) or \
                     (prev_switch is not None and sw_now != prev_switch)
            prev_switch = sw_now

            for _ in range(60):   # 牛顿（二极管）
                A = np.zeros((N, N)); b = np.zeros(N)
                self._Lslot = 0
                # --- 线性部分 ---
                for (n1, n2, ohm, lb) in self.R:
                    self._stamp_g(A, n1, n2, 1.0 / ohm)
                for m, (n1, n2, f, lb) in enumerate(self.C):
                    if use_be:
                        geq = f / dt
                        ieq = -geq * vC[m]
                    else:
                        geq = 2.0 * f / dt
                        ieq = -geq * vC[m] - iC[m]
                    self._stamp_g(A, n1, n2, geq)
                    i, j = self.node(n1), self.node(n2)
                    if i: b[i - 1] -= ieq
                    if j: b[j - 1] += ieq
                for m, (n1, n2, h, lb) in enumerate(self.L):
                    if use_be:
                        req = h / dt; veq = -req * iL[m]
                    else:
                        req = 2.0 * h / dt; veq = -req * iL[m] - vL[m]
                    r = self.nn - 1 + len(self.V) + len(self.E) + m
                    i, j = self.node(n1), self.node(n2)
                    if i: A[i - 1, r] += 1.0
                    if j: A[j - 1, r] -= 1.0
                    if i: A[r, i - 1] += 1.0
                    if j: A[r, j - 1] -= 1.0
                    A[r, r] -= req
                    b[r] += veq
                bi = self._bidx()
                for m, (np_, nn_, dc, lb, am, ap) in enumerate(self.V):
                    wv = self.VWAVE[m] if m < len(self.VWAVE) else None
                    val = wv(t) if wv is not None else dc
                    self._stamp_vsrc(A, b, bi[("V", m)], np_, nn_, val)
                for m, (np_, nn_, dc, lb, am, ap) in enumerate(self.I):
                    self._stamp_isrc(b, np_, nn_, dc)
                for m, (nop, non, ncp, ncn, gain, lb) in enumerate(self.E):
                    r = bi[("E", m)]
                    self._stamp_vsrc(A, b, r, nop, non, 0.0)
                    self._stamp_ctrl(A, r, ncp, ncn, gain)
                for m, (nop, non, ncp, ncn, gm, lb) in enumerate(self.Gs):
                    self._stamp_vccs(A, nop, non, ncp, ncn, gm)
                self._stamp_diodes(A, b, vD, 1e-12)
                self._stamp_gtanh(A, b, vGT)
                self._stamp_switches(A, t, dt)
                xn = np.linalg.solve(A, b)
                if not self.D and not self.GT:
                    x = xn; break
                dv = np.max(np.abs(np.array([self._dv(xn, d) for d in self.D]) - vD)) if self.D else 0.0
                dg = np.max(np.abs(np.array([self._gv(xn, e) for e in self.GT]) - vGT)) if self.GT else 0.0
                x = xn
                if self.D:
                    vD = np.array([self._dv(xn, d) for d in self.D])
                if self.GT:
                    vGT = np.array([self._gv(xn, e) for e in self.GT])
                if max(dv, dg) < 1e-9:
                    break

            # --- 更新状态 ---
            for m, (n1, n2, f, lb) in enumerate(self.C):
                i, j = self.node(n1), self.node(n2)
                v = (x[i - 1] if i else 0.0) - (x[j - 1] if j else 0.0)
                if use_be:
                    iC[m] = f / dt * (v - vC[m])
                else:
                    iC[m] = 2.0 * f / dt * (v - vC[m]) - iC[m]
                vC[m] = v
            for m, (n1, n2, h, lb) in enumerate(self.L):
                r = self.nn - 1 + len(self.V) + len(self.E) + m
                i, j = self.node(n1), self.node(n2)
                v = (x[i - 1] if i else 0.0) - (x[j - 1] if j else 0.0)
                iL[m] = x[r]
                vL[m] = v
            for k, nm in enumerate(self._nodename):
                vs[nm][s] = 0.0 if k == 0 else x[k - 1]
        return ts, vs

    # ---------- 便捷 ----------
    def vdiff(self, res, n1, n2):
        return res[n1] - res[n2]


def mag_db(x):
    return 20 * np.log10(np.maximum(np.abs(x), 1e-30))
