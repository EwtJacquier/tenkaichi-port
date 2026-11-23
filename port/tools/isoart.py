"""Pictures for the web page, read from the player's own ISO: the disc's own pictures,
PSP_GAME/PIC1.PNG (the key art, with the logo: beside the sign-in and behind the page) and
PSP_GAME/ICON0.PNG (the logo). A small PNG reader of its own: Python's standard library only
(serve_web.py imports this).

    python3 -I tools/isoart.py ISO NAME OUT.png     # NAME: scene, backdrop, logo
"""
import struct
import sys
import zlib
from pathlib import Path

ART = {  # name -> the disc's file
    'scene': 'PSP_GAME/PIC1.PNG',
    'backdrop': 'PSP_GAME/PIC1.PNG',
    'logo': 'PSP_GAME/ICON0.PNG',
}
CROP = {}
SHOWN = ('scene', 'logo')  # what the page uses (made when the server starts)

# The title screen's layers, for the page to draw it again sharp: the game's own textures, taken
# from the texture pack by their keys (its upscaled pictures: only when the pack is installed; the
# page keeps the disc's pictures without them). Where the game draws each one in its 480x272
# frame is in web/shell.html (.scene).
PACKED = {
    'sky': '6ebec385642918fb',     # the orange sky, 480x272
    'clouds': 'd1be53c67f680e35',  # four strips of clouds, 64 texels tall, scrolling along the bottom
    'vegeta': '10f97b476cc8780a',  # 256x256 at (0, 0)
    'goku': '21571de4cc233831',    # 256x256 at (224, 0)
    'trunks': '24b2094a60a7fd70',  # 128x256 at (48, 16)
    'goten': '3f872a1a6618fec1',   # 128x256 at (320, 16)
    'balls1': '664006054d09d34f',  # the dragon balls around Vegeta and Trunks, 256x256 at (0, 18)
    'balls2': '22373ef3a6582d9b',  # around Goku and Goten, 256x256 at (224, 18)
    'title': 'f9c83ce22890161d',   # the logo: DRAGON BALL Z / TENKAICHI / TAG TEAM, 352x160 at (64, 0)
}


# ---- ISO 9660 ----------------------------------------------------------------------------------------
def iso_file(f, path):
    """(byte offset, size) of a file on the ISO."""
    f.seek(16 * 2048)
    pvd = f.read(2048)
    lba, size = struct.unpack_from('<I', pvd, 158)[0], struct.unpack_from('<I', pvd, 166)[0]
    for part in path.split('/'):
        f.seek(lba * 2048)
        data = f.read(size)
        p, found = 0, None
        while p < len(data):
            n = data[p]
            if n == 0:
                p = (p // 2048 + 1) * 2048
                continue
            name = data[p + 33:p + 33 + data[p + 32]].decode('latin1').split(';')[0]
            if name.upper() == part.upper():
                found = struct.unpack_from('<I', data, p + 2)[0], struct.unpack_from('<I', data, p + 10)[0]
                break
            p += n
        if not found:
            raise FileNotFoundError(f'{path}: not on the ISO')
        lba, size = found
    return lba * 2048, size


# ---- PNG ---------------------------------------------------------------------------------------------
def unpng(d):
    """(width, height, RGBA bytes) of an 8-bit PNG (greyscale, RGB, palette, with or without alpha;
    not interlaced: the disc's are not)."""
    if d[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('not a PNG')
    p, idat, pal, trns = 8, [], b'', b''
    while p < len(d):
        n, t = struct.unpack_from('>I4s', d, p)
        body = d[p + 8:p + 8 + n]
        if t == b'IHDR':
            w, h, depth, ctype, _, _, interlace = struct.unpack('>IIBBBBB', body)
        elif t == b'PLTE':
            pal = body
        elif t == b'tRNS':
            trns = body
        elif t == b'IDAT':
            idat.append(body)
        p += 12 + n
    if depth != 8 or interlace:
        raise ValueError('only 8-bit, non-interlaced PNGs')
    bpp = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    raw = zlib.decompress(b''.join(idat))
    stride = w * bpp
    rows, prev, i = [], bytearray(stride), 0
    for _ in range(h):
        f, line = raw[i], bytearray(raw[i + 1:i + 1 + stride])
        i += 1 + stride
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if f == 1:
                line[x] = (line[x] + a) & 255
            elif f == 2:
                line[x] = (line[x] + b) & 255
            elif f == 3:
                line[x] = (line[x] + ((a + b) >> 1)) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(bytes(line))
        prev = line
    px = b''.join(rows)
    out = bytearray(w * h * 4)
    for k in range(w * h):
        if ctype == 2:
            out[k * 4:k * 4 + 4] = px[k * 3:k * 3 + 3] + b'\xff'
        elif ctype == 6:
            out[k * 4:k * 4 + 4] = px[k * 4:k * 4 + 4]
        elif ctype == 3:
            j = px[k]
            out[k * 4:k * 4 + 4] = pal[j * 3:j * 3 + 3] + bytes((trns[j] if j < len(trns) else 255,))
        elif ctype == 0:
            out[k * 4:k * 4 + 4] = bytes((px[k],) * 3 + (255,))
        else:
            out[k * 4:k * 4 + 4] = bytes((px[k * 2],) * 3 + (px[k * 2 + 1],))
    return w, h, bytes(out)


def png(w, h, rgba):
    """PNG bytes: RGB when every texel is opaque, each row filtered against the one above."""
    opaque = rgba[3::4] == b'\xff' * (w * h)
    px, bpp = (bytes(b for i, b in enumerate(rgba) if i & 3 != 3), 3) if opaque else (rgba, 4)
    stride = w * bpp
    out, prev = [], bytes(stride)
    for y in range(h):
        row = px[y * stride:(y + 1) * stride]
        out.append(b'\2' + bytes((a - b) & 255 for a, b in zip(row, prev)) if y else b'\0' + row)
        prev = row
    chunk = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2 if opaque else 6, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(b''.join(out), 6)) + chunk(b'IEND', b''))


