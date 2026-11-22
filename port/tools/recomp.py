#!/usr/bin/env python3
"""Static recompiler: Allegrex (PSP MIPS) machine code -> C.

Inputs:
  generated/image.bin, generated/meta.json   (prx_load.py)
  re/export/program.json                      (Ghidra: functions, jump tables)
Outputs (in generated/):
  funcs.h          prototypes for every recompiled function
  code_NNN.c       function bodies
  func_table.c     sorted address -> function table (indirect calls)
  hle.h / hle_stubs.c  wrappers for the 202 imported PSP OS functions
  recomp_report.txt    anything the translator could not handle

  python3 recomp.py PORT_DIR PROGRAM_JSON
"""
import bisect
import json
import struct
import sys
from collections import defaultdict
from pathlib import Path

FUNCS_PER_FILE = 250
HOOKS = set()  # function entries replaced by hand-written code (port/hooks.txt)
OWN = set()    # overlay mode: the overlay's own function entries (named PREFIX + address)
PREFIX = "f_"


def fname(a):
    """The C name of the function at a: an overlay's own ones o<N>_<addr>, the rest f_<addr>."""
    return f"{PREFIX}{a:08x}" if a in OWN else f"f_{a:08x}"


class Ctx:
    def __init__(self, img, base):
        self.img, self.base = img, base

    def word(self, a):
        return struct.unpack_from("<I", self.img, a - self.base)[0]


def simm(op):
    v = op & 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def R(n):
    return "0u" if n == 0 else f"c->r[{n}]"


def W(n, expr):
    return "" if n == 0 else f"c->r[{n}] = {expr};"


def addr_expr(op):
    rs, imm = (op >> 21) & 31, simm(op)
    if rs == 0:
        return f"0x{imm & 0xFFFFFFFF:x}u"
    return f"({R(rs)} + {imm}u)" if imm >= 0 else f"({R(rs)} - {-imm}u)"


def emit_simple(op, pc, unknown):
    """C for one non-control-flow instruction (no delay-slot semantics)."""
    o = op >> 26
    rs, rt, rd, sa, fn = (op >> 21) & 31, (op >> 16) & 31, (op >> 11) & 31, (op >> 6) & 31, op & 63
    if op == 0:
        return ""
    if o == 0:
        if fn == 0x00: return W(rd, f"{R(rt)} << {sa}")
        if fn == 0x02:
            if rs == 1: return W(rd, f"({R(rt)} >> {sa}) | ({R(rt)} << {(32 - sa) & 31})" if sa else R(rt))
            return W(rd, f"{R(rt)} >> {sa}")
        if fn == 0x03: return W(rd, f"(uint32_t)((int32_t){R(rt)} >> {sa})")
        if fn == 0x04: return W(rd, f"{R(rt)} << ({R(rs)} & 31)")
        if fn == 0x06:
            if sa == 1: return W(rd, f"({R(rt)} >> ({R(rs)} & 31)) | ({R(rt)} << ((32 - ({R(rs)} & 31)) & 31))")
            return W(rd, f"{R(rt)} >> ({R(rs)} & 31)")
        if fn == 0x07: return W(rd, f"(uint32_t)((int32_t){R(rt)} >> ({R(rs)} & 31))")
        if fn == 0x0A: return f"if ({R(rt)} == 0) {{ {W(rd, R(rs))} }}" if rd else ""
        if fn == 0x0B: return f"if ({R(rt)} != 0) {{ {W(rd, R(rs))} }}" if rd else ""
        if fn == 0x0D: return f"rt_break(c, 0x{pc:08x}u, 0x{(op >> 6) & 0xFFFFF:x}u);"
        if fn == 0x0F: return ""  # sync
        if fn == 0x10: return W(rd, "c->hi")
        if fn == 0x11: return f"c->hi = {R(rs)};"
        if fn == 0x12: return W(rd, "c->lo")
        if fn == 0x13: return f"c->lo = {R(rs)};"
        if fn == 0x16: return W(rd, f"({R(rs)} ? (uint32_t)__builtin_clz({R(rs)}) : 32u)")
        if fn == 0x17: return W(rd, f"(~{R(rs)} ? (uint32_t)__builtin_clz(~{R(rs)}) : 32u)")
        if fn in (0x18, 0x19, 0x1C, 0x1D, 0x2E, 0x2F):
            signed = fn in (0x18, 0x1C, 0x2E)
            prod = (f"(int64_t)(int32_t){R(rs)} * (int64_t)(int32_t){R(rt)}" if signed
                    else f"(uint64_t){R(rs)} * (uint64_t){R(rt)}")
            if fn in (0x18, 0x19):
                return f"{{ uint64_t p = (uint64_t)({prod}); c->lo = (uint32_t)p; c->hi = (uint32_t)(p >> 32); }}"
            sign = "+" if fn in (0x1C, 0x1D) else "-"
            return (f"{{ uint64_t acc = ((uint64_t)c->hi << 32) | c->lo; acc = acc {sign} (uint64_t)({prod}); "
                    f"c->lo = (uint32_t)acc; c->hi = (uint32_t)(acc >> 32); }}")
        if fn == 0x1A: return f"rt_div(c, {R(rs)}, {R(rt)});"
        if fn == 0x1B: return f"rt_divu(c, {R(rs)}, {R(rt)});"
        if fn in (0x20, 0x21): return W(rd, f"{R(rs)} + {R(rt)}")
        if fn in (0x22, 0x23): return W(rd, f"{R(rs)} - {R(rt)}")
        if fn == 0x24: return W(rd, f"{R(rs)} & {R(rt)}")
        if fn == 0x25: return W(rd, f"{R(rs)} | {R(rt)}")
        if fn == 0x26: return W(rd, f"{R(rs)} ^ {R(rt)}")
        if fn == 0x27: return W(rd, f"~({R(rs)} | {R(rt)})")
        if fn == 0x2A: return W(rd, f"(uint32_t)((int32_t){R(rs)} < (int32_t){R(rt)})")
        if fn == 0x2B: return W(rd, f"(uint32_t)({R(rs)} < {R(rt)})")
        if fn == 0x2C: return W(rd, f"((int32_t){R(rs)} > (int32_t){R(rt)} ? {R(rs)} : {R(rt)})")
        if fn == 0x2D: return W(rd, f"((int32_t){R(rs)} < (int32_t){R(rt)} ? {R(rs)} : {R(rt)})")
    if o in (0x08, 0x09): return W(rt, f"{R(rs)} + {simm(op) & 0xFFFFFFFF:#x}u")
    if o == 0x0A: return W(rt, f"(uint32_t)((int32_t){R(rs)} < {simm(op)})")
    if o == 0x0B: return W(rt, f"(uint32_t)({R(rs)} < {simm(op) & 0xFFFFFFFF:#x}u)")
    if o == 0x0C: return W(rt, f"{R(rs)} & {op & 0xFFFF:#x}u")
    if o == 0x0D: return W(rt, f"{R(rs)} | {op & 0xFFFF:#x}u")
    if o == 0x0E: return W(rt, f"{R(rs)} ^ {op & 0xFFFF:#x}u")
    if o == 0x0F: return W(rt, f"{(op & 0xFFFF) << 16:#x}u")
    a = addr_expr(op)
    if o == 0x20: return W(rt, f"S8({a})")
    if o == 0x21: return W(rt, f"S16({a})")
    if o in (0x23, 0x30): return W(rt, f"M32({a})")
    if o == 0x24: return W(rt, f"M8({a})")
    if o == 0x25: return W(rt, f"M16({a})")
    if o == 0x22: return f"rt_lwl(c, {rt}, {a});" if rt else ""
    if o == 0x26: return f"rt_lwr(c, {rt}, {a});" if rt else ""
    if o == 0x28: return f"M8({a}) = (uint8_t){R(rt)};"
    if o == 0x29: return f"M16({a}) = (uint16_t){R(rt)};"
    if o == 0x2B: return f"M32({a}) = {R(rt)};"
    if o == 0x38: return f"M32({a}) = {R(rt)}; {W(rt, '1u')}"
    if o == 0x2A: return f"rt_swl(c, {rt}, {a});"
    if o == 0x2E: return f"rt_swr(c, {rt}, {a});"
    if o == 0x2F: return ""  # cache
    if o == 0x31: return f"c->fpr.u[{rt}] = M32({a});"
    if o == 0x39: return f"M32({a}) = c->fpr.u[{rt}];"
    if o == 0x1F:
        if fn == 0x00:  # ext
            size = rd + 1
            mask = (1 << size) - 1 if size < 32 else 0xFFFFFFFF
            return W(rt, f"({R(rs)} >> {sa}) & {mask:#x}u")
        if fn == 0x04:  # ins
            size = rd - sa + 1
            mask = ((1 << size) - 1) << sa & 0xFFFFFFFF
            return W(rt, f"({R(rt)} & {~mask & 0xFFFFFFFF:#x}u) | (({R(rs)} << {sa}) & {mask:#x}u)")
        if fn == 0x20:
            if sa == 0x02: return W(rd, f"((({R(rt)} & 0xff00ff00u) >> 8) | (({R(rt)} & 0x00ff00ffu) << 8))")
            if sa == 0x03: return W(rd, f"__builtin_bswap32({R(rt)})")
            if sa == 0x10: return W(rd, f"(uint32_t)(int32_t)(int8_t){R(rt)}")
            if sa == 0x14: return W(rd, f"rt_bitrev({R(rt)})")
            if sa == 0x18: return W(rd, f"(uint32_t)(int32_t)(int16_t){R(rt)}")
    if o == 0x11:
        fs, ft, fd = rd, rt, sa
        if rs == 0x00: return W(rt, f"c->fpr.u[{fs}]")
        if rs == 0x04: return f"c->fpr.u[{fs}] = {R(rt)};"
        if rs == 0x02: return W(rt, "c->fcr31") if fs == 31 else W(rt, "0x3351u")
        if rs == 0x06: return f"c->fcr31 = {R(rt)};" if fs == 31 else ""
        if rs == 0x10:
            F = lambda n: f"c->fpr.f[{n}]"
            if fn == 0x00: return f"{F(fd)} = {F(fs)} + {F(ft)};"
            if fn == 0x01: return f"{F(fd)} = {F(fs)} - {F(ft)};"
            if fn == 0x02: return f"{F(fd)} = {F(fs)} * {F(ft)};"
            if fn == 0x03: return f"{F(fd)} = {F(fs)} / {F(ft)};"
            if fn == 0x04: return f"{F(fd)} = sqrtf({F(fs)});"
            if fn == 0x05: return f"c->fpr.u[{fd}] = c->fpr.u[{fs}] & 0x7fffffffu;"
            if fn == 0x06: return f"c->fpr.u[{fd}] = c->fpr.u[{fs}];"
            if fn == 0x07: return f"c->fpr.u[{fd}] = c->fpr.u[{fs}] ^ 0x80000000u;"
            if fn in (0x0C, 0x0D, 0x0E, 0x0F):
                return f"c->fpr.u[{fd}] = rt_f2i({F(fs)}, {fn - 0x0C});"
            if fn == 0x24:  # cvt.w.s uses the current rounding mode (same numbering as rt_f2i)
                return f"c->fpr.u[{fd}] = rt_f2i({F(fs)}, (int)(c->fcr31 & 3));"
            if 0x30 <= fn <= 0x3F:
                cond = fn & 7
                parts = []
                if cond & 1: parts.append(f"(isnan({F(fs)}) || isnan({F(ft)}))")
                if cond & 2: parts.append(f"({F(fs)} == {F(ft)})")
                if cond & 4: parts.append(f"({F(fs)} < {F(ft)})")
                expr = " || ".join(parts) if parts else "0"
                return f"c->fcr31 = ({expr}) ? (c->fcr31 | 0x800000u) : (c->fcr31 & ~0x800000u);"
        if rs == 0x14 and fn == 0x20:
            return f"c->fpr.f[{fd}] = (float)(int32_t)c->fpr.u[{fs}];"
    # The VFPU forms run most often, as C (the same as runtime/vfpu.c does them): loads and stores
    # (lv.s/sv.s/lv.q/sv.q, which prefixes don't touch), the prefixes themselves and vnop/vflush/
    # vsync. Every other VFPU encoding (COP2 moves, arithmetic) goes to the runtime.
    if o in (0x32, 0x3A, 0x36, 0x3E):
        quad = o in (0x36, 0x3E)
        reg = ((op >> 16) & 0x1F) | (((op & 1) if quad else (op & 3)) << 5)
        idx = vfpu_regs(reg, 4 if quad else 1)
        imm = op & 0xFFFC
        imm = imm - 0x10000 if imm & 0x8000 else imm
        base = f"c->r[{(op >> 21) & 31}] + {imm & 0xFFFFFFFF:#x}u"
        load = o in (0x32, 0x36)
        parts = [f"VLD({k}, a_ + {4 * i}u);" if load else f"VST({k}, a_ + {4 * i}u);" for i, k in enumerate(idx)]
        return "{ uint32_t a_ = " + base + "; " + " ".join(parts) + " }"
    if o == 0x3F:
        return "/* vnop / vflush / vsync */"
    if o == 0x37 and ((op >> 23) & 7) < 6:
        sub = (op >> 23) & 7
        which, val = (0, op & 0xFFFFF) if sub < 2 else (1, op & 0xFFFFF) if sub < 4 else (2, op & 0xFFF)
        return f"c->vfpu_ctrl[{which}] = {val:#x}u;"
    if o == 0x12 and ((op >> 21) & 31) in (3, 7) and (op & 0xFF) < 128:  # mfv / mtv (prefixes play no part)
        rt, k = (op >> 16) & 31, vfpu_regs(op & 0xFF, 1)[0]
        if (op >> 21) & 31 == 3:
            return f"{{ uint32_t t_; memcpy(&t_, &c->v[{k}], 4); c->r[{rt}] = t_; }}" if rt else "/* mfv to $zero */"
        return f"memcpy(&c->v[{k}], &c->r[{rt}], 4);"
    pfx = vfpu_prefixes_before(pc)
    fast = vfpu_fast(op, pfx)
    if fast and pfx == VPFX_NONE:  # arithmetic as C while no prefix is set (else the runtime applies them)
        return f"VFAST(0x{pc:08x}u, 0x{op:08x}u, {{ {fast} }});"
    if fast:  # with the prefixes the vpfx instructions right before set, when they are those as it runs
        return f"VFASTP(0x{pc:08x}u, 0x{op:08x}u, {pfx[0]:#x}u, {pfx[1]:#x}u, {pfx[2]:#x}u, {{ {fast} }});"
    if o in (0x12, 0x18, 0x19, 0x1B, 0x32, 0x34, 0x35, 0x36, 0x37, 0x3A, 0x3C, 0x3D, 0x3E, 0x3F):
        return f"rt_vfpu(c, 0x{pc:08x}u, 0x{op:08x}u);"
    unknown.append(f"{pc:08x} {op:08x} unhandled instruction")
    return f"rt_break(c, 0x{pc:08x}u, 0xdead);"


