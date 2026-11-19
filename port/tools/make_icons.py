"""The page's icons (web/favicon-32.png, web/apple-touch-icon.png): an orange crystal ball with four
red stars in it, drawn here (no game art), 8x larger than needed and scaled down.

    python3 -I tools/make_icons.py
"""
import math
from pathlib import Path

from PIL import Image, ImageDraw

OUT = Path(__file__).resolve().parents[1] / 'web'


def star(cx, cy, r):
    pts = []
    for k in range(10):
        a = -math.pi / 2 + k * math.pi / 5
        rr = r if k % 2 == 0 else r * 0.42
        pts.append((cx + rr * math.cos(a), cy + rr * math.sin(a)))
    return pts


def icon(size, pad):
    S = size * 8
    img = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    c = S / 2
    R = S / 2 - pad * 8
    # the ball: a radial gradient from a pale yellow highlight (upper left) to deep orange, a dark rim
    for i in range(int(R), 0, -1):
        t = i / R
        col = (int(255 - 40 * t), int(220 - 120 * t), int(120 - 110 * t), 255)
        off = (1 - t) * R * 0.35
        d.ellipse((c - i - off, c - i - off, c + i - off, c + i - off), fill=col)
    d.ellipse((c - R, c - R, c + R, c + R), outline=(110, 40, 0, 255), width=max(2, int(R * 0.06)))
    # four red stars, as a diamond
    sr = R * 0.2
    for dx, dy in ((0, -0.36), (-0.36, 0.04), (0.36, 0.04), (0, 0.42)):
        d.polygon(star(c + dx * R, c + dy * R, sr), fill=(220, 30, 20, 255))
    # the shine, blended over the ball
    shine = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(shine).ellipse((c - R * 0.62, c - R * 0.7, c - R * 0.22, c - R * 0.38), fill=(255, 255, 240, 150))
    img.alpha_composite(shine)
    return img.resize((size, size), Image.LANCZOS)


def main():
    icon(32, 1).save(OUT / 'favicon-32.png')
    bg = Image.new('RGBA', (180, 180), (6, 26, 82, 255))  # the apple touch icon: on the windows' deep blue
    bg.alpha_composite(icon(180, 14))
    bg.convert('RGB').save(OUT / 'apple-touch-icon.png')
    print('icons written to', OUT)


if __name__ == '__main__':
    main()