# ---- what the page asks for --------------------------------------------------------------------------
def original(iso, name):
    """(width, height, RGBA bytes, sprites) of one of ART, as the ISO has it."""
    with open(iso, 'rb') as f:
        off, size = iso_file(f, ART[name])
        f.seek(off)
        w, h, rgba = unpng(f.read(size))
    return w, h, rgba, {}


def pack_file(pack, w, h, rgba):
    """These pictures are not textures of the game: never in the texture pack."""
    return None


def pack_key_file(pack, k):
    """The texture pack's file for the texture with this key (PACKED), or None."""
    try:
        for line in (Path(pack) / 'index.txt').read_text().splitlines():
            parts = line.split()
            if len(parts) == 4 and parts[0] == k:
                return Path(pack) / parts[3]
    except OSError:
        pass
    return None


def qoi(d):
    """(width, height, RGBA bytes) of a QOI picture (the texture pack's)."""
    if d[:4] != b'qoif':
        raise ValueError('not a QOI picture')
    w, h = struct.unpack_from('>II', d, 4)
    n = w * h
    out = bytearray(n * 4)
    index = [b'\0\0\0\0'] * 64
    r, g, b, a = 0, 0, 0, 255
    p, i = 14, 0
    while i < n:
        c = d[p]
        p += 1
        if c == 0xfe:
            r, g, b = d[p], d[p + 1], d[p + 2]
            p += 3
        elif c == 0xff:
            r, g, b, a = d[p], d[p + 1], d[p + 2], d[p + 3]
            p += 4
        elif c >> 6 == 0:
            r, g, b, a = index[c]
        elif c >> 6 == 1:
            r, g, b = (r + (c >> 4 & 3) - 2) & 255, (g + (c >> 2 & 3) - 2) & 255, (b + (c & 3) - 2) & 255
        elif c >> 6 == 2:
            dg = (c & 63) - 32
            c2 = d[p]
            p += 1
            r, g, b = (r + dg - 8 + (c2 >> 4)) & 255, (g + dg) & 255, (b + dg - 8 + (c2 & 15)) & 255
        else:
            run = (c & 63) + 1
            out[i * 4:(i + run) * 4] = bytes((r, g, b, a)) * run
            i += run
            continue
        px = bytes((r, g, b, a))
        index[(r * 3 + g * 5 + b * 7 + a * 11) & 63] = px
        out[i * 4:i * 4 + 4] = px
        i += 1
    return w, h, bytes(out)


def picture(w, h, rgba, upscaled=None, crop=None):
    """PNG bytes of the picture and the scale used (always 1)."""
    return png(w, h, rgba), 1


if __name__ == '__main__':
    w, h, rgba, _ = original(sys.argv[1], sys.argv[2])
    Path(sys.argv[3]).write_bytes(picture(w, h, rgba)[0])
    print(dict(w=w, h=h))
