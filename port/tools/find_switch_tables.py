#!/usr/bin/env python3
"""Recover every switch jump table from the code pattern and add missing case targets to
re/export/fixups.json (Ghidra sometimes under-sizes or misses tables).

Pattern before `jr $at`: sltiu $at, $rX, N ; ... ; lui $at, %hi(table) ; addu ; lw $at, %lo(table)($at)
"""
import json
import struct
import sys
from pathlib import Path

root = Path(sys.argv[1])
B = 0x08804000
img = (root / "port/generated/image.bin").read_bytes()
meta = json.loads((root / "port/generated/meta.json").read_text())
prog = json.loads((root / "re/export/program.json").read_text())
text_lo, text_hi = (int(x, 16) for x in meta["text"])


def w(a):  # a word of the image (0 past it: tables in .bss are not the code's)
    return struct.unpack_from("<I", img, a - B)[0] if 0 <= a - B <= len(img) - 4 else 0


def s16(v):
    return v - 0x10000 if v & 0x8000 else v


known = {int(j["at"], 16): {int(t, 16) for t in j["targets"]} for j in prog["jumptables"]}
fix_path = root / "re/export/fixups.json"
fix = json.loads(fix_path.read_text())
existing = {j["at"]: set(j["targets"]) for j in fix["jumptables"]}
added = 0
for at, tg in sorted(known.items()):
    if not text_lo <= at < text_hi:
        continue
    op = w(at)
    if op >> 26 != 0 or (op & 63) != 8 or (op >> 21) & 31 == 31:
        continue
    reg = (op >> 21) & 31
    hi = lo = n = None
    bias = 0
    for k in range(1, 24):
        o = w(at - 4 * k)
        opc = o >> 26
        if opc == 0x23 and lo is None and (o >> 16) & 31 == reg:
            lo = s16(o & 0xFFFF)
        elif opc == 0x0F and hi is None and (o >> 16) & 31 == reg:
            hi = o & 0xFFFF
        elif opc == 0x09 and lo is not None and not bias and s16(o & 0xFFFF) < 0 and (o >> 16) & 31 == (o >> 21) & 31:
            bias = -s16(o & 0xFFFF)  # addiu rX, rX, -K : index = value - K
        elif opc in (0x0A, 0x0B) and n is None and hi is not None:
            n = (o & 0xFFFF) - bias if bias else (o & 0xFFFF)
    if hi is None or lo is None:
        continue
    base = ((hi << 16) + lo) & 0xFFFFFFFF
    if not n or n <= 0 or n > 1024:
        # No usable bound: take consecutive entries that look like code near the dispatch.
        n = 0
        while n < 1024 and text_lo <= w(base + 4 * n) < text_hi and abs(w(base + 4 * n) - at) < 0x20000:
            n += 1
    targets = {w(base + 4 * i) for i in range(n)}
    if not targets or not all(text_lo <= t < text_hi for t in targets):
        continue
    missing = targets - tg
    if missing:
        key = f"{at:08x}"
        merged = existing.get(key, set()) | {f"{t:08x}" for t in targets}
        existing[key] = merged
        added += len(missing)
        print(f"{at:08x}: table {base:08x} n={n}, Ghidra had {len(tg)} targets, missing {len(missing)}")
fix["jumptables"] = [{"at": k, "targets": sorted(v)} for k, v in sorted(existing.items())]
fix_path.write_text(json.dumps(fix, indent=1))
print(f"added {added} missing case targets")
