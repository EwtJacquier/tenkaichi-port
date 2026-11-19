#!/usr/bin/env python3
"""HD textures: install a texture pack into the port, take it out again, or pack one up.

    python3 tools/textures.py install dbztag-hd-textures-*.zip   # from port/
    python3 tools/textures.py uninstall                          # back to the game's own textures
    python3 tools/textures.py status
    python3 tools/textures.py pack OUT_PREFIX [--max-size 1900]  # zip build/textures up (MB per part)

A pack never changes the game's files: the game (native and web) reads it from build/textures
while it runs and draws each replacement in place of the texture it matches, and every texture
the pack doesn't have stays the game's own. Uninstalling, or the "HD textures" switch of the web
page's menu, brings the originals back. Each replacement is a QOI picture at an integer scale of
the texture it replaces, listed in index.txt as "<key> <width> <height> <file>" (key: CRC-32 and
Adler-32 of the original picture's RGBA texels, see runtime/texpack.h).

A pack may come in several zip parts (each with its own index.txt); install takes them all at
once, or more parts later (they add up). The HD font (font-<key>.qoi + font-<key>.txt: the game's
glyphs upscaled, for the lines its font engine writes, see runtime/fonthd.h) and the HD movies
(movies/<name>.h264, see runtime/psmf.c) come in the first part. Standard library only.
"""
import argparse
import json
import os
import re
import shutil
import struct
import sys
import time
import zipfile
from pathlib import Path

PORT = Path(__file__).resolve().parents[1]
PACK = PORT / 'build' / 'textures'
# key, picture w h, file; then the whole texture's w h when the picture is its top left corner
LINE = re.compile(r'([0-9a-f]{16}) (\d{1,4}) (\d{1,4}) ([0-9a-f]{16}-[0-9a-z]{1,24}\.qoi)(?: (\d{1,4}) (\d{1,4}))?')
FONT = re.compile(r'font-[0-9a-f]{16}\.(qoi|txt)')
MOVIE = re.compile(r'movies/[A-Za-z0-9_]{1,32}\.h264')  # HD movies (runtime/psmf.c)
HEADER = '# texture pack (key: CRC-32 + Adler-32 of the picture\'s RGBA texels; picture w h; file; [texture w h when the picture is its corner])'


def read_index(text):
    """{key: (w, h, file, rest)} of an index.txt (comments and blank lines skipped; rest: " tw th" or "")."""
    out = {}
    for n, line in enumerate(text.splitlines(), 1):
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        m = LINE.fullmatch(line)
        if not m:
            raise ValueError(f'index.txt line {n}: {line[:60]!r}')
        out[m[1]] = (int(m[2]), int(m[3]), m[4], f' {m[5]} {m[6]}' if m[5] else '')
    return out


def write_index(entries, folder):
    lines = [HEADER] + [f'{k} {w} {h} {f}{x}' for k, (w, h, f, x) in sorted(entries.items())]
    tmp = folder / 'index.txt.tmp'
    tmp.write_text('\n'.join(lines) + '\n')
    os.replace(tmp, folder / 'index.txt')  # the running game reads it again within a second


def qoi_size(head):
    if head[:4] != b'qoif':
        raise ValueError('not a QOI picture')
    return struct.unpack_from('>II', head, 4)


def install(zips):
    PACK.mkdir(parents=True, exist_ok=True)
    try:
        have = read_index((PACK / 'index.txt').read_text())
    except FileNotFoundError:
        have = {}
    added = 0
    for z in zips:
        with zipfile.ZipFile(z) as zf:
            names = {Path(n).name: n for n in zf.namelist() if not n.endswith('/')}
            if 'index.txt' not in names:
                raise SystemExit(f'{z}: no index.txt in it: not a texture pack')
            entries = read_index(zf.read(names['index.txt']).decode())
            for name in names:
                if FONT.fullmatch(name):  # the HD font
                    tmp = PACK / (name + '.tmp')
                    with zf.open(names[name]) as src, open(tmp, 'wb') as dst:
                        shutil.copyfileobj(src, dst)
                    os.replace(tmp, PACK / name)
                    print(f'{z}: HD font {name}')
            for name in zf.namelist():
                if MOVIE.fullmatch(name):  # an HD movie
                    (PACK / 'movies').mkdir(exist_ok=True)
                    tmp = PACK / (name + '.tmp')
                    with zf.open(name) as src, open(tmp, 'wb') as dst:
                        shutil.copyfileobj(src, dst, 1 << 20)
                    os.replace(tmp, PACK / name)
                    print(f'{z}: HD movie {name}')
            for i, (k, (w, h, f, x)) in enumerate(entries.items()):
                if f not in names:
                    raise SystemExit(f'{z}: {f} is listed but missing')
                with zf.open(names[f]) as src:
                    head = src.read(14)
                    pw, ph = qoi_size(head)
                    if pw % w or ph % h or pw // w != ph // h:
                        raise SystemExit(f'{z}: {f} is {pw}x{ph}, not a whole multiple of {w}x{h}')
                    tmp = PACK / (f + '.tmp')
                    with open(tmp, 'wb') as dst:
                        dst.write(head)
                        shutil.copyfileobj(src, dst, 1 << 20)
                    os.replace(tmp, PACK / f)
                old = have.get(k)
                if old and old[2] != f and (PACK / old[2]).exists():
                    (PACK / old[2]).unlink()
                have[k] = (w, h, f, x)
                added += 1
                if i % 200 == 0:
                    print(f'\r{z}: {i}/{len(entries)}', end='', flush=True)
            print(f'\r{z}: {len(entries)} textures' + ' ' * 10)
    write_index(have, PACK)
    print(f'{added} textures installed, {len(have)} in the pack ({PACK}).')
    print('The game picks them up while it runs; "HD textures" in the web menu turns them off and on.')


