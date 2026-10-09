#!/usr/bin/env python3
"""Draws the backdrop of the start screen: our own art (no game assets), a deep blue night with a cluster of faceted
crystals and a glow on the right, leaving the left for the menu. Writes game/ui/res/prelaunch-bg.png (1920x1080).
Pure Python, a few tens of seconds. Run from anywhere: python tools/gen_prelaunch_art.py"""
import math
import os
import struct
import zlib

W, H = 1920, 1080
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'game', 'ui', 'res', 'prelaunch-bg.png')

GLOW_C = (1340.0, 500.0)
# (centre x, centre y, half height, half width, rotation in degrees, brightness)
CRYSTALS = [
    (1340, 520, 330, 118, -7, 1.0),
    (1105, 650, 200, 72, 13, 0.85),
    (1570, 625, 245, 88, -21, 0.9),
    (1745, 395, 125, 46, 19, 0.7),
    (975, 335, 95, 34, 31, 0.6),
    (1475, 285, 85, 31, -34, 0.65),
    (1215, 345, 70, 26, -15, 0.7),
    (1795, 690, 110, 40, 9, 0.55),
    (905, 760, 70, 25, -24, 0.45),
]
BAYER = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]


def lerp(a, b, t):
    return a + (b - a) * t


def smoothstep(a, b, x):
    t = max(0.0, min(1.0, (x - a) / (b - a)))
    return t * t * (3 - 2 * t)


def crystal_planes(h, w):
    pts = [(0, -h), (w, -0.5 * h), (w, 0.4 * h), (0, 0.62 * h), (-w, 0.4 * h), (-w, -0.5 * h)]
    planes = []
    for i in range(6):
        (ax, ay), (bx, by) = pts[i], pts[(i + 1) % 6]
        dx, dy = bx - ax, by - ay
        n = math.hypot(dx, dy)
        nx, ny = dy / n, -dx / n
        if nx * (0 - ax) + ny * (0.03 * h - ay) < 0:
            nx, ny = -nx, -ny
        planes.append((nx, ny, nx * ax + ny * ay))
    return planes


class Crystal:
    def __init__(self, cx, cy, h, w, rot, bright):
        self.cx, self.cy, self.h, self.w, self.bright = cx, cy, h, w, bright
        a = math.radians(rot)
        self.cos, self.sin = math.cos(a), math.sin(a)
        self.planes = crystal_planes(h, w)
        r = math.hypot(h, w) + 4
        self.box = (int(cx - r), int(cy - r), int(cx + r), int(cy + r))

    def shade(self, x, y, base):
        """Blends this crystal over `base` (r, g, b) at pixel (x, y); returns the new colour."""
        dx, dy = x - self.cx, y - self.cy
        u = dx * self.cos + dy * self.sin
        v = -dx * self.sin + dy * self.cos
        d = min(nx * u + ny * v - c for nx, ny, c in self.planes)
        cover = d + 0.5
        if cover <= 0:
            return base
        cover = min(1.0, cover)
        h, w = self.h, self.w
        up = (v + h) / (1.62 * h)  # 0 at the tip, 1 at the foot
        if v < -0.5 * h:  # the cap: its two faces catch the light differently
            col = (118, 200, 255) if u < 0 else (70, 150, 235)
        elif u < 0:
            col = (62, 142, 232)
        else:
            col = (26, 66, 150)
        k = (1.15 - 0.55 * up) * self.bright
        inner = math.exp(-((u / (0.9 * w)) ** 2 + ((v + 0.1 * h) / (1.1 * h)) ** 2))  # a glow in the heart
        col = tuple(min(255.0, c * k + 70 * inner * self.bright) for c in col)
        if abs(u) < 1.3 and v > -0.5 * h:  # the ridge between the faces
            col = tuple(min(255.0, c + 70) for c in col)
        if v < -0.5 * h and abs(u) < 1.3 + 0.0:
            col = tuple(min(255.0, c + 50) for c in col)
        if d < 3.0:  # a thin rim light on the edges
            t = (1 - d / 3.0) * (0.85 if (u < 0 or v < -0.5 * h) else 0.4)
            col = tuple(lerp(c, e, t) for c, e in zip(col, (232, 246, 255)))
        alpha = 0.94 * cover
        return tuple(lerp(b, c, alpha) for b, c in zip(base, col))


def background(x, y):
    t = y / H
    r, g, b = lerp(13, 4, t), lerp(28, 9, t), lerp(68, 26, t)
    dx, dy = (x - GLOW_C[0]) / 1.15, y - GLOW_C[1]
    d = math.hypot(dx, dy)
    glow = 0.55 * math.exp(-(d / 360) ** 2) + 0.25 * math.exp(-(d / 880) ** 2)
    a = math.atan2(dy, dx)
    rays = (max(0.0, math.sin(a * 9 + 1.3)) ** 8 * 0.55 + max(0.0, math.sin(a * 5 + 0.4)) ** 10 * 0.45) * math.exp(-d / 1000) * 0.2
    r += 70 * glow + 150 * rays
    g += 150 * glow + 200 * rays
    b += 230 * glow + 255 * rays
    mist = smoothstep(H * 0.62, H, y) * (0.5 + 0.5 * math.sin(x / 260 + y / 95)) * 0.16
    return r + 60 * mist, g + 100 * mist, b + 170 * mist


def png_chunk(tag, data):
    return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data))


def main():
    crystals = [Crystal(*c) for c in CRYSTALS]
    rows, prev = [], bytes(W * 3)
    for y in range(H):
        row = bytearray(W * 3)
        near = [c for c in crystals if c.box[1] <= y <= c.box[3]]
        for x in range(W):
            col = background(x, y)
            for c in near:
                if c.box[0] <= x <= c.box[2]:
                    col = c.shade(x, y, col)
            dither = (BAYER[y & 3][x & 3] + 0.5) / 16 - 0.5
            i = x * 3
            row[i] = max(0, min(255, int(col[0] + dither + 0.5)))
            row[i + 1] = max(0, min(255, int(col[1] + dither + 0.5)))
            row[i + 2] = max(0, min(255, int(col[2] + dither + 0.5)))
        rows.append(b'\x02' + bytes((a - b) & 255 for a, b in zip(row, prev)))  # filter "up"
        prev = bytes(row)
    png = (b'\x89PNG\r\n\x1a\n' + png_chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0))
           + png_chunk(b'IDAT', zlib.compress(b''.join(rows), 9)) + png_chunk(b'IEND', b''))
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, 'wb') as f:
        f.write(png)
    print(f'{OUT}: {len(png) // 1024} KiB')


if __name__ == '__main__':
    main()
