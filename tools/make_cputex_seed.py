"""The ripple texture seed shipped beside the game program (cpu_textures.bin, read by loader/src/ngb_cpu_textures.cpp):
the textures FUN_000C64D0 (x2.1) computes on the CPU, kept by test runs in their cache\\cpu_textures.bin, so a new
player's first start copies them as later starts do (they were the one fixed spike left on a first start). Data the
game's code computes from its arguments and constants, no code (the user, 2026-11-16).

  make:  python -I tools/make_cputex_seed.py -o seed/cpu_textures.bin FILE [FILE ...]
  check: python -I tools/make_cputex_seed.py --check FILE [--seed seed/cpu_textures.bin]

A FILE is a cache\\cpu_textures.bin (or an earlier seed): "NGBCPUT1", a u32 count, then per texture a 32-byte key
(u64 hash of the build's constants, u32 width, height, three more arguments, the x87 control word) and a u32 size
with that many bytes (width x height x 2, V8U8). Every distinct key is taken (the builds' constants differ by their
hash, so one seed serves them all); two files holding one key with different bytes stop the tool. Written sorted by
key: the same inputs make the same file.
"""
import argparse
import os
import struct
import sys

MAGIC = b'NGBCPUT1'


def read(path):
    d = open(path, 'rb').read()
    if d[:8] != MAGIC:
        sys.exit('%s: not a ripple texture file' % path)
    count, = struct.unpack_from('<I', d, 8)
    at, out = 12, {}
    for _ in range(count):
        key = d[at:at + 32]
        n, = struct.unpack_from('<I', d, at + 32)
        w, h = struct.unpack_from('<II', key, 8)
        if n != w * h * 2:
            sys.exit('%s: a texture of %d bytes for %dx%d' % (path, n, w, h))
        out[key] = d[at + 36:at + 36 + n]
        at += 36 + n
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('files', nargs='+')
    ap.add_argument('-o', '--out')
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--seed', default=os.path.join(os.path.dirname(__file__), '..', 'seed', 'cpu_textures.bin'))
    a = ap.parse_args()
    tex = {}
    for f in a.files:
        for k, v in read(f).items():
            if k in tex and tex[k] != v:
                sys.exit('%s: key %s holds other bytes than an earlier file' % (f, k.hex()))
            tex[k] = v
    if a.check:
        seed = read(a.seed)
        have = sum(1 for k, v in tex.items() if seed.get(k) == v)
        print('%d of the runs\' %d ripple textures are in the seed (%.0f%%); the seed has %d' % (have, len(tex), 100.0 * have / max(1, len(tex)), len(seed)))
        return
    if not a.out:
        sys.exit('-o OUT, or --check')
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, 'wb') as f:
        f.write(MAGIC + struct.pack('<I', len(tex)))
        for k in sorted(tex):
            f.write(k + struct.pack('<I', len(tex[k])) + tex[k])
    builds = len(set(k[:8] for k in tex))
    print('%s: %d ripple textures of %d build(s), %d bytes' % (a.out, len(tex), builds, os.path.getsize(a.out)))


if __name__ == '__main__':
    main()
