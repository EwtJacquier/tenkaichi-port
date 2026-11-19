#!/usr/bin/env python3
"""The game's code overlays: ELF files in PACKFILE.BIN (type 0xffa8, one PT_LOAD at 0x08a814a0,
the space right after the executable's code and data, already linked there). The game reads one
in at a time and calls into it; the port recompiles each one apart (port/tools/recomp.py --overlay).

    python3 -I overlays.py ISO OUTDIR
writes, per overlay N (its number in PACKFILE.BIN): OUTDIR/ovN.bin (the bytes loaded at the
base, file size), OUTDIR/ovN.json (base, file size, memory size), and OUTDIR/ovN.elf: the
executable with the overlay as one more PT_LOAD segment, for Ghidra (re/analyze_overlays.sh).
"""
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import packfile  # noqa: E402

BASE = 0x08A814A0


def overlays(iso):
    for i, sec, size, _, _ in packfile.table(iso):
        if size < 0x54:
            continue
        iso.seek(sec * packfile.SECTOR)
        d = iso.read(size)
        if d[:4] != b'\x7fELF':
            continue
        phoff, = struct.unpack_from('<I', d, 28)
        _, off, vaddr, _, filesz, memsz, _, _ = struct.unpack_from('<8I', d, phoff)
        if vaddr != BASE:
            raise SystemExit(f'overlay {i}: loads at {vaddr:08x}, not {BASE:08x}')
        yield i, d[off:off + filesz], memsz


def with_overlay(exe, seg, memsz):
    """The executable with one more PT_LOAD (the overlay), its program headers moved to the end."""
    phoff, = struct.unpack_from('<I', exe, 28)
    phentsize, phnum = struct.unpack_from('<HH', exe, 42)
    phs = exe[phoff:phoff + phentsize * phnum]
    out = bytearray(exe)
    while len(out) % 16:
        out.append(0)
    seg_off = len(out)
    out += seg
    while len(out) % 16:
        out.append(0)
    new_phoff = len(out)
    out += phs + struct.pack('<8I', 1, seg_off, BASE, BASE, len(seg), memsz, 7, 16)
    struct.pack_into('<I', out, 28, new_phoff)
    struct.pack_into('<H', out, 44, phnum + 1)
    struct.pack_into('<I', out, 32, 0)   # no section headers
    struct.pack_into('<H', out, 48, 0)
    return bytes(out)


def main():
    iso = open(sys.argv[1], 'rb')
    out = Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    exe = (Path(__file__).resolve().parents[2] / 'EBOOT_decrypted.elf').read_bytes()
    n = 0
    for i, seg, memsz in overlays(iso):
        (out / f'ov{i}.bin').write_bytes(seg)
        (out / f'ov{i}.json').write_text(json.dumps(dict(number=i, base=f'{BASE:08x}', size=len(seg), memsz=memsz)))
        (out / f'ov{i}.elf').write_bytes(with_overlay(exe, seg, memsz))
        n += 1
    print(f'{n} overlays in {out}')


if __name__ == '__main__':
    main()
