#!/usr/bin/env python3
"""Functions Ghidra did not make: code addresses the program stores (vtables, callbacks, thread
entries: prx_load.py's code_pointers) that are neither a function nor a switch case, kept when
they look like a function's start (a stack frame, addiu sp, sp, -N, within the first instructions)
or sit where no function is (a gap between Ghidra's functions). Added to re/export/fixups.json
("functions"); FixupCode.java makes them (tools/rebuild.sh).

    python3 -I tools/find_functions.py ROOT     (ROOT: the project folder, with port/ and re/)
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
fix_path = root / "re/export/fixups.json"
fix = json.loads(fix_path.read_text())
text_lo, text_hi = (int(x, 16) for x in meta["text"])


def w(a):
    return struct.unpack_from("<I", img, a - B)[0] if 0 <= a - B <= len(img) - 4 else 0


entries = {int(f["entry"], 16) for f in prog["functions"]}
inside = []  # (lo, hi) of every function's ranges
for f in prog["functions"]:
    for lo, hi in f["ranges"]:
        inside.append((int(lo, 16), int(hi, 16)))
inside.sort()
cases = {int(t, 16) for j in prog["jumptables"] for t in j["targets"]}
cases |= {int(t, 16) for j in fix["jumptables"] for t in j["targets"]}


def in_function(a):
    import bisect
    i = bisect.bisect_right(inside, (a, 0xFFFFFFFF)) - 1
    return i >= 0 and inside[i][0] <= a <= inside[i][1]


def frame_start(a):  # addiu sp, sp, -N in the first 6 instructions
    for k in range(6):
        o = w(a + 4 * k)
        if o >> 16 == 0x27BD and o & 0x8000:
            return True
    return False


found, kinds = set(), {"frame": 0, "gap": 0}
for s in meta["code_pointers"]:
    a = int(s, 16)
    if a in entries or a in cases or a & 3 or not text_lo <= a < text_hi:
        continue
    if not in_function(a):
        found.add(a), kinds.__setitem__("gap", kinds["gap"] + 1)
    elif frame_start(a):
        found.add(a), kinds.__setitem__("frame", kinds["frame"] + 1)
have = set(fix["functions"])
new = sorted(f"{a:08x}" for a in found if f"{a:08x}" not in have)
fix["functions"] = sorted(have | set(new))
fix_path.write_text(json.dumps(fix, indent=1))
print(f"{len(new)} functions added ({kinds['frame']} with a stack frame inside other functions' ranges, {kinds['gap']} in gaps)")
