"""Difference of two same-size PNG screenshots (pure Python, no PIL): writes |a-b| x gain as a PNG and prints
where the change concentrates (a coarse grid of mean differences).

usage: png_diff.py A.png B.png OUT.png [gain]
"""
import struct
import sys
import zlib


def read_png(path):
    d = open(path, "rb").read()
    assert d[:8] == b"\x89PNG\r\n\x1a\n"
    pos, idat, w, h, ct, bd = 8, [], 0, 0, 0, 0
    while pos < len(d):
        n, t = struct.unpack_from(">I4s", d, pos)
        body = d[pos + 8:pos + 8 + n]
        if t == b"IHDR":
            w, h, bd, ct = struct.unpack_from(">IIBB", body, 0)
        elif t == b"IDAT":
            idat.append(body)
        pos += 12 + n
    assert bd == 8 and ct in (2, 6), (bd, ct)
    bpp = 3 if ct == 2 else 4
    raw = zlib.decompress(b"".join(idat))
    stride = w * bpp
    out = bytearray(h * stride)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        f = raw[p]
        p += 1
        line = bytearray(raw[p:p + stride])
        p += stride
        if f == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 255
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 255
        elif f == 3:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 255
        elif f == 4:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return w, h, bpp, out


def write_png(path, w, h, rgb):
    raw = b"".join(b"\x00" + bytes(rgb[y * w * 3:(y + 1) * w * 3]) for y in range(h))

    def chunk(t, body):
        c = t + body
        return struct.pack(">I", len(body)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


if __name__ == "__main__":
    a, b, out = sys.argv[1], sys.argv[2], sys.argv[3]
    gain = int(sys.argv[4]) if len(sys.argv) > 4 else 4
    w, h, bpp, pa = read_png(a)
    w2, h2, bpp2, pb = read_png(b)
    assert (w, h, bpp) == (w2, h2, bpp2)
    diff = bytearray(w * h * 3)
    grid = [[0] * 8 for _ in range(6)]
    for y in range(h):
        gy = y * 6 // h
        for x in range(w):
            o = (y * w + x) * bpp
            dr, dg, db = abs(pa[o] - pb[o]), abs(pa[o + 1] - pb[o + 1]), abs(pa[o + 2] - pb[o + 2])
            m = (dr + dg + db) // 3
            grid[gy][x * 8 // w] += m
            q = (y * w + x) * 3
            diff[q] = min(255, dr * gain)
            diff[q + 1] = min(255, dg * gain)
            diff[q + 2] = min(255, db * gain)
    write_png(out, w, h, diff)
    cw, ch = w // 8, h // 6
    print("mean |diff| per grid cell (rows top..bottom, cols left..right):")
    for row in grid:
        print(" ".join("%5.1f" % (v / (cw * ch)) for v in row))
