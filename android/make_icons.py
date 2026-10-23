#!/usr/bin/env python3
"""Make the launcher icons from the game's own icon art.

Reads data/icon/xboxgame.bmp (the 64x64 icon the Xbox dashboard shows for the
title) and writes res/mipmap-*/ic_launcher.png, bilinear-scaled. Pure stdlib:
the BMP is 24-bit bottom-up, the PNG is RGB8 with filter 0.
"""
import os
import struct
import sys
import zlib

SIZES = {"mdpi": 48, "hdpi": 72, "xhdpi": 96, "xxhdpi": 144, "xxxhdpi": 192}


def read_bmp(path):
    d = open(path, "rb").read()
    off = struct.unpack_from("<I", d, 10)[0]
    w, h = struct.unpack_from("<ii", d, 18)
    bpp = struct.unpack_from("<H", d, 28)[0]
    if bpp not in (24, 32):
        sys.exit("unsupported BMP depth %d" % bpp)
    stride = ((w * bpp // 8) + 3) & ~3
    flip = h > 0
    h = abs(h)
    px = [[None] * w for _ in range(h)]
    for y in range(h):
        row = off + (h - 1 - y if flip else y) * stride
        for x in range(w):
            b, g, r = d[row + x * bpp // 8: row + x * bpp // 8 + 3]
            px[y][x] = (r, g, b)
    return px, w, h


def scale(px, w, h, n):
    out = []
    for y in range(n):
        sy = (y + 0.5) * h / n - 0.5
        y0 = max(0, min(h - 1, int(sy)))
        y1 = min(h - 1, y0 + 1)
        fy = max(0.0, min(1.0, sy - y0))
        row = []
        for x in range(n):
            sx = (x + 0.5) * w / n - 0.5
            x0 = max(0, min(w - 1, int(sx)))
            x1 = min(w - 1, x0 + 1)
            fx = max(0.0, min(1.0, sx - x0))
            p00, p10 = px[y0][x0], px[y0][x1]
            p01, p11 = px[y1][x0], px[y1][x1]
            row.append(tuple(
                int(p00[c] * (1 - fx) * (1 - fy) + p10[c] * fx * (1 - fy) +
                    p01[c] * (1 - fx) * fy + p11[c] * fx * fy + 0.5)
                for c in range(3)))
        out.append(row)
    return out


def write_png(path, px, n):
    raw = bytearray()
    for row in px:
        raw.append(0)
        for r, g, b in row:
            raw += bytes((r, g, b))

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff))

    png = (b"\x89PNG\r\n\x1a\n" +
           chunk(b"IHDR", struct.pack(">IIBBBBB", n, n, 8, 2, 0, 0, 0)) +
           chunk(b"IDAT", zlib.compress(bytes(raw), 9)) +
           chunk(b"IEND", b""))
    open(path, "wb").write(png)


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else "game/data/icon/xboxgame.bmp"
    out = sys.argv[2] if len(sys.argv) > 2 else "android/res"
    px, w, h = read_bmp(src)
    for density, n in SIZES.items():
        d = os.path.join(out, "mipmap-" + density)
        os.makedirs(d, exist_ok=True)
        write_png(os.path.join(d, "ic_launcher.png"), scale(px, w, h, n), n)
        print("%s/ic_launcher.png %dx%d" % (d, n, n))


if __name__ == "__main__":
    main()
