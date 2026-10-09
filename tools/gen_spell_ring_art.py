#!/usr/bin/env python3
"""Draws the spell radial's pieces: our own art (nothing taken from the game), a pointed holder in the look of the spell
gems on the bottom screen (plain and selected) and six round orbs in the elements' colours. Writes game/ui/res/ring-*.png.
Pure Python, a few seconds. Run from anywhere: python tools/gen_spell_ring_art.py"""
import math
import os
import struct
import zlib

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'game', 'ui', 'res')
HOLDER_W, HOLDER_H = 192, 160
ORB = 88
DISC = 512
SS = 3  # samples per pixel along each axis

# The holder points left; (x, y) from its centre.
HOLDER = [(-94, 0), (-30, -70), (66, -70), (94, -42), (94, 42), (66, 70), (-30, 70)]

# Element colours, the gems' top to bottom: red, blue, yellow, green, orange, teal: (highlight, body, shade).
ORBS = [
    ((255, 150, 120), (200, 48, 36), (110, 20, 20)),
    ((150, 190, 255), (56, 92, 205), (24, 40, 120)),
    ((255, 240, 130), (228, 180, 30), (150, 100, 10)),
    ((170, 240, 150), (56, 160, 60), (20, 90, 40)),
    ((255, 205, 120), (232, 120, 28), (140, 64, 14)),
    ((150, 250, 235), (36, 184, 168), (14, 100, 100)),
]


def clamp01(v):
    return max(0.0, min(1.0, v))


def edges(poly):
    """Half-planes (nx, ny, d) of a convex polygon with its points in clockwise order (y down): inside is n.p < d."""
    out = []
    for i, (ax, ay) in enumerate(poly):
        bx, by = poly[(i + 1) % len(poly)]
        dx, dy = bx - ax, by - ay
        n = math.hypot(dx, dy)
        nx, ny = dy / n, -dx / n
        out.append((nx, ny, nx * ax + ny * ay))
    return out


HOLDER_EDGES = edges(HOLDER)


def holder_dist(x, y):
    """Signed distance to the holder's outline (negative inside; exact on the edges, close to it at the corners)."""
    return max(nx * x + ny * y - d for nx, ny, d in HOLDER_EDGES)


def mix(a, b, t):
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(3))


def holder_colour(x, y, selected):
    """The holder's colour at a point, or None outside it, with its coverage in [0, 1] for anti-aliasing."""
    sd = holder_dist(x, y)
    cover = clamp01(0.5 - sd)
    if cover <= 0:
        return None, 0
    if selected:
        rim, outer, fill_a, fill_b, line = (116, 84, 8), (250, 205, 20), (252, 240, 190), (236, 214, 140), (214, 170, 20)
    else:
        rim, outer, fill_a, fill_b, line = (66, 58, 30), (122, 110, 62), (226, 218, 188), (198, 188, 148), (170, 158, 112)
    depth = -sd
    t = clamp01((y + 70) / 140)
    fill = mix(fill_a, fill_b, t)
    if depth < 3:
        col = rim
    elif depth < 9:
        col = outer
    elif depth < 12:
        col = mix(outer, fill, (depth - 9) / 3)
    else:
        col = fill
        # a thin engraved line inside, as on the game's gems
        inner = abs(depth - 20)
        if inner < 1.2:
            col = mix(fill, line, clamp01(1.2 - inner) * 0.9)
    return col, cover


def orb_colour(x, y, orb):
    r = math.hypot(x, y)
    radius = ORB / 2 - 1
    cover = clamp01(radius + 0.5 - r)
    if cover <= 0:
        return None, 0
    hi, body, shade = orb
    if r > radius - 5:
        return (50, 36, 20), cover
    # lit from the upper left
    lx, ly = x + radius * 0.35, y + radius * 0.4
    t = clamp01(math.hypot(lx, ly) / (radius * 1.15))
    col = mix(hi, body, clamp01(t * 1.6)) if t < 0.62 else mix(body, shade, clamp01((t - 0.62) / 0.38))
    spec = clamp01(1 - math.hypot(x + radius * 0.35, y + radius * 0.4) / (radius * 0.22))
    col = mix(col, (255, 255, 255), spec * 0.7)
    return col, cover


def disc_colour(x, y):
    """The backdrop: a dark translucent disc with a soft rim (a picture, so that it has no seams as a big rounded box has)."""
    radius = DISC / 2 - 1
    r = math.hypot(x, y)
    cover = clamp01(radius + 0.5 - r)
    if cover <= 0:
        return None, 0
    rim = clamp01(1 - (radius - r) / 3)
    col = mix((8, 12, 28), (200, 167, 100), rim * 0.55)
    return col, cover * (0.58 + 0.2 * rim)


def render(w, h, shader):
    rows = []
    for py in range(h):
        row = bytearray()
        for px in range(w):
            r = g = b = a = 0.0
            for sy in range(SS):
                for sx in range(SS):
                    x = px + (sx + 0.5) / SS - w / 2
                    y = py + (sy + 0.5) / SS - h / 2
                    col, cover = shader(x, y)
                    if col is not None:
                        r += col[0] * cover
                        g += col[1] * cover
                        b += col[2] * cover
                        a += cover
            n = SS * SS
            if a > 0:
                row += bytes((int(r / a + 0.5), int(g / a + 0.5), int(b / a + 0.5), int(255 * a / n + 0.5)))
            else:
                row += bytes(4)
        rows.append(bytes(row))
    return rows


def png_chunk(tag, data):
    return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data))


def write_png(name, w, h, rows):
    raw = b''.join(b'\x00' + r for r in rows)
    png = (b'\x89PNG\r\n\x1a\n' + png_chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
           + png_chunk(b'IDAT', zlib.compress(raw, 9)) + png_chunk(b'IEND', b''))
    path = os.path.join(OUT, name)
    with open(path, 'wb') as f:
        f.write(png)
    print(f'{path}: {len(png) / 1024:.1f} KiB')


def main():
    os.makedirs(OUT, exist_ok=True)
    write_png('ring-backdrop.png', DISC, DISC, render(DISC, DISC, disc_colour))
    write_png('ring-holder.png', HOLDER_W, HOLDER_H, render(HOLDER_W, HOLDER_H, lambda x, y: holder_colour(x, y, False)))
    write_png('ring-holder-selected.png', HOLDER_W, HOLDER_H,
              render(HOLDER_W, HOLDER_H, lambda x, y: holder_colour(x, y, True)))
    for i, orb in enumerate(ORBS):
        write_png(f'ring-orb{i}.png', ORB, ORB, render(ORB, ORB, lambda x, y, o=orb: orb_colour(x, y, o)))


if __name__ == '__main__':
    main()
