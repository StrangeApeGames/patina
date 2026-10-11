"""Fractions of a baked map (an 8-bit grey PNG from `patina bake --dump`) below 0.4 and above 0.6.
usage: map_stats.py map.png  ->  prints "<low> <high>"; for a curvature map that is "<concave> <convex>"."""
import struct
import sys
import zlib


def grey(path):
    data = open(path, "rb").read()
    pos, idat = 8, b""
    while pos < len(data):
        size, tag = struct.unpack(">I4s", data[pos:pos + 8])
        if tag == b"IHDR":
            w, h, depth, kind = struct.unpack(">IIBB", data[pos + 8:pos + 18])
        if tag == b"IDAT":
            idat += data[pos + 8:pos + 8 + size]
        pos += 12 + size
    assert depth == 8 and kind == 0, "expected an 8-bit grey PNG"
    raw, out, prev = zlib.decompress(idat), bytearray(), bytes(w)
    for y in range(h):
        f, row = raw[y * (w + 1)], bytearray(raw[y * (w + 1) + 1:(y + 1) * (w + 1)])
        for x in range(w):
            a, up, c = (row[x - 1], prev[x], prev[x - 1]) if x else (0, prev[x], 0)
            if f == 4:  # Paeth
                p = a + up - c
                pa, pb, pc = abs(p - a), abs(p - up), abs(p - c)
                pred = a if pa <= pb and pa <= pc else (up if pb <= pc else c)
            else:
                pred = (0, a, up, (a + up) // 2)[f]
            row[x] = (row[x] + pred) & 255
        out += row
        prev = bytes(row)
    return out


px = grey(sys.argv[1])
print(sum(v < 102 for v in px) / len(px), sum(v > 153 for v in px) / len(px))