def uninstall():
    if not PACK.exists():
        print('No texture pack installed: the game shows its own textures.')
        return
    n = sum(1 for _ in PACK.glob('*.qoi'))
    write_index({}, PACK)  # a running game goes back to the originals at once
    time.sleep(1.5)
    shutil.rmtree(PACK)
    print(f'Texture pack removed ({n} pictures): the game shows its own textures again.')


def status():
    try:
        entries = read_index((PACK / 'index.txt').read_text())
    except FileNotFoundError:
        print('No texture pack installed: the game shows its own textures.')
        return
    size = sum((PACK / f).stat().st_size for _, _, f, _ in entries.values() if (PACK / f).exists())
    missing = sum(1 for _, _, f, _ in entries.values() if not (PACK / f).exists())
    fonts = sum(1 for f in PACK.glob('font-*.qoi'))
    movies = sorted(f.stem for f in PACK.glob('movies/*.h264') if f.stat().st_size)
    print(f'{len(entries)} textures in {PACK} ({size / 1e9:.2f} GB)' + (f', {missing} files missing' if missing else '')
          + (f', HD font: {fonts}' if fonts else '') + (f', HD movies: {", ".join(movies)}' if movies else ''))


def pack(prefix, max_mb):
    entries = read_index((PACK / 'index.txt').read_text())
    parts, cur, size = [], {}, 0
    for k, e in sorted(entries.items()):
        s = (PACK / e[2]).stat().st_size
        if cur and size + s > max_mb * 1e6:
            parts.append(cur)
            cur, size = {}, 0
        cur[k] = e
        size += s
    if cur:
        parts.append(cur)
    readme = (__doc__.split('\n\n')[2] + '\n\nInstall (from the port\'s folder):\n'
              '    python3 tools/textures.py install dbztag-hd-textures-*.zip\n'
              'Take it out again:\n    python3 tools/textures.py uninstall\n')
    for i, part in enumerate(parts, 1):
        out = Path(f'{prefix}-{i}-of-{len(parts)}.zip' if len(parts) > 1 else f'{prefix}.zip')
        with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as zf:
            zf.writestr('README.txt', readme)
            zf.writestr('pack.json', json.dumps(dict(part=i, parts=len(parts), textures=len(part))))
            zf.writestr('index.txt', '\n'.join([HEADER] + [f'{k} {w} {h} {f}{x}' for k, (w, h, f, x) in sorted(part.items())]) + '\n')
            if i == 1:
                for f in sorted(PACK.iterdir()):
                    if FONT.fullmatch(f.name):
                        zf.write(f, f.name)
                for f in sorted((PACK / 'movies').glob('*.h264')):
                    if MOVIE.fullmatch(f'movies/{f.name}'):
                        zf.write(f, f'movies/{f.name}')
            for j, (_, _, f, _) in enumerate(part.values()):
                zf.write(PACK / f, f)
                if j % 200 == 0:
                    print(f'\r{out}: {j}/{len(part)}', end='', flush=True)
        print(f'\r{out}: {len(part)} textures, {out.stat().st_size / 1e6:.0f} MB')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    sub.add_parser('install').add_argument('zips', nargs='+')
    sub.add_parser('uninstall')
    sub.add_parser('status')
    p = sub.add_parser('pack')
    p.add_argument('prefix')
    p.add_argument('--max-size', type=int, default=1900, help='MB per zip part (GitHub release files: under 2 GB)')
    a = ap.parse_args()
    if a.cmd == 'install':
        install(a.zips)
    elif a.cmd == 'uninstall':
        uninstall()
    elif a.cmd == 'status':
        status()
    else:
        pack(a.prefix, a.max_size)


if __name__ == '__main__':
    sys.exit(main())
