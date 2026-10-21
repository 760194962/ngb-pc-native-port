#!/usr/bin/env python3
"""Export a title's save data (UDATA / TDATA) from an xemu Xbox HDD image (qcow2, FATX E partition). Read-only.

Usage: python -I tools/xemu_save_export.py <xbox_hdd.qcow2> <title id hex, e.g. 5443000d> <out dir> [--list]

Writes <out dir>\\UDATA\\<title>\\... and <out dir>\\TDATA\\<title>\\... with the FATX modification times.
Uses route A's stdlib qcow2 reader (tools\\qcow2.py next to this script's ROUTE_A_TOOLS).
"""
import os
import struct
import sys
import time

ROUTE_A_TOOLS = r"E:\games\emu\ninja gaiden black local\tools"
sys.path.insert(0, ROUTE_A_TOOLS)
from qcow2 import Qcow2  # noqa: E402

E_OFF, E_SIZE = 0xABE80000, 0x131F00000


class Fatx:
    def __init__(self, q, off, size):
        self.q, self.off = q, off
        hdr = q.read(off, 0x1000)
        if hdr[:4] != b"FATX":
            raise SystemExit("no FATX signature at 0x%x" % off)
        self.spc = struct.unpack_from("<I", hdr, 8)[0]
        self.root = struct.unpack_from("<I", hdr, 12)[0]
        self.cs = self.spc * 512
        n = size // self.cs
        self.w = 2 if n < 0xFFF0 else 4
        self.fat_off = off + 0x1000
        self.fatsz = (n * self.w + 0xFFF) & ~0xFFF
        self.data = self.fat_off + self.fatsz

    def chain(self, c):
        out = []
        end = 0xFFF0 if self.w == 2 else 0xFFFFFFF0
        while 0 < c < end and len(out) < 1 << 20:
            out.append(c)
            b = self.q.read(self.fat_off + c * self.w, self.w)
            c = struct.unpack("<H" if self.w == 2 else "<I", b)[0]
        return out

    def read_chain(self, first):
        return b"".join(self.q.read(self.data + (c - 1) * self.cs, self.cs) for c in self.chain(first))

    def listdir(self, first):
        raw = self.read_chain(first)
        ents = []
        for k in range(0, len(raw), 64):
            nl = raw[k]
            if nl in (0x00, 0xFF):
                break
            if nl == 0xE5:
                continue
            name = raw[k + 2:k + 2 + nl].decode("latin-1")
            attr = raw[k + 1]
            fc, fs, mt, mdate = struct.unpack_from("<IIHH", raw, k + 44)
            ents.append((name, attr, fc, fs, mdate, mt))
        return ents


def fatx_time(d, t):
    try:
        return time.mktime(((d >> 9) + 2000, (d >> 5) & 15, d & 31, t >> 11, (t >> 5) & 63, (t & 31) * 2, 0, 0, -1))
    except (ValueError, OverflowError):
        return None


def find(fs, first, name):
    for e in fs.listdir(first):
        if e[0].lower() == name.lower() and e[1] & 0x10:
            return e[2]
    return None


def export(fs, first, out, listing, depth=0):
    total = 0
    for name, attr, fc, size, md, mt in fs.listdir(first):
        p = os.path.join(out, name)
        if attr & 0x10:
            print("%s%s\\" % ("  " * depth, name))
            if not listing:
                os.makedirs(p, exist_ok=True)
            total += export(fs, fc, p, listing, depth + 1)
        else:
            print("%s%-32s %9d  %s" % ("  " * depth, name, size, time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(fatx_time(md, mt))) if fatx_time(md, mt) else "?"))
            if not listing:
                data = fs.read_chain(fc)[:size]
                with open(p, "wb") as f:
                    f.write(data)
                ts = fatx_time(md, mt)
                if ts:
                    os.utime(p, (ts, ts))
            total += 1
    return total


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    img, title, out = sys.argv[1], sys.argv[2].lower(), sys.argv[3]
    listing = "--list" in sys.argv
    q = Qcow2(img)
    fs = Fatx(q, E_OFF, E_SIZE)
    print("E partition: cluster %d bytes, FAT%d, data at 0x%x" % (fs.cs, 16 if fs.w == 2 else 32, fs.data))
    n = 0
    for top in ("UDATA", "TDATA"):
        d = find(fs, fs.root, top)
        if d is None:
            print("no %s" % top)
            continue
        t = find(fs, d, title)
        if t is None:
            print("%s\\%s: not present" % (top, title))
            continue
        dest = os.path.join(out, top, title)
        print("== %s\\%s" % (top, title))
        if not listing:
            os.makedirs(dest, exist_ok=True)
        n += export(fs, t, dest, listing, 1)
    print("%d files %s" % (n, "listed" if listing else "written to " + out))


if __name__ == "__main__":
    main()
