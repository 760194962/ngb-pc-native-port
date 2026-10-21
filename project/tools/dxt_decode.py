"""Decode raw Xbox/PC DXT1 or DXT5 block data into a PNG (for looking at texture memory dumps).

usage: dxt_decode.py IN.bin OFFSET WIDTH HEIGHT dxt1|dxt5 OUT.png
"""
import struct
import sys
import zlib


def rgb565(v):
    r = (v >> 11) & 31
    g = (v >> 5) & 63
    b = v & 31
    return (r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2)


def decode(data, w, h, fmt):
    px = bytearray(w * h * 4)
    bs = 8 if fmt == "dxt1" else 16
    off = 0
    for by in range(h // 4):
        for bx in range(w // 4):
            blk = data[off:off + bs]
            off += bs
            alpha = [255] * 16
            if fmt == "dxt5":
                a0, a1 = blk[0], blk[1]
                bits = int.from_bytes(blk[2:8], "little")
                pal = [a0, a1]
                if a0 > a1:
                    pal += [((7 - i) * a0 + i * a1) // 7 for i in range(1, 7)]
                else:
                    pal += [((5 - i) * a0 + i * a1) // 5 for i in range(1, 5)] + [0, 255]
                alpha = [pal[(bits >> (3 * i)) & 7] for i in range(16)]
                blk = blk[8:]
            c0, c1 = struct.unpack_from("<HH", blk, 0)
            idx = struct.unpack_from("<I", blk, 4)[0]
            p0, p1 = rgb565(c0), rgb565(c1)
            if c0 > c1 or fmt == "dxt5":
                cols = [p0, p1, tuple((2 * a + b) // 3 for a, b in zip(p0, p1)), tuple((a + 2 * b) // 3 for a, b in zip(p0, p1))]
            else:
                cols = [p0, p1, tuple((a + b) // 2 for a, b in zip(p0, p1)), (0, 0, 0)]
                alpha = [a if ((idx >> (2 * i)) & 3) != 3 else 0 for i, a in enumerate(alpha)]
            for i in range(16):
                r, g, b = cols[(idx >> (2 * i)) & 3]
                x = bx * 4 + i % 4
                y = by * 4 + i // 4
                o = (y * w + x) * 4
                px[o:o + 4] = bytes((r, g, b, alpha[i]))
    return px


def write_png(path, w, h, rgba):
    raw = b"".join(b"\x00" + bytes(rgba[y * w * 4:(y + 1) * w * 4]) for y in range(h))

    def chunk(t, d):
        c = t + d
        return struct.pack(">I", len(d)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


if __name__ == "__main__":
    src, off, w, h, fmt, out = sys.argv[1], int(sys.argv[2], 0), int(sys.argv[3]), int(sys.argv[4]), sys.argv[5], sys.argv[6]
    data = open(src, "rb").read()[off:]
    write_png(out, w, h, decode(data, w, h, fmt))
    print("wrote", out)
