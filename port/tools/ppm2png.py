#!/usr/bin/env python3
"""Convert P6 PPM frame dumps to PNG (stdlib only).  ppm2png.py a.ppm [b.ppm ...]"""
import struct
import sys
import zlib
from pathlib import Path


def convert(path):
    data = Path(path).read_bytes()
    parts = data.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    px = parts[3]
    raw = b"".join(b"\x00" + px[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")
    out = Path(path).with_suffix(".png")
    out.write_bytes(png)
    return out


for p in sys.argv[1:]:
    print(convert(p))