def vfpu_regs(reg, n):
    """Storage indices (v[mtx*16 + col*4 + row]) of an n-element VFPU vector register: runtime/vfpu.c vregs()."""
    mtx, col, transpose = (reg >> 2) & 7, reg & 3, (reg >> 5) & 1
    if n == 1:
        transpose, row = 0, (reg >> 5) & 3
    elif n == 3:
        row = (reg >> 6) & 1
    else:
        row = (reg >> 5) & 2
    at = lambda m, cc, r: m * 16 + (cc & 3) * 4 + (r & 3)
    return [at(mtx, row + i, col) if transpose else at(mtx, col, row + i) for i in range(n)]


def vfpu_mat(reg, n):
    """Storage indices of an n x n VFPU matrix register, as m[j*4 + i] (column j, row i): runtime/vfpu.c mat_index()."""
    transpose = (reg >> 5) & 1
    if n == 1:
        transpose, row = 0, (reg >> 5) & 3
    elif n == 3:
        row = (reg >> 6) & 1
    else:
        row = (reg >> 5) & 2
    mtx, col = (reg >> 2) & 7, reg & 3
    at = lambda m, cc, r: m * 16 + (cc & 3) * 4 + (r & 3)
    idx = [None] * 16
    for j in range(n):
        for i in range(n):
            idx[j * 4 + i] = at(mtx, row + i, col + j) if transpose else at(mtx, col + j, row + i)
    return idx


def VMMUL(MS, MT, MD, n, V, store):
    """vmmul: each element the hardware's exact dot product (runtime/vfpu.c rt_vdot4)."""
    dst, exprs = [], []
    for a in range(n):
        for b in range(n):
            dst.append(MD[a * 4 + b])
            xs = [V(MS[b * 4 + k]) if k < n else "0.f" for k in range(4)]
            ys = [V(MT[a * 4 + k]) if k < n else "0.f" for k in range(4)]
            exprs.append(f"rt_vdot4({', '.join(xs)}, {', '.join(ys)})")
    return store(dst, exprs)


CTX = None  # the program being translated (translate sets it): vfpu_prefixes_before reads back from pc


def vfpu_half_bits(h):
    """The float bits of a half float, as runtime/vfpu.c half_to_float makes them."""
    sgn, e, m = (h >> 15) << 31, (h >> 10) & 31, h & 0x3FF
    if e == 0:
        if not m:
            return sgn
        return struct.unpack("<I", struct.pack("<f", (-1.0 if sgn else 1.0) * m * 2.0 ** -24))[0]
    if e == 31:
        return sgn | 0x7F800000 | (m << 13)
    return sgn | ((e + 112) << 23) | (m << 13)


VPFX_NONE = (0xE4, 0xE4, 0)  # the prefixes when none is set: S and T the identity swizzle, D nothing


