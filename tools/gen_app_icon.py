#!/usr/bin/env python3
"""Draws the program's built-in icon: our own art (no game assets), a faceted crystal on a night-blue tile with a gold
rim, in the colours of the interface. Writes game/res/ffcc.ico (16, 32, 48, 64, 128 and 256 pixels, a PNG each), which
game/ffcc.rc puts into ffcc.exe. Once the player's game is set up, the program swaps in the game's own icon
(game/src/app_icon.hpp). Pure Python: python tools/gen_app_icon.py"""
import math
import os
import struct
import zlib

SIZES = [16, 32, 48, 64, 128, 256]
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'game', 'res', 'ffcc.ico')
BASE = 256          # drawn at this size (with 3 x 3 samples a pixel), the others are averaged down from it
SS = 3

NIGHT = (11, 20, 48)
GOLD = (230, 199, 127)
# The crystal: a ring of six points around a centre, one shade for each of the six facets (bright at the top left).
POINTS = [(0.0, -0.80), (0.46, -0.40), (0.46, 0.38), (0.0, 0.80), (-0.46, 0.38), (-0.46, -0.40)]
CENTRE = (0.04, -0.06)
FACETS = [(235, 250, 255), (160, 215, 255), (70, 135, 215), (36, 78, 160), (95, 165, 235), (200, 235, 255)]


def lerp(a, b, t):
    return a + (b - a) * t


def inside(p, a, b, c):
    def side(p1, p2, p3):
        return (p1[0] - p3[0]) * (p2[1] - p3[1]) - (p2[0] - p3[0]) * (p1[1] - p3[1])
    d1, d2, d3 = side(p, a, b), side(p, b, c), side(p, c, a)
    neg = d1 < 0 or d2 < 0 or d3 < 0
    pos = d1 > 0 or d2 > 0 or d3 > 0
    return not (neg and pos)


def sample(x, y):
    """The colour (r, g, b, a) at a point of the unit square (-1..1 both ways)."""
    # a rounded tile
    r = 0.30
    qx, qy = abs(x) - (1 - r), abs(y) - (1 - r)
    dist = math.hypot(max(qx, 0), max(qy, 0)) + min(max(qx, qy), 0) - r
    if dist > 0:
        return (0, 0, 0, 0)
    if dist > -0.075:
        return GOLD + (255,)
    # the night with a glow behind the crystal
    glow = max(0.0, 1 - math.hypot(x - 0.04, y + 0.05) / 1.0) ** 2
    base = tuple(lerp(NIGHT[i], (40, 90, 170)[i], glow * 0.8) for i in range(3))
    p = (x, y)
    for i in range(6):
        a, b = POINTS[i], POINTS[(i + 1) % 6]
        if inside(p, a, b, CENTRE):
            shade = FACETS[i]
            return tuple(int(c) for c in shade) + (255,)
    return tuple(int(c) for c in base) + (255,)


def render():
    n = BASE * SS
    rows = []
    for j in range(BASE):
        row = []
        for i in range(BASE):
            acc = [0.0, 0.0, 0.0, 0.0]
            for sj in range(SS):
                for si in range(SS):
                    x = ((i * SS + si + 0.5) / n) * 2 - 1
                    y = ((j * SS + sj + 0.5) / n) * 2 - 1
                    r, g, b, a = sample(x, y)
                    acc[0] += r * a
                    acc[1] += g * a
                    acc[2] += b * a
                    acc[3] += a
            if acc[3] > 0:
                row.append((acc[0] / acc[3], acc[1] / acc[3], acc[2] / acc[3], acc[3] / (SS * SS)))
            else:
                row.append((0, 0, 0, 0))
        rows.append(row)
    return rows


def shrink(rows, size):
    out = []
    for j in range(size):
        row = []
        for i in range(size):
            acc = [0.0, 0.0, 0.0, 0.0]
            count = 0
            y0, y1 = j * BASE // size, max((j + 1) * BASE // size, j * BASE // size + 1)
            x0, x1 = i * BASE // size, max((i + 1) * BASE // size, i * BASE // size + 1)
            for y in range(y0, y1):
                for x in range(x0, x1):
                    r, g, b, a = rows[y][x]
                    acc[0] += r * a
                    acc[1] += g * a
                    acc[2] += b * a
                    acc[3] += a
                    count += 1
            if acc[3] > 0:
                row.append((int(acc[0] / acc[3] + 0.5), int(acc[1] / acc[3] + 0.5), int(acc[2] / acc[3] + 0.5),
                            int(acc[3] / count + 0.5)))
            else:
                row.append((0, 0, 0, 0))
        out.append(row)
    return out


def png(rows):
    h, w = len(rows), len(rows[0])
    raw = b''.join(b'\x00' + bytes(v for px in row for v in px) for row in rows)

    def chunk(kind, data):
        body = kind + data
        return struct.pack('>I', len(data)) + body + struct.pack('>I', zlib.crc32(body) & 0xFFFFFFFF)

    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def main():
    big = render()
    images = [png(shrink(big, s)) for s in SIZES]
    out = struct.pack('<HHH', 0, 1, len(images))
    offset = 6 + 16 * len(images)
    for s, img in zip(SIZES, images):
        out += struct.pack('<BBBBHHII', 0 if s >= 256 else s, 0 if s >= 256 else s, 0, 0, 1, 32, len(img), offset)
        offset += len(img)
    out += b''.join(images)
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, 'wb') as f:
        f.write(out)
    print('wrote', os.path.normpath(OUT), len(out), 'bytes')


if __name__ == '__main__':
    main()
