"""Crop a region out of a PNG screenshot (pure Python): png_crop.py IN.png X Y W H OUT.png"""
import sys

from png_diff import read_png, write_png

if __name__ == "__main__":
    src, x0, y0, cw, ch, out = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5]), sys.argv[6]
    w, h, bpp, px = read_png(src)
    rgb = bytearray(cw * ch * 3)
    for y in range(ch):
        for x in range(cw):
            o = ((y0 + y) * w + x0 + x) * bpp
            q = (y * cw + x) * 3
            rgb[q:q + 3] = px[o:o + 3]
    write_png(out, cw, ch, rgb)
    print("wrote", out)