def vfpu_prefixes_before(pc):
    """The prefixes that the vpfxs / vpfxt / vpfxd right before pc set (the nearest of each kind, up to
    three back), the others as when none is set. Only a guess: the code checks it as it runs
    (VFASTP), something may jump in between."""
    ps, pt, pd = VPFX_NONE
    seen = set()
    for back in (4, 8, 12):
        try:
            op = CTX.word(pc - back)
        except Exception:
            break
        if op >> 26 != 0x37 or ((op >> 23) & 7) >= 6:
            break
        kind = ((op >> 23) & 7) >> 1  # 0 S, 1 T, 2 D
        if kind in seen:
            continue
        seen.add(kind)
        if kind == 0:
            ps = op & 0xFFFFF
        elif kind == 1:
            pt = op & 0xFFFFF
        else:
            pd = op & 0xFFF
    return ps, pt, pd


VCST = ["0.f", "3.40282347e38f", "1.41421356f", "0.70710678f", "1.12837917f", "0.63661977f", "0.31830989f",
        "0.78539816f", "1.57079633f", "3.14159265f", "2.71828183f", "1.44269504f", "0.43429448f", "0.69314718f",
        "2.30258509f", "6.28318531f", "0.52359878f", "0.30103000f", "3.32192809f", "0.86602540f"]
PFX_CONSTS = ["0.f", "1.f", "2.f", "0.5f", "3.f", "(1.f / 3.f)", "0.25f", "(1.f / 6.f)"]


