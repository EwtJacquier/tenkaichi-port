#!/usr/bin/env python3
"""PACKFILE.BIN (USRDIR): the game's archive. A 16-byte header ("PacK", entry count 0x3711) and one
16-byte entry per file, each word scrambled (FUN_0882fb3c): sector = (w0 ^ header word 0) +
the archive's first sector + 0x70 (the table's 0x70 sectors), size = w1 ^ index, w2 ^ 0xaaaaaaaa,
w3 ^ index. Files are addressed by number.

    python3 -I packfile.py ISO list            # number, sector, size, w2, w3, first bytes
    python3 -I packfile.py ISO extract N OUT   # file N
"""
import struct
import sys

SECTOR = 2048


def find(iso, path):
    def rd(lba, n):
        iso.seek(lba * SECTOR)
        return iso.read(n)
    pvd = rd(16, SECTOR)
    lba, size = struct.unpack_from('<I', pvd, 156 + 2)[0], struct.unpack_from('<I', pvd, 156 + 10)[0]
    for part in path.split('/'):
        d = rd(lba, size)
        i = 0
        found = None
        while i < len(d):
            n = d[i]
            if n == 0:
                i = (i // SECTOR + 1) * SECTOR
                continue
            name = d[i + 33:i + 33 + d[i + 32]].split(b';')[0].decode(errors='replace')
            if name == part:
                found = struct.unpack_from('<I', d, i + 2)[0], struct.unpack_from('<I', d, i + 10)[0]
                break
            i += n
        lba, size = found
    return lba, size


def table(iso):
    lba, size = find(iso, 'PSP_GAME/USRDIR/PACKFILE.BIN')
    iso.seek(lba * SECTOR)
    t = iso.read(0x70 * SECTOR)
    h0, count = struct.unpack_from('<II', t, 0)
    out = []
    for i in range(count):
        w0, w1, w2, w3 = struct.unpack_from('<IIII', t, 16 + 16 * i)
        out.append((i, ((w0 ^ h0) + lba + 0x70) & 0xFFFFFFFF, w1 ^ i, w2 ^ 0xAAAAAAAA, w3 ^ i))
    return out


def main():
    iso = open(sys.argv[1], 'rb')
    t = table(iso)
    if sys.argv[2] == 'list':
        for i, sec, size, w2, w3 in t:
            iso.seek(sec * SECTOR)
            head = iso.read(16) if size else b''
            print(f'{i:5d} {sec:#08x} {size:9d} {w2:08x} {w3:08x} {head.hex()}')
    elif sys.argv[2] == 'extract':
        i, sec, size, _, _ = t[int(sys.argv[3])]
        iso.seek(sec * SECTOR)
        open(sys.argv[4], 'wb').write(iso.read(size))


if __name__ == '__main__':
    main()
