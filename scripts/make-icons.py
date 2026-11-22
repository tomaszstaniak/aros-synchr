#!/usr/bin/env python3
"""Write Synchr's AROS PNG icons (.info) from drawings made in code.

AROS reads a PNG whose icOn chunk carries the DiskObject attributes
(workbench/libs/icon/diskobjPNGio.c). The drawing helpers follow
aros-dockbar's scripts/make-icons.py (same author, MIT).

usage: make-icons.py OUTPUT_DIR
Writes Synchr.info (the program), drawer.info (the drawer) and ReadMe.info.
"""
import math
import struct
import sys
import zlib
from pathlib import Path

SIZE = 64
SS = 4  # supersampling per axis

ATTR_DRAWERX, ATTR_DRAWERY = 0x80001003, 0x80001004
ATTR_DRAWERWIDTH, ATTR_DRAWERHEIGHT = 0x80001005, 0x80001006
ATTR_STACKSIZE, ATTR_DEFAULTTOOL = 0x80001009, 0x8000100A
ATTR_TYPE, ATTR_FRAMELESS = 0x8000100F, 0x80001010
WBDRAWER, WBTOOL, WBPROJECT = 2, 3, 4

TEAL = (0x23, 0x9a, 0x8c, 255)
BLUE = (0x35, 0x80, 0xd4, 255)
SHADOW = (0, 0, 0, 70)


def rounded(px, py, x0, y0, x1, y1, r):
    """Signed distance to a rounded rectangle, negative inside."""
    cx, cy = min(max(px, x0 + r), x1 - r), min(max(py, y0 + r), y1 - r)
    if x0 + r <= px <= x1 - r or y0 + r <= py <= y1 - r:
        return max(x0 - px, px - x1, y0 - py, py - y1)
    return math.hypot(px - cx, py - cy) - r


def rr(x0, y0, x1, y1, r):
    return lambda px, py: rounded(px, py, x0, y0, x1, y1, r)


def arc(cx, cy, radius, width, a0, a1):
    """A ring segment between the angles a0..a1 (degrees, counter-clockwise
    from three o'clock, y up)."""
    def dist(px, py):
        t = math.degrees(math.atan2(-(py - cy), px - cx)) % 360
        lo, hi = a0 % 360, a1 % 360
        inside = lo <= t <= hi if lo <= hi else (t >= lo or t <= hi)
        if not inside:
            return 1
        return abs(math.hypot(px - cx, py - cy) - radius) - width / 2
    return dist


def triangle(a, b, c):
    def side(p, q, r):
        return (p[0] - r[0]) * (q[1] - r[1]) - (q[0] - r[0]) * (p[1] - r[1])

    def dist(px, py):
        p = (px, py)
        d1, d2, d3 = side(p, a, b), side(p, b, c), side(p, c, a)
        neg = d1 < 0 or d2 < 0 or d3 < 0
        pos = d1 > 0 or d2 > 0 or d3 > 0
        return 1 if (neg and pos) else -1
    return dist


def head(cx, cy, radius, angle, size):
    """An arrowhead at the end of an arc that runs clockwise on the screen."""
    t = math.radians(angle)
    px, py = cx + radius * math.cos(t), cy - radius * math.sin(t)
    dx, dy = math.sin(t), math.cos(t)          # clockwise on the screen
    nx, ny = math.cos(t), -math.sin(t)         # outwards
    tip = (px + dx * size, py + dy * size)
    return triangle(tip, (px + nx * size * .85, py + ny * size * .85), (px - nx * size * .85, py - ny * size * .85))


def sync_parts(cx, cy, radius, width, shadow=True):
    parts = []
    for off, colours in (((1.5, 1.5), (SHADOW, SHADOW)), ((0, 0), (TEAL, BLUE))):
        if off != (0, 0) and not shadow:
            continue
        x, y = cx + off[0], cy + off[1]
        parts += [(arc(x, y, radius, width, 35, 165), colours[0]), (head(x, y, radius, 35, width * 1.25), colours[0]),
                  (arc(x, y, radius, width, 215, 345), colours[1]), (head(x, y, radius, 215, width * 1.25), colours[1])]
    return parts


def render(shapes):
    out = bytearray()
    n = SS * SS
    for y in range(SIZE):
        out.append(0)
        for x in range(SIZE):
            acc = [0.0, 0.0, 0.0, 0.0]
            for sy in range(SS):
                for sx in range(SS):
                    px, py = x + (sx + .5) / SS, y + (sy + .5) / SS
                    c = [0.0, 0.0, 0.0, 0.0]
                    for dist, (r, g, b, a) in shapes:
                        if dist(px, py) <= 0:
                            fa = a / 255
                            c = [r * fa + c[0] * (1 - fa), g * fa + c[1] * (1 - fa),
                                 b * fa + c[2] * (1 - fa), fa + c[3] * (1 - fa)]
                    for i in range(4):
                        acc[i] += c[i]
            a = acc[3] / n
            if a > 0:
                out += bytes(int(min(255, acc[i] / n / a + .5)) for i in range(3))
            else:
                out += b"\0\0\0"
            out.append(int(a * 255 + .5))
    return bytes(out)


def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


def icon_png(pixels, attrs):
    body = b""
    for tag, value in attrs:
        body += struct.pack(">I", tag)
        body += value.encode("latin-1") + b"\0" if isinstance(value, str) else struct.pack(">I", value)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", SIZE, SIZE, 8, 6, 0, 0, 0))
            + chunk(b"icOn", body) + chunk(b"IDAT", zlib.compress(pixels, 9)) + chunk(b"IEND", b""))


def tool_icon():
    return render(sync_parts(31, 31, 20, 8))


def drawer_icon():
    back = (0x8a, 0x93, 0x9b, 255)
    front = (0xc9, 0xcf, 0xd4, 255)
    return render([(rr(6, 10, 30, 22, 3), back), (rr(6, 14, 58, 54, 4), back),
                   (rr(6, 20, 58, 54, 4), front)] + sync_parts(32, 37, 11, 5, shadow=False))


def readme_icon():
    page = (0xf4, 0xf4, 0xf0, 255)
    lines = [(rr(18, y, 46 - (8 if y > 40 else 0), y + 2, 1), (0x80, 0x88, 0x90, 255)) for y in range(18, 50, 6)]
    return render([(rr(13, 7, 53, 59, 3), (0, 0, 0, 60)), (rr(12, 6, 52, 58, 3), (0x90, 0x96, 0x9c, 255)),
                   (rr(13, 7, 51, 57, 2), page)] + lines)


def main():
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    icons = {
        "Synchr.info": (tool_icon(), [(ATTR_TYPE, WBTOOL), (ATTR_STACKSIZE, 65536), (ATTR_FRAMELESS, 1)]),
        "drawer.info": (drawer_icon(), [(ATTR_TYPE, WBDRAWER), (ATTR_FRAMELESS, 1),
                                        (ATTR_DRAWERX, 80), (ATTR_DRAWERY, 80),
                                        (ATTR_DRAWERWIDTH, 380), (ATTR_DRAWERHEIGHT, 180)]),
        "ReadMe.info": (readme_icon(), [(ATTR_TYPE, WBPROJECT), (ATTR_FRAMELESS, 1),
                                        (ATTR_DEFAULTTOOL, "SYS:Utilities/MultiView")]),
    }
    for name, (pixels, attrs) in icons.items():
        (out / name).write_bytes(icon_png(pixels, attrs))
        print(out / name)


if __name__ == "__main__":
    main()