def vfpu_fast(op, pfx=VPFX_NONE):
    """C for the common VFPU forms, computed exactly as runtime/vfpu.c does (same operations in the
    same order, results stored after all reads), with the prefixes pfx (S, T, D) applied as it
    applies them (apply_st, write_vec); None for the forms left to the runtime."""
    ps, pt, pd = pfx
    plain = pfx == VPFX_NONE
    o = op >> 26
    vd, vs, vt = op & 0x7F, (op >> 8) & 0x7F, (op >> 16) & 0x7F
    n = (((op >> 7) & 1) | ((op >> 14) & 2)) + 1
    V = lambda k: f"c->v[{k}]"

    def lane(regs, p, i, m=None):  # lane i of a source with prefix p (apply_st: over its m lanes)
        m = n if m is None else m
        if p == 0xE4:
            return V(regs[i])
        sel, ab, cst, neg = (p >> (2 * i)) & 3, (p >> (8 + i)) & 1, (p >> (12 + i)) & 1, (p >> (16 + i)) & 1
        if cst:
            e = PFX_CONSTS[sel + (ab << 2)]
        else:
            e = V(regs[sel]) if sel < m else "0.f"
            if ab:
                e = f"fabsf({e})"
        return f"(-{e})" if neg else e

    def store(dst, exprs):  # the D prefix's saturation and write mask (write_vec)
        tmps = " ".join(f"float d{i}_ = {e};" for i, e in enumerate(exprs))
        writes = []
        for i, k in enumerate(dst):
            sat, mask = (pd >> (2 * i)) & 3, (pd >> (8 + i)) & 1
            if mask:
                continue
            x = f"d{i}_"
            if sat == 1:
                x = f"({x} < 0 ? 0.f : ({x} > 1 ? 1.f : {x}))"
            elif sat == 3:
                x = f"({x} < -1 ? -1.f : ({x} > 1 ? 1.f : {x}))"
            writes.append(f"{V(k)} = {x};")
        return tmps + " " + " ".join(writes)

    def store_bits(dst, exprs):  # 32-bit patterns into registers (no prefix: see the callers)
        tmps = " ".join(f"uint32_t d{i}_ = {e};" for i, e in enumerate(exprs))
        return tmps + " " + " ".join(f"memcpy(&{V(k)}, &d{i}_, 4);" for i, k in enumerate(dst))

    def elementwise(fmt):
        S, T, D = vfpu_regs(vs, n), vfpu_regs(vt, n), vfpu_regs(vd, n)
        return store(D, [fmt.format(s=lane(S, ps, i), t=lane(T, pt, i)) for i in range(n)])

    def chain(start, terms):  # ((start + a) + b) ... as float, like d += x
        e = start
        for t in terms:
            e = f"({e} + {t})"
        return e

    B = lambda k: f"rt_vbits(c, {k})"  # a register's bits (cpu.h)
    sub = (op >> 23) & 7
    if o == 0x37 and sub in (6, 7):  # viim / vfim: a constant (an integer, a half float)
        k = vfpu_regs((op >> 16) & 0x7F, 1)[0]
        if sub == 6:
            val = float(op & 0xFFFF if not op & 0x8000 else (op & 0xFFFF) - 0x10000)
            bits = struct.unpack("<I", struct.pack("<f", val))[0]
        else:
            bits = vfpu_half_bits(op & 0xFFFF)
        if pd == 0:
            return store_bits([k], [f"{bits:#010x}u"])
        f = struct.unpack("<f", struct.pack("<I", bits))[0]
        if f != f or f in (float("inf"), float("-inf")):
            return None
        return store([k], [f"{float.hex(f)}f" if f else ("-0.f" if bits >> 31 else "0.f")])
    if o == 0x1B and sub == 0:  # vcmp: each lane's condition into CC bits 0-3, any (bit 4), all (bit 5)
        cond = op & 0xF
        S, T = vfpu_regs(vs, n), vfpu_regs(vt, n)
        forms = {0: "0", 1: "{s} == {t}", 2: "{s} < {t}", 3: "{s} <= {t}", 4: "1", 5: "{s} != {t}", 6: "{s} >= {t}",
                 7: "{s} > {t}", 8: "{s} == 0.f", 9: "isnan({s})", 10: "isinf({s})", 11: "(isnan({s}) || isinf({s}))",
                 12: "{s} != 0.f", 13: "!isnan({s})", 14: "!isinf({s})", 15: "!(isnan({s}) || isinf({s}))"}
        lanes = " ".join(f"uint32_t b{i}_ = ({forms[cond].format(s=lane(S, ps, i), t=lane(T, pt, i))}) ? 1u : 0u;"
                         for i in range(n))
        bits = " | ".join(f"b{i}_ << {i}" for i in range(n))
        return (f"{lanes} c->vfpu_ctrl[VFPU_CTRL_CC] = (c->vfpu_ctrl[VFPU_CTRL_CC] & ~0x3Fu) | {bits} | "
                f"({' | '.join(f'b{i}_' for i in range(n))}) << 4 | ({' & '.join(f'b{i}_' for i in range(n))}) << 5;")
    if o == 0x34 and ((op >> 21) & 31) == 1 and ((op >> 16) & 31) >= 24:  # packing and unpacking integers
        if not plain:
            return None
        fn = (op >> 16) & 31
        S = vfpu_regs(vs, n)
        u = [B(S[i]) if i < n else "0u" for i in range(4)]
        if fn in (24, 25):  # vuc2i / vc2i: each byte of the first to the top of a word
            w = [f"(({u[0]} >> {8 * i}) & 0xFFu) * 0x01010101u >> 1" if fn == 24 else f"(({u[0]} >> {8 * i}) & 0xFFu) << 24"
                 for i in range(4)]
        elif fn in (26, 27):  # vus2i / vs2i: each halfword to the top of a word
            w = []
            for i in range(2 if n >= 2 else 1):
                w += [f"({u[i]} & 0xFFFFu) << 15", f"({u[i]} & 0xFFFF0000u) >> 1"] if fn == 26 else \
                     [f"({u[i]} & 0xFFFFu) << 16", f"{u[i]} & 0xFFFF0000u"]
        elif fn in (28, 29):  # vi2uc / vi2c: four words to their top bytes
            parts = [f"((uint32_t)(((int32_t){u[i]} < 0 ? 0 : (int32_t){u[i]}) >> 23) & 0xFFu) << {8 * i}" if fn == 28
                     else f"({u[i]} >> 24) << {8 * i}" for i in range(4)]
            w = [" | ".join(parts)]
        else:  # vi2us / vi2s: pairs of words to halfwords
            w = []
            for i in range((n + 1) // 2):
                lo, hi = u[2 * i], u[2 * i + 1]
                w.append(f"((uint32_t)(((int32_t){lo} < 0 ? 0 : (int32_t){lo}) >> 15) & 0xFFFFu) | "
                         f"((uint32_t)(((int32_t){hi} < 0 ? 0 : (int32_t){hi}) >> 15) & 0xFFFFu) << 16" if fn == 30
                         else f"({lo} >> 16) | ({hi} & 0xFFFF0000u)")
        return store_bits(vfpu_regs(vd, len(w)), w)
    if o == 0x34 and 16 <= ((op >> 21) & 31) <= 20:  # vf2in / vf2iz / vf2iu / vf2id, vi2f: scaled by 2^imm
        if not plain:
            return None
        kind, imm = (op >> 21) & 31, (op >> 16) & 31
        S, D = vfpu_regs(vs, n), vfpu_regs(vd, n)
        if kind == 20:  # exact: an int's float times a power of two stays normal (ldexpf's result)
            return store(D, [f"(float)(int32_t){B(S[i])} * 0x1p-{imm}f" for i in range(n)])
        return store_bits(D, [f"rt_vf2i({V(S[i])}, {imm}, {kind - 16})" for i in range(n)])
    if o == 0x34 and ((op >> 21) & 31) == 3:  # vcst: a constant in every lane
        fn = (op >> 16) & 31
        return store(vfpu_regs(vd, n), [VCST[fn] if fn < 20 else "0.f" for _ in range(n)])
    if o == 0x34 and ((op >> 21) & 31) == 21:  # vcmov: s (S prefix) where the condition holds, else d (T prefix)
        tf, imm3 = (op >> 19) & 1, ((op >> 16) & 31) & 7
        S, D = vfpu_regs(vs, n), vfpu_regs(vd, n)
        cc = "c->vfpu_ctrl[VFPU_CTRL_CC]"
        exprs = []
        for i in range(n):
            s_, d_ = lane(S, ps, i), lane(D, pt, i)
            if imm3 < 6:
                exprs.append(f"(({cc} >> {imm3}) & 1u) == {int(not tf)}u ? {s_} : {d_}")
            elif imm3 == 6:
                exprs.append(f"(({cc} >> {i}) & 1u) == {int(not tf)}u ? {s_} : {d_}")
            else:
                exprs.append(d_)
        return store(D, exprs)
    if o == 0x3C and ((op >> 21) & 31) == 29:  # vrot: a quarter-turn angle's sine and cosine into chosen lanes
        imm = (op >> 16) & 31
        sl, cl = (imm >> 2) & 3, imm & 3
        a = lane(vfpu_regs(vs, 1), ps, 0, 1)
        sn = f"{'-' if imm & 0x10 else ''}sinf({a} * (float)M_PI_2)"
        cs = f"cosf({a} * (float)M_PI_2)"
        return store(vfpu_regs(vd, n), [cs if i == cl else sn if i == sl or sl == cl else "0.f" for i in range(n)])
    if o == 0x18 and sub in (0, 1, 7):
        return elementwise({0: "{s} + {t}", 1: "{s} - {t}", 7: "{s} / {t}"}[sub])
    if o == 0x19:
        if sub == 0:
            return elementwise("{s} * {t}")
        S, T = vfpu_regs(vs, n), vfpu_regs(vt, n)
        if sub == 1:  # vdot
            return store(vfpu_regs(vd, 1), [chain("0.f", [f"{lane(S, ps, i)} * {lane(T, pt, i)}" for i in range(n)])])
        if sub == 2:  # vscl (the runtime reads t's first lane as it is, no T prefix)
            t0 = vfpu_regs(vt, 1)[0]
            return store(vfpu_regs(vd, n), [f"{lane(S, ps, i)} * {V(t0)}" for i in range(n)])
        if sub == 4:  # vhdp
            return store(vfpu_regs(vd, 1), [chain(lane(T, pt, n - 1),
                                                  [f"{lane(S, ps, i)} * {lane(T, pt, i)}" for i in range(n - 1)])])
        return None
    if o == 0x1B and sub in (2, 3, 6, 7):
        return elementwise({2: "fminf({s}, {t})", 3: "fmaxf({s}, {t})", 6: "({s} >= {t} ? 1.f : 0.f)",
                            7: "({s} < {t} ? 1.f : 0.f)"}[sub])
    if o == 0x34 and ((op >> 21) & 31) == 0:
        fn = (op >> 16) & 31
        S, D = vfpu_regs(vs, n), vfpu_regs(vd, n)
        forms = {0x00: "{s}", 0x01: "fabsf({s})", 0x02: "-{s}", 0x06: "0.f", 0x07: "1.f", 0x10: "1.f / {s}",
                 0x16: "sqrtf({s})", 0x04: "({s} <= 0.f ? 0.f : {s} > 1.f ? 1.f : {s})",
                 0x05: "({s} <= -1.f ? -1.f : {s} > 1.f ? 1.f : {s})", 0x11: "1.f / sqrtf({s})",
                 0x12: "sinf({s} * (float)M_PI_2)", 0x13: "cosf({s} * (float)M_PI_2)", 0x14: "exp2f({s})",
                 0x15: "log2f({s})", 0x17: "asinf({s}) / (float)M_PI_2", 0x18: "-1.f / {s}"}
        if fn == 0x03:  # vidt
            one = vd & (3 if n >= 3 else 1)
            return store(D, ["1.f" if i == one else "0.f" for i in range(n)])
        if fn in forms:
            return store(D, [forms[fn].format(s=lane(S, ps, i)) for i in range(n)])
        return None
    if o == 0x3C:
        if not plain:
            return None
        idx = (op >> 21) & 31
        group = idx >> 2
        if idx == 28:  # vmmov / vmidt / vmzero / vmone
            fn = (op >> 16) & 15
            MD = vfpu_mat(vd, n)
            cells = [(j, i) for j in range(n) for i in range(n)]
            if fn == 0:
                MS = vfpu_mat(vs, n)
                return store([MD[j * 4 + i] for j, i in cells], [V(MS[j * 4 + i]) for j, i in cells])
            const = {3: None, 6: "0.f", 7: "1.f"}
            if fn not in const:
                return None
            return store([MD[j * 4 + i] for j, i in cells],
                         [("1.f" if i == j else "0.f") if fn == 3 else const[fn] for j, i in cells])
        if group == 0:  # vmmul
            return VMMUL(vfpu_mat(vs, n), vfpu_mat(vt, n), vfpu_mat(vd, n), n, V, store)
        if group in (1, 2, 3):  # vtfmN / vhtfmN
            ins = group
            MS = vfpu_mat(vs, ins + 1)
            tn = min(n, ins + 1)
            T = vfpu_regs(vt, tn)
            tv = [V(T[k]) if k < tn else "0.f" for k in range(ins + 1)]
            if n <= ins:
                tv[ins] = "1.f"
            exprs = [chain("0.f", [f"{V(MS[i * 4 + k])} * {tv[k]}" for k in range(ins + 1)]) for i in range(ins + 1)]
            return store(vfpu_regs(vd, ins + 1), exprs)
    return None


def branch_info(op, pc):
    """Return (kind, target, cond_expr, likely, link) for control-flow ops, else None."""
    o = op >> 26
    rs, rt = (op >> 21) & 31, (op >> 16) & 31
    rel = (pc + 4 + (simm(op) << 2)) & 0xFFFFFFFF
    if o == 0x02: return ("j", (pc & 0xF0000000) | ((op & 0x03FFFFFF) << 2), None, False, False)
    if o == 0x03: return ("jal", (pc & 0xF0000000) | ((op & 0x03FFFFFF) << 2), None, False, True)
    if o == 0 and (op & 63) == 0x08: return ("jr", rs, None, False, False)
    if o == 0 and (op & 63) == 0x09: return ("jalr", rs, (op >> 11) & 31, False, True)
    conds = {
        0x04: f"{R(rs)} == {R(rt)}", 0x05: f"{R(rs)} != {R(rt)}",
        0x06: f"(int32_t){R(rs)} <= 0", 0x07: f"(int32_t){R(rs)} > 0",
        0x14: f"{R(rs)} == {R(rt)}", 0x15: f"{R(rs)} != {R(rt)}",
        0x16: f"(int32_t){R(rs)} <= 0", 0x17: f"(int32_t){R(rs)} > 0",
    }
    if o in conds:
        return ("b", rel, conds[o], o >= 0x14, False)
    if o == 0x01 and rt in (0x10, 0x11, 0x12, 0x13):  # bltzal/bgezal(l): a call when the condition holds; ra is set either way
        cond = f"(int32_t){R(rs)} < 0" if rt in (0x10, 0x12) else f"(int32_t){R(rs)} >= 0"
        return ("bal", rel, cond, rt >= 0x12, True)
    if o == 0x01 and rt in (0, 1, 2, 3):
        cond = f"(int32_t){R(rs)} < 0" if rt in (0, 2) else f"(int32_t){R(rs)} >= 0"
        return ("b", rel, cond, rt >= 2, False)
    if o == 0x11 and rs == 0x08:
        cond = "(c->fcr31 & 0x800000u)" if rt & 1 else "!(c->fcr31 & 0x800000u)"
        return ("b", rel, cond, bool(rt & 2), False)
    if o == 0x12 and rs == 0x08:  # bvf/bvt/bvfl/bvtl, cc bit in (op>>18)&7
        bit = (op >> 18) & 7
        cond = f"((c->vfpu_ctrl[VFPU_CTRL_CC] >> {bit}) & 1)"
        if not (op >> 16) & 1:
            cond = "!" + cond
        return ("b", rel, cond, bool((op >> 17) & 1), False)
    return None


def flow(ctx, starts, owned, is_claimed, text_lo, text_hi):
    """Addresses reachable from starts (recursive descent), stopping at code already owned by
    this function or claimed by another one. Returns (new addresses, jal targets seen)."""
    seen, calls, work = set(), set(), list(starts)
    while work:
        pc = work.pop()
        while text_lo <= pc < text_hi and pc not in seen and pc not in owned and not is_claimed(pc):
            seen.add(pc)
            op = ctx.word(pc)
            bi = branch_info(op, pc)
            if not bi:
                pc += 4
                continue
            kind, target = bi[0], bi[1]
            seen.add(pc + 4)  # delay slot
            o, rs, rt = op >> 26, (op >> 21) & 31, (op >> 16) & 31
            always = kind == "j" or (o == 0x04 and rs == rt) or (o == 0x01 and rs == 0 and rt in (1, 3))
            if kind in ("b", "j", "bal"):
                work.append(target)
            if kind in ("jal", "jalr", "bal"):
                if kind == "jal":
                    calls.add(target)
                pc += 8
                continue
            if kind == "jr" or always:
                break
            pc += 8
    return seen, calls


def to_ranges(addrs):
    out = []
    for a in sorted(addrs):
        if out and out[-1][1] + 1 == a:
            out[-1][1] = a + 3
        else:
            out.append([a, a + 3])
    return [tuple(r) for r in out]


def discover_code(ctx, funcs, imports, text_lo, text_hi):
    """Completes Ghidra's function list: code a function branches or falls into that nobody owns
    joins that function, and unclaimed targets of jal become new functions."""
    def claimed_index():
        rs = sorted((lo, hi) for f in funcs for lo, hi in f["ranges"])
        return rs, [lo for lo, _ in rs]

    def make_is_claimed(rs, starts):
        def is_claimed(a):
            i = bisect.bisect_right(starts, a) - 1
            return i >= 0 and rs[i][0] <= a <= rs[i][1]
        return is_claimed

    entries = {f["entry"] for f in funcs} | set(imports)
    added, extended = [], []
    for _ in range(8):  # new code may call or reach more new code
        rs, starts = claimed_index()
        is_claimed = make_is_claimed(rs, starts)
        pending_calls = set()
        changed = False
        for f in funcs:
            owned = {a for lo, hi in f["ranges"] for a in range(lo, hi + 1, 4)}
            seeds = []
            for pc in sorted(owned):
                bi = branch_info(ctx.word(pc), pc)
                if bi and bi[0] in ("b", "j") and bi[1] not in owned:
                    seeds.append(bi[1])
                if bi and bi[0] == "jal":
                    pending_calls.add(bi[1])
            last = max(owned)

            def always(op, bi):  # control never continues past this branch's delay slot
                return bi and (bi[0] in ("jr", "j") or (op >> 26 == 0x04 and ((op >> 21) & 31) == ((op >> 16) & 31))
                               or (op >> 26 == 0x01 and ((op >> 21) & 31) == 0 and ((op >> 16) & 31) in (1, 3)))
            prev, lop = ctx.word(last - 4), ctx.word(last)
            pb, lb = branch_info(prev, last - 4), branch_info(lop, last)
            new_tail = set()
            if lb and always(lop, lb):
                new_tail.add(last + 4)  # its delay slot only
            elif not (pb and always(prev, pb)):
                seeds.append(last + 4)  # falls through (after a conditional branch: its delay slot, then on)
            new, calls = flow(ctx, seeds, owned, is_claimed, text_lo, text_hi)
            new |= {a for a in new_tail if text_lo <= a < text_hi and not is_claimed(a)}
            pending_calls |= calls
            if new:
                f["ranges"] = to_ranges(owned | new)
                extended.append((f["entry"], 4 * len(new)))
                rs, starts = claimed_index()
                is_claimed = make_is_claimed(rs, starts)
                changed = True
        for t in sorted(pending_calls):
            if t in entries or is_claimed(t) or not text_lo <= t < text_hi:
                continue
            new, _ = flow(ctx, [t], set(), is_claimed, text_lo, text_hi)
            if new:
                funcs.append({"entry": t, "ranges": to_ranges(new)})
                entries.add(t)
                added.append(t)
                rs, starts = claimed_index()
                is_claimed = make_is_claimed(rs, starts)
                changed = True
        if not changed:
            break
    return added, extended


def find_switch_tables(ctx, text_lo, text_hi, image_end):
    """Switch tables from the code: before a computed jump (jr rX, not ra) the target is loaded
    with lw rX, lo(rB) where rB = table + index * 4 (the table's address from lui + addiu, or the
    lui's half with lw's offset); the case count from the sltiu that guards the index. Returns
    {jr address: [case targets]} for the tables whose entries all point into the code."""
    def s16(v):
        return v - 0x10000 if v & 0x8000 else v

    out = {}
    for pc in range(text_lo, text_hi, 4):
        op = ctx.word(pc)
        if op >> 26 != 0 or (op & 63) != 0x08:
            continue
        rx = (op >> 21) & 31
        if rx == 31:
            continue
        table, bound, base_reg, lo_off = None, None, None, 0
        for k in range(1, 24):  # back from the jr
            a = pc - 4 * k
            if a < text_lo:
                break
            o = ctx.word(a)
            oc, rs, rt, imm = o >> 26, (o >> 21) & 31, (o >> 16) & 31, o & 0xFFFF
            if base_reg is None:
                if oc == 0x23 and rt == rx:  # lw rx, imm(rs)
                    base_reg, lo_off, consts = rs, s16(imm), {}
                continue
            if oc == 0x0B and bound is None:  # sltiu rt, rs, N
                bound = imm
            if oc == 0x09 and imm and table is None:  # addiu rt, rs, lo  (with a lui of rs further back)
                for k2 in range(1, 12):
                    b = a - 4 * k2
                    o2 = ctx.word(b)
                    if o2 >> 26 == 0x0F and ((o2 >> 16) & 31) == rs:
                        table = ((o2 & 0xFFFF) << 16) + s16(imm) + lo_off
                        break
            if oc == 0x0F and table is None and (rt == base_reg or k < 8):  # lui rt, hi; lw's offset is the low half
                cand = ((imm << 16) + lo_off) & 0xFFFFFFFF
                if text_hi <= cand < image_end:
                    table = cand
            if table is not None and bound is not None:
                break
        if table is None or not text_hi <= table < image_end:
            continue
        if not bound or bound > 1024:
            continue  # without the guard it may be a vtable, not a switch
        targets = []
        for i in range(bound):
            t = ctx.word(table + 4 * i) if table + 4 * i + 4 <= image_end else 0
            if not (text_lo <= t < text_hi) or t & 3:
                break
            targets.append(t)
        if len(targets) == bound:
            out[pc] = targets
    return out


def code_immediates(ctx, text_lo, text_hi):
    """Code addresses the program builds in a register: lui + addiu/ori, the lui's register
    followed until something else writes it or the function returns (a compiler may keep the
    high half in a saved register for the whole function); also any addiu/ori after one of the
    last lui values of its register nearby (a switch's cases add the low half after the jump).
    Thread entries, callbacks passed as arguments, function pointers stored at run time."""
    out, lui = set(), {}
    recent = {}  # register -> the last few lui values given to it (a switch's cases add their low half after the jump)
    for a in range(text_lo, text_hi, 4):
        op = ctx.word(a)
        o, rs, rt, rd, v = op >> 26, (op >> 21) & 31, (op >> 16) & 31, (op >> 11) & 31, op & 0xFFFF
        if o == 0x0F:
            lui[rt] = v << 16
            q = recent.setdefault(rt, [])
            q.append((a, v << 16))
            del q[:-8]
            continue
        if o in (0x09, 0x0D) and rs in recent and rs not in (0, 28, 29):
            for la, hv in recent[rs]:
                if a - la <= 0x800:
                    val = (hv + ((v - 0x10000) if (o == 0x09 and v & 0x8000) else v)) & 0xFFFFFFFF
                    if text_lo <= val < text_hi and not val & 3:
                        out.add(val)
        if o in (0x09, 0x0D) and rs in lui:
            val = (lui[rs] + ((v - 0x10000) if (o == 0x09 and v & 0x8000) else v)) & 0xFFFFFFFF
            if text_lo <= val < text_hi and not val & 3:
                out.add(val)
        dest = None  # the general register this instruction writes, if any
        if o == 0:
            fn = op & 63
            if fn == 0x08:
                if rs == 31:
                    lui.clear()  # the function returns
                continue
            if fn not in (0x0C, 0x0D, 0x18, 0x19, 0x1A, 0x1B, 0x11, 0x13, 0x0F):
                dest = rd
        elif 0x08 <= o <= 0x0E or 0x20 <= o <= 0x26:
            dest = rt
        elif o == 0x03:
            dest = 31
        elif o == 0x11 and rs in (0, 2):  # mfc1, cfc1
            dest = rt
        elif o == 0x12 and rs in (0, 3):  # mfv, mfvc
            dest = rt
        elif o == 0x1F:  # ext/ins write rt, bshfl (seb, seh, wsbh, bitrev) rd
            dest = rt if (op & 63) in (0, 4) else rd
        elif o == 0x1C and (op & 63) in (0x20, 0x21, 0x02):  # clz, clo, mul
            dest = rd
        if dest:
            lui.pop(dest, None)
    return out


def share_code(ctx, funcs, imports, text_lo, text_hi, extra_entries=(), jumptables=None):
    """Branches into the middle of another function's body (code the compiler shares between
    functions, or a boundary Ghidra put in the wrong place): the code reachable from there is
    copied into the branching function, which translates it as its own (functions may overlap).
    A jal to the middle of a function makes a function there, overlapping the other one (so does
    the program's entry, extra_entries).
    Returns (functions grown, bytes copied, functions made)."""
    entries = {f["entry"] for f in funcs} | set(imports)
    never = lambda a: False

    def always(op, bi):  # control never continues past this branch's delay slot
        return bi and (bi[0] in ("jr", "j") or (op >> 26 == 0x04 and ((op >> 21) & 31) == ((op >> 16) & 31))
                       or (op >> 26 == 0x01 and ((op >> 21) & 31) == 0 and ((op >> 16) & 31) in (1, 3)))

    def exits(owned):
        """Where the code in owned can go next that owned lacks: fall-through, branch targets,
        the code after a call."""
        out = set()
        for pc in owned:
            op = ctx.word(pc)
            bi = branch_info(op, pc)
            if bi:
                out.add(pc + 4)  # its delay slot
                if bi[0] in ("b", "j", "bal"):
                    out.add(bi[1])
                if bi[0] == "jr" and jumptables and pc in jumptables:
                    out.update(jumptables[pc])  # a switch's cases
                continue
            pop = ctx.word(pc - 4)
            pbi = branch_info(pop, pc - 4)
            if pbi and pc - 4 in owned and always(pop, pbi):
                continue  # the delay slot of a jump: nothing after it
            out.add(pc + 4)
        return {a for a in out if a not in owned and text_lo <= a < text_hi}

    def grow(owned, own_entry):
        added = 0
        stop = lambda a: a in entries and a != own_entry
        for _ in range(64):
            seeds = [a for a in exits(owned) if a not in entries]
            if not seeds:
                break
            new, _ = flow(ctx, seeds, owned, stop, text_lo, text_hi)
            if not new:
                break
            owned |= new
            added += len(new)
        return added

    grown, copied, made = 0, 0, []
    calls = set()
    for f in funcs:
        owned = {a for lo, hi in f["ranges"] for a in range(lo, hi + 1, 4)}
        # A branch's delay slot is the branch's, even where Ghidra started a function at it (after a
        # call, execution goes on 8 bytes later, never back to the delay slot).
        slots = {pc + 4 for pc in owned if branch_info(ctx.word(pc), pc)} - owned
        if slots:
            owned |= slots
            f["ranges"] = to_ranges(owned)
        n = grow(owned, f["entry"])
        if n:
            f["ranges"] = to_ranges(owned)
            grown += 1
            copied += 4 * n
        for pc in owned:
            bi = branch_info(ctx.word(pc), pc)
            if bi and bi[0] == "jal":
                calls.add(bi[1])
    work = sorted(t for t in calls | set(extra_entries) if t not in entries and text_lo <= t < text_hi)
    while work:
        t = work.pop()
        if t in entries:
            continue
        owned, _ = flow(ctx, [t], set(), lambda a: a in entries and a != t, text_lo, text_hi)
        grow(owned, t)
        funcs.append({"entry": t, "ranges": to_ranges(owned)})
        entries.add(t)
        made.append(t)
        for pc in owned:
            bi = branch_info(ctx.word(pc), pc)
            if bi and bi[0] == "jal" and bi[1] not in entries and text_lo <= bi[1] < text_hi:
                work.append(bi[1])
    return grown, copied, made


def translate(ctx, func, funcs_by_entry, jumptables, report):
    global CTX
    CTX = ctx
    entry = func["entry"]
    addrs = sorted({a for lo, hi in func["ranges"] for a in range(lo, hi + 1, 4)})
    inset = set(addrs)
    unknown = []

    # Pass 1: which addresses need labels.
    labels = {entry} | set(func.get("entries", ()))
    local_rets = set()  # where a local subroutine (bal) returns to
    needs_all_labels = False
    for pc in addrs:
        bi = branch_info(ctx.word(pc), pc)
        if not bi:
            continue
        kind, target = bi[0], bi[1]
        if kind in ("b", "j") and target in inset:
            labels.add(target)
        if kind == "bal" and target in inset:  # a local subroutine (hand-written code): goto, and back by jr ra
            labels.add(target)
            if pc + 8 in inset:
                labels.add(pc + 8)
                local_rets.add(pc + 8)
        if kind == "jr" and target != 31:
            tgts = jumptables.get(pc)
            if tgts:
                labels.update(t for t in tgts if t in inset)
            else:
                needs_all_labels = True
    if needs_all_labels:
        labels.update(addrs)

    def jump_to(target, from_pc):
        if target in inset:
            return f"goto L_{target:08x};"
        if target in funcs_by_entry:
            return f"{{ {fname(target)}(c); return; }}"
        report.append(f"{entry:08x}: branch at {from_pc:08x} to {target:08x} outside function and not an entry")
        return f"rt_bad_jump(c, 0x{from_pc:08x}u, 0x{target:08x}u);"

    suffix = "_orig" if entry in HOOKS else ""
    if func.get("entries"):  # a cycle of tail calls merged: one body, entered at any of them
        out = [f"static void {func['name']}(CPU *c, uint32_t entry) {{",
               "\tswitch (entry) {" + " ".join(f"case 0x{e:08x}u: goto L_{e:08x};" for e in sorted(func["entries"])) + " }"]
    else:
        out = [f"void {fname(entry)}{suffix}(CPU *c) {{"]
        if addrs[0] != entry:
            out.append(f"\tgoto L_{entry:08x};")
    delay_slots = set()
    emitted_labels = set()

    def label(a):
        if a not in emitted_labels:
            emitted_labels.add(a)
            out.append(f"L_{a:08x}:;")

    def fall(a):  # execution runs past this function's code into a: another function's entry is a tail call
        if a in funcs_by_entry:
            return f"{{ {fname(a)}(c); return; }}"
        return f"rt_fallthrough(c, 0x{a:08x}u);"

    prev = None
    for pc in addrs:
        if prev is not None and pc != prev + 4:
            out.append(f"\t{fall(prev + 4)}")  # gap between body ranges
        prev = pc
        if pc in delay_slots:
            continue  # emitted together with its branch
        if pc in labels:
            label(pc)
        op = ctx.word(pc)
        bi = branch_info(op, pc)
        if not bi:
            s = emit_simple(op, pc, unknown)
            if s:
                out.append("\t" + s)
            continue

        kind, target, extra, likely, link = bi
        dpc = pc + 4
        dop = ctx.word(dpc)
        if branch_info(dop, dpc):
            report.append(f"{entry:08x}: branch in delay slot at {dpc:08x}")
        delay = emit_simple(dop, dpc, unknown)
        delay_slots.add(dpc)

        if kind == "b":
            if likely:
                out.append(f"\tif ({extra}) {{ {delay} {jump_to(target, pc)} }}")
            else:
                out.append(f"\t{{ int cond = ({extra}); {delay} if (cond) {jump_to(target, pc)} }}")
        elif kind == "j":
            out.append(f"\t{{ {delay} {jump_to(target, pc)} }}")
        elif kind == "jal":
            if target in funcs_by_entry:
                call = f"{fname(target)}(c);"
            else:
                report.append(f"{entry:08x}: jal at {pc:08x} to non-entry {target:08x}")
                call = f"rt_call(c, 0x{target:08x}u);"
            out.append(f"\t{{ c->r[31] = 0x{pc + 8:08x}u; {delay} {call} }}")
        elif kind == "bal" and target in inset:
            if likely:
                out.append(f"\t{{ int cond = ({extra}); c->r[31] = 0x{pc + 8:08x}u; if (cond) {{ {delay} goto L_{target:08x}; }} }}")
            else:
                out.append(f"\t{{ int cond = ({extra}); c->r[31] = 0x{pc + 8:08x}u; {delay} if (cond) goto L_{target:08x}; }}")
        elif kind == "bal":
            call = f"{fname(target)}(c);" if target in funcs_by_entry else f"rt_call(c, 0x{target:08x}u);"
            if likely:
                out.append(f"\t{{ int cond = ({extra}); c->r[31] = 0x{pc + 8:08x}u; if (cond) {{ {delay} {call} }} }}")
            else:
                out.append(f"\t{{ int cond = ({extra}); c->r[31] = 0x{pc + 8:08x}u; {delay} if (cond) {call} }}")
        elif kind == "jalr":
            out.append(f"\t{{ uint32_t t = {R(target)}; {W(extra, f'0x{pc + 8:08x}u')} {delay} rt_call(c, t); }}")
        elif kind == "jr":
            if target == 31 and local_rets:
                sw = " ".join(f"case 0x{t:08x}u: goto L_{t:08x};" for t in sorted(local_rets))
                out.append(f"\t{{ uint32_t t = c->r[31]; {delay} switch (t) {{ {sw} default: return; }} }}")
            elif target == 31:
                out.append(f"\t{{ {delay} return; }}")
            else:
                cases = sorted({t for t in (jumptables.get(pc) or addrs) if t in inset})
                sw = " ".join(f"case 0x{t:08x}u: goto L_{t:08x};" for t in cases)
                out.append(f"\t{{ uint32_t t = {R(target)}; {delay} switch (t) {{ {sw} "
                           f"default: rt_tail_call(c, 0x{pc:08x}u, t); return; }} }}")

        if dpc in labels:
            # Something jumps straight into this delay slot: give it its own copy, then continue at pc+8.
            nxt = dpc + 4
            out.append(f"\tgoto L_{nxt:08x};" if nxt in inset else f"\t{fall(nxt)}")
            label(dpc)
            if delay:
                out.append("\t" + delay)
            if nxt in inset:
                labels.add(nxt)

    out.append(f"\t{fall(addrs[-1] + 4)}")
    out.append("}")
    for u in unknown:
        report.append(f"{entry:08x}: {u}")
    return out


def tail_cycles(ctx, funcs, funcs_by_entry, jumptables):
    """Functions that tail-call each other in a cycle (a loop whose body spans function entries):
    as C calls they grow the stack every time around (GCC turns them into jumps, WebAssembly
    doesn't: the browser's stack overflowed). Returns the cycles (lists of entries, not hooked)."""
    import re
    pat = re.compile(r"\{ ((?:f|o\d+)_([0-9a-f]{8}))\(c\); return; \}")
    edges = {}
    for f in funcs:
        lines = translate(ctx, f, funcs_by_entry, jumptables, [])
        edges[f["entry"]] = {int(m.group(2), 16) for line in lines for m in pat.finditer(line)} - {f["entry"]}
    index, low, stack, on, out, n = {}, {}, [], set(), [], 0
    for v in edges:
        if v in index:
            continue
        work = [(v, iter(sorted(edges[v])))]
        index[v] = low[v] = n; n += 1; stack.append(v); on.add(v)
        while work:
            node, it = work[-1]
            for w in it:
                if w not in edges:
                    continue
                if w not in index:
                    index[w] = low[w] = n; n += 1; stack.append(w); on.add(w)
                    work.append((w, iter(sorted(edges[w]))))
                    break
                if w in on:
                    low[node] = min(low[node], index[w])
            else:
                work.pop()
                if work:
                    low[work[-1][0]] = min(low[work[-1][0]], low[node])
                if low[node] == index[node]:
                    comp = []
                    while True:
                        w = stack.pop(); on.discard(w); comp.append(w)
                        if w == node:
                            break
                    if len(comp) > 1 and not any(e in HOOKS for e in comp):
                        out.append(sorted(comp))
    return out


def merged_functions(funcs, cycles, prefix):
    """One function per cycle (the union of its members' bodies, entered by address), and the
    members as wrappers of it. Returns (merged pseudo-functions, {member entry: C wrapper})."""
    by_entry = {f["entry"]: f for f in funcs}
    merged, wrappers = [], {}
    for comp in cycles:
        name = f"{prefix}cycle_{comp[0]:08x}"
        addrs = set()
        for e in comp:
            for lo, hi in by_entry[e]["ranges"]:
                addrs.update(range(lo, hi + 1, 4))
        merged.append({"entry": comp[0], "entries": comp, "name": name, "ranges": to_ranges(addrs)})
        for e in comp:
            wrappers[e] = f"void {fname(e)}(CPU *c) {{ {name}(c, 0x{e:08x}u); }}"
    return merged, wrappers


def main_overlay(port, number, program_json, ov_bin, ov_meta):
    """One code overlay (re/tools/overlays.py): its functions as o<N>_<addr> in generated/ov_<N>.c,
    calls out of it to the executable's functions (f_) and the system's; a table of its functions
    and the first bytes it puts at its base (how the runtime knows it is the one loaded)."""
    global PREFIX
    gen = port / "generated"
    for hooks_file in [port / "hooks.txt"] + sorted(port.glob("hooks.*.txt")):  # this overlay's: ovN:addr
        for line in hooks_file.read_text().splitlines():
            line = line.split("#", 1)[0].strip()
            if line.startswith(f"ov{number}:"):
                HOOKS.add(int(line.split(":", 1)[1], 16))
    meta = json.loads((gen / "meta.json").read_text())
    base = int(meta["base"], 16)
    ovm = json.loads(ov_meta.read_text())
    ob, seg = int(ovm["base"], 16), ov_bin.read_bytes()
    img = bytearray((gen / "image.bin").read_bytes())
    need = ob - base + max(len(seg), ovm["memsz"])
    if len(img) < need:
        img += bytes(need - len(img))
    img[ob - base:ob - base + len(seg)] = seg
    ctx = Ctx(bytes(img), base)
    lo, hi = ob, ob + len(seg)
    imports = {int(i["addr"], 16): i for i in meta["imports"]}
    main_entries = set()
    for line in (gen / "funcs.h").read_text().splitlines():
        if line.startswith("void f_") and "_orig" not in line:
            main_entries.add(int(line[7:15], 16))
    prog = json.loads(program_json.read_text())
    funcs = []
    for f in prog["functions"]:
        entry = int(f["entry"], 16)
        if lo <= entry < hi:
            ranges = [(int(a, 16), int(b, 16)) for a, b in f["ranges"]]
            funcs.append({"entry": entry, "ranges": [(a, b) for a, b in ranges if lo <= a and b < hi]})
    funcs = [f for f in funcs if f["ranges"]]
    jumptables = {}
    for j in prog["jumptables"]:
        if j["targets"] and lo <= int(j["at"], 16) < hi:
            jumptables[int(j["at"], 16)] = [int(x, 16) for x in j["targets"]]
    for pc, ts in find_switch_tables(ctx, lo, hi, hi).items():
        if len(ts) > len(jumptables.get(pc, [])):
            jumptables[pc] = ts
    outside = dict(imports)
    outside.update({a: None for a in main_entries})
    discover_code(ctx, funcs, outside, lo, hi)
    stored = set()
    for k in range(0, len(seg) - 3, 4):  # its data's words pointing into its code
        v = struct.unpack_from("<I", seg, k)[0]
        if lo <= v < hi and not v & 3:
            stored.add(v)
    # The code right after a return (jr ra and its delay slot, past any padding nops) starts a
    # function: some are reached only through addresses the game works out at run time.
    for a in range(lo, hi - 8, 4):
        if ctx.word(a) == 0x03E00008:
            nxt = a + 8
            while nxt < hi and ctx.word(nxt) == 0:
                nxt += 4
            if nxt < hi:
                stored.add(nxt)
    stored.add(lo + 0x10)  # the code after the overlay's 16-byte header: where the game calls it first
    stored = {a for a in stored | code_immediates(ctx, lo, hi) if ctx.word(a)}
    share_code(ctx, funcs, outside, lo, hi, sorted(stored), jumptables)
    funcs.sort(key=lambda f: f["entry"])
    OWN.update(f["entry"] for f in funcs)
    PREFIX = f"o{number}_"
    funcs_by_entry = {f["entry"]: f for f in funcs}
    for a in list(main_entries) + list(imports):
        funcs_by_entry.setdefault(a, None)
    report = []
    body = ['#include <math.h>', '#include "funcs.h"', '#include "overlay.h"',
            'static inline uint32_t rt_bitrev(uint32_t v) { uint32_t r = 0; for (int i = 0; i < 32; i++) { r = (r << 1) | (v & 1); v >>= 1; } return r; }',
            '#pragma GCC diagnostic ignored "-Wunused-label"', '']
    body += [f"void {fname(f['entry'])}(CPU *c);" for f in funcs] + [""]
    merged, wrappers = merged_functions(funcs, tail_cycles(ctx, funcs, funcs_by_entry, jumptables), PREFIX)
    body += [f"static void {m['name']}(CPU *c, uint32_t entry);" for m in merged] + [""]
    for f in funcs:
        if f["entry"] in wrappers:
            body.append(wrappers[f["entry"]])
            continue
        body.extend(translate(ctx, f, funcs_by_entry, jumptables, report))
        body.append("")
    for m in merged:
        body.extend(translate(ctx, m, funcs_by_entry, jumptables, report))
        body.append("")
    sig = seg[:64]
    body.append(f"static const uint8_t sig[{len(sig)}] = {{{', '.join(str(x) for x in sig)}}};")
    body.append(f"static const OverlayFunc table[] = {{")
    body += [f"	{{0x{f['entry']:08x}u, {fname(f['entry'])}}}," for f in funcs]
    body.append("};")
    body.append(f"const Overlay rt_overlay_{number} = {{{number}, 0x{lo:08x}u, {len(seg)}u, sig, sizeof sig, table, "
                f"sizeof table / sizeof table[0]}};")
    (gen / f"ov_{number}.c").write_text("\n".join(body) + "\n")
    (gen / f"ov_{number}_report.txt").write_text("\n".join(report) + "\n")
    print(f"overlay {number}: {len(funcs)} functions ({len(wrappers)} in {len(merged)} merged cycles of tail calls), {len(report)} report lines")


def main():
    if sys.argv[1] == "--overlay":  # --overlay PORT N PROGRAM_JSON OV.bin OV.json
        main_overlay(Path(sys.argv[2]), int(sys.argv[3]), Path(sys.argv[4]), Path(sys.argv[5]), Path(sys.argv[6]))
        return
    port, program_json = Path(sys.argv[1]), Path(sys.argv[2])
    gen = port / "generated"
    meta = json.loads((gen / "meta.json").read_text())
    base = int(meta["base"], 16)
    ctx = Ctx((gen / "image.bin").read_bytes(), base)
    prog = json.loads(program_json.read_text())
    for hooks_file in [port / "hooks.txt"] + sorted(port.glob("hooks.*.txt")):  # hooks.private.txt etc., when present
        for line in hooks_file.read_text().splitlines():
            line = line.split("#", 1)[0].strip()
            if line and ":" not in line:  # (ovN:addr lines are an overlay's, below)
                HOOKS.add(int(line, 16))

    imports = {int(i["addr"], 16): i for i in meta["imports"]}
    funcs = []
    for f in prog["functions"]:
        entry = int(f["entry"], 16)
        if entry in imports:
            continue
        ranges = [(int(a, 16), int(b, 16)) for a, b in f["ranges"]]
        # Skip pseudo-functions Ghidra found inside data (e.g. .data/.rodata).
        text_lo, text_hi = (int(x, 16) for x in meta["text"])
        if not text_lo <= entry < text_hi:
            continue
        funcs.append({"entry": entry, "ranges": ranges})
    funcs.sort(key=lambda f: f["entry"])

    # Ghidra sometimes leaves holes inside a function body (code it did not follow). Execution
    # falling into one would abort at run time (rt_fallthrough; e.g. the results screen after a
    # round, FUN_089385f4), so holes that no other function claims become part of the function.
    claimed = sorted((lo, hi) for f in funcs for lo, hi in f["ranges"])
    starts = [lo for lo, _ in claimed]

    def is_claimed(a):
        i = bisect.bisect_right(starts, a) - 1
        return i >= 0 and claimed[i][0] <= a <= claimed[i][1]

    filled = []
    for f in funcs:
        r = sorted(f["ranges"])
        holes = []
        for (_, hi), (lo, _) in zip(r, r[1:]):
            g0, g1 = hi + 1, lo - 1
            if g1 >= g0 and not any(is_claimed(a) for a in range(g0, g1 + 1, 4)):
                holes.append((g0, g1))
        if holes:
            f["ranges"] = sorted(r + holes)
            filled += [(f["entry"], g0, g1 - g0 + 1) for g0, g1 in holes]
    text_lo, text_hi = (int(x, 16) for x in meta["text"])
    jumptables = {}
    for j in prog["jumptables"]:
        if j["targets"]:
            jumptables[int(j["at"], 16)] = [int(t, 16) for t in j["targets"]]
    found = find_switch_tables(ctx, text_lo, text_hi, int(meta["image_end"], 16))
    more = 0
    for pc, ts in found.items():
        if len(ts) > len(jumptables.get(pc, [])):
            jumptables[pc] = ts
            more += 1
    added, extended = discover_code(ctx, funcs, imports, text_lo, text_hi)
    # Functions Ghidra did not make (or merged into a neighbour): the entry, and every code address
    # the program keeps in its data (vtables, callback tables, member function pointers) or builds
    # in a register. Switch cases among them only cost some code: even Ghidra's tables sometimes
    # run past their end into a table of function pointers.
    stored = {int(p, 16) for p in meta["code_pointers"]} | code_immediates(ctx, text_lo, text_hi)
    # Code overlays (more arguments: their .bin files) call into the executable: their calls'
    # targets and the code addresses they build or keep are entries here too.
    for ov in sys.argv[3:]:
        seg = Path(ov).read_bytes()
        lui = {}
        for k in range(0, len(seg) - 3, 4):
            op = struct.unpack_from("<I", seg, k)[0]
            o, rs, rt, v = op >> 26, (op >> 21) & 31, (op >> 16) & 31, op & 0xFFFF
            if o == 3:
                tg = ((op & 0x3FFFFFF) << 2) | 0x08000000
                if text_lo <= tg < text_hi:
                    stored.add(tg)
            elif o == 0x0F:
                lui[rt] = v << 16
            elif o in (0x09, 0x0D) and rs in lui:  # lui + addiu/ori: an address built in a register
                val = (lui[rs] + ((v - 0x10000) if (o == 0x09 and v & 0x8000) else v)) & 0xFFFFFFFF
                if text_lo <= val < text_hi and not val & 3:
                    stored.add(val)
            if text_lo <= op < text_hi and not op & 3:
                stored.add(op)  # a word of its data pointing into the executable's code
    extra = [int(meta["entry"], 16)] + sorted(stored)
    grown, copied, made = share_code(ctx, funcs, imports, text_lo, text_hi, extra, jumptables)
    funcs.sort(key=lambda f: f["entry"])
    funcs_by_entry = {f["entry"]: f for f in funcs}
    for a in imports:
        funcs_by_entry.setdefault(a, None)

    report = [f"switch tables found in the code: {len(found)} ({more} new or larger than Ghidra's)"]
    # Clean old outputs.
    for p in gen.glob("code_*.c"):
        p.unlink()
    header = ['#pragma once', '#include "cpu.h"']
    header += [f"void f_{f['entry']:08x}(CPU *c);" for f in funcs]
    header += [f"void f_{a:08x}(CPU *c);" for a in sorted(imports)]
    header += [f"void f_{a:08x}_orig(CPU *c);  /* original of a hooked function */" for a in sorted(HOOKS)]
    (gen / "funcs.h").write_text("\n".join(header) + "\n")

    prelude = ['#include <math.h>', '#include "funcs.h"',
               'static inline uint32_t rt_bitrev(uint32_t v) { uint32_t r = 0; for (int i = 0; i < 32; i++) { r = (r << 1) | (v & 1); v >>= 1; } return r; }',
               '#pragma GCC diagnostic ignored "-Wunused-label"', '']
    merged, wrappers = merged_functions(funcs, tail_cycles(ctx, funcs, funcs_by_entry, jumptables), "")
    n_files = 0
    plain = [f for f in funcs if f["entry"] not in wrappers]
    for i in range(0, len(plain), FUNCS_PER_FILE):
        body = list(prelude)
        for f in plain[i:i + FUNCS_PER_FILE]:
            body.extend(translate(ctx, f, funcs_by_entry, jumptables, report))
            body.append("")
        (gen / f"code_{n_files:03d}.c").write_text("\n".join(body))
        n_files += 1
    body = list(prelude) + [f"static void {m['name']}(CPU *c, uint32_t entry);" for m in merged] + [""]
    body += [wrappers[e] for e in sorted(wrappers)] + [""]
    for m in merged:
        body.extend(translate(ctx, m, funcs_by_entry, jumptables, report))
        body.append("")
    (gen / f"code_{n_files:03d}.c").write_text("\n".join(body))
    n_files += 1
    report.append(f"cycles of tail calls merged: {len(merged)} ({len(wrappers)} functions)")

    # HLE imports.
    hle_h = ['#pragma once', '#include "cpu.h"']
    stubs = ['#include <stdio.h>', '#include "hle.h"', '#include "funcs.h"', '']
    seen = set()
    for a, imp in sorted(imports.items()):
        name = imp["name"]
        if name not in seen:
            seen.add(name)
            hle_h.append(f"void hle_{name}(CPU *c);")
            stubs.append(f'__attribute__((weak)) void hle_{name}(CPU *c) {{ rt_hle_unimplemented(c, "{name}"); }}')
        stubs.append(f"void f_{a:08x}(CPU *c) {{ rt_hle_poll(); if (rt_trace > 1) rt_hle_enter(c, \"{name}\"); "
                     f"hle_{name}(c); if (rt_trace > 1) rt_hle_leave(c, \"{name}\"); }}")
    hle_h.append("void rt_hle_unimplemented(CPU *c, const char *name);")
    hle_h.append("void rt_hle_poll(void);")
    hle_h.append("extern int rt_trace;")
    hle_h.append("void rt_hle_enter(CPU *c, const char *name);")
    hle_h.append("void rt_hle_leave(CPU *c, const char *name);")
    (gen / "hle.h").write_text("\n".join(hle_h) + "\n")
    (gen / "hle_stubs.c").write_text("\n".join(stubs) + "\n")

    # Indirect call table: every function entry, sorted.
    entries = sorted([f["entry"] for f in funcs] + list(imports))
    table = ['#include "funcs.h"', '#include <stddef.h>',
             'typedef struct { uint32_t addr; RecompFunc fn; } FuncEntry;',
             f'const size_t rt_func_count = {len(entries)};',
             'const FuncEntry rt_func_table[] = {']
    table += [f"\t{{0x{a:08x}u, f_{a:08x}}}," for a in entries]
    table.append("};")
    (gen / "func_table.c").write_text("\n".join(table) + "\n")

    report.append(f"functions found by flow analysis: {', '.join(f'{a:08x}' for a in added) or 'none'}; "
                  f"bodies extended past Ghidra's ranges: {', '.join(f'{e:08x}+{n}' for e, n in extended) or 'none'}")
    report.append(f"holes filled inside function bodies: {len(filled)} ({sum(n for _, _, n in filled)} bytes); larger ones: " +
                  ", ".join(f"{e:08x}@{a:08x}+{n}" for e, a, n in filled if n > 8))
    report.append(f"code shared between functions: {grown} functions took {copied} bytes of another's body; "
                  f"functions made where a jal, the entry or a stored code address lands inside another: {len(made)}")
    missing_ptrs = [p for p in meta["code_pointers"] if int(p, 16) not in funcs_by_entry]
    report.append(f"code pointers that are not function entries (jump-table cases etc.): {len(missing_ptrs)}")
    (gen / "recomp_report.txt").write_text("\n".join(report) + "\n")
    print(f"{len(funcs)} functions -> {n_files} files, {len(imports)} HLE stubs, report lines: {len(report)}")


if __name__ == "__main__":
    main()
