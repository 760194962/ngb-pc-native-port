"""The Hurricane patches' base image ("NINJA GAIDEN BLACK.iso") written out from the user's own Redump disc.

    python -I tools/make_base_iso.py "Ninja Gaiden Black (USA) (En,Ja).iso" "NINJA GAIDEN BLACK.iso"

The launcher never needs this file: it builds the same bytes in memory from a Redump image (launcher/src/patch.cpp,
Base::make / Base::read). Making a whole-disc patch does need it on disk, as xdelta3's source, e.g. the x2.1 menu patch
(vb-jp-x2.1-menu-redump.xdelta: the previous patch decoded against it, the new default.xbe written in, encoded with
`xdelta3 -e -9 -S lzma -s base.iso new.iso out.xdelta`). So the base image can be deleted after each use and remade.

Layout as patch.cpp reads it, from launcher/src/ngb_base_layout.inc (made by tools/gen_ngb_base_layout.py; layout only,
no game data): zeros, extract-xiso's banner runs, the volume header at 0x10000, the directory tables, each file of
the tables copied from the Redump disc and padded with 0xFF to its sector's end, then the certificate bytes changed.
The result is checked against the base image's SHA-1.
"""
import hashlib
import os
import re
import struct
import sys

BASE_SHA1 = 'd35859d1f3a43ce793dbe3e914a5188f4e83ad67'
LAYOUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'launcher', 'src', 'ngb_base_layout.inc')


def load_layout(path):
    src = open(path, encoding='utf-8').read()
    arrays = {m.group(1): bytes(int(x, 16) for x in re.findall(r'0x([0-9A-Fa-f]{2})', m.group(3)))
              for m in re.finditer(r'const uint8_t (\w+)\[(\d+)\] = \{[^\n]*\n(.*?)\n\};', src, re.S)}
    for m in re.finditer(r'const uint8_t (\w+)\[(\d+)\]', src):
        assert len(arrays[m.group(1)]) == int(m.group(2)), m.group(1)
    size = int(re.search(r'kBaseSize = (\d+)u;', src).group(1))
    part = int(re.search(r'kRedumpPartition = 0x([0-9A-Fa-f]+)u;', src).group(1), 16)

    def table(name):
        return re.search(r'\} %s\[\] = \{\n(.*?)\n\};' % name, src, re.S).group(1)

    banner = [(int(o, 16), int(n), arrays[a]) for o, n, a in re.findall(r'\{0x([0-9A-F]+), (\d+), (\w+)\}', table('kBanner'))]
    dirs = [(int(s, 16), int(n), p, arrays[a]) for s, n, p, a in
            re.findall(r'\{0x([0-9A-F]+), (\d+), "([^"]*)", (\w+)\}', table('kDirs'))]
    fixes = [(p, int(o, 16), int(v, 16)) for p, o, v in
             re.findall(r'\{"([^"]*)", 0x([0-9A-F]+), 0x([0-9A-F]+)\}', table('kFixes'))]
    return size, part, arrays['kBaseHeader'], banner, dirs, fixes


def entries(t, size):
    """An XDVDFS directory table's entries: (name, sector, size, is_dir), in patch.cpp's order."""
    out = []

    def node(o, depth):
        if depth > 64 or o * 4 + 14 > size:
            return
        left, right, sec, sz, attr, nl = struct.unpack_from('<HHIIBB', t, o * 4)
        if left == 0xFFFF:
            return
        out.append((t[o * 4 + 14:o * 4 + 14 + nl].decode('latin1'), sec, sz, bool(attr & 0x10)))
        if left:
            node(left, depth + 1)
        if right:
            node(right, depth + 1)
    node(0, 0)
    return out


def redump_files(f, part):
    f.seek(part + 0x10000)
    hdr = f.read(2048)
    if hdr[:20] != b'MICROSOFT*XBOX*MEDIA':
        raise SystemExit('no Xbox game partition at the Redump offset: not a Redump image')
    root, rsize = struct.unpack_from('<II', hdr, 20)
    files = {}

    def walk(sec, size, path):
        f.seek(part + sec * 2048)
        t = f.read(size)
        for name, s, sz, is_dir in entries(t, size):
            if is_dir:
                if sz:
                    walk(s, sz, path + name + '/')
            else:
                files[(path + name).lower()] = (part + s * 2048, sz)
    walk(root, rsize, '/')
    return files


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    redump, out = sys.argv[1:3]
    size, part, header, banner, dirs, fixes = load_layout(LAYOUT)
    red = open(redump, 'rb')
    files = redump_files(red, part)

    with open(out, 'wb') as o:
        o.truncate(size)  # zeros between the pieces
        for off, n, b in banner:
            o.seek(off)
            o.write(b[:n])
        o.seek(0x10000)
        o.write(header)
        starts = {}
        done = 0
        for sec, tsize, path, t in dirs:
            o.seek(sec * 2048)
            o.write(t)
            for name, s, sz, is_dir in entries(t, tsize):
                if is_dir:
                    continue
                p = path + name
                src = files.get(p.lower())
                if not src or src[1] != sz:
                    raise SystemExit('the disc has no %s as the patches expect' % p)
                red.seek(src[0])
                o.seek(s * 2048)
                left = sz
                while left:
                    chunk = red.read(min(left, 8 << 20))
                    o.write(chunk)
                    left -= len(chunk)
                o.write(b'\xFF' * ((2048 - sz % 2048) % 2048))
                starts[p] = s * 2048
                done += sz
                print('\r  %.2f GB' % (done / 1e9), end='', flush=True)
        for p, off, v in fixes:
            o.seek(starts[p] + off)
            o.write(bytes([v]))
    print()

    h = hashlib.sha1()
    with open(out, 'rb') as f:
        while True:
            b = f.read(16 << 20)
            if not b:
                break
            h.update(b)
    if h.hexdigest() != BASE_SHA1:
        raise SystemExit('%s: SHA-1 %s, not the base image (%s)' % (out, h.hexdigest(), BASE_SHA1))
    print('%s: %d bytes, SHA-1 %s (the base image)' % (out, size, h.hexdigest()))


if __name__ == '__main__':
    main()
