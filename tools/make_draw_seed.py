"""The draw-state seed shipped beside the game program (draw_states.bin, read by loader/src/d3d/drawwarm.cpp): the draw
states test runs recorded (their cache\\draw_states.bin), drawn once at a player's first start so the driver builds
their programs then and not in the frame an area first shows them.

  make:  python -I tools/make_draw_seed.py -o seed/draw_states.bin [--shader-seed seed/shader_seed.bin] FILE [FILE ...]
  check: python -I tools/make_draw_seed.py --check FILE [--seed seed/draw_states.bin]

A FILE is a cache\\draw_states.bin. Header: "NGBDRAW2", the record size, the count, the version of the program that
wrote it (16 bytes). Records are fixed-size (a file of another size is a different build's and is refused); the
files must be of one version, which the seed carries (the program drops learned files of another version). Taken: every distinct record. With --shader-seed, records whose shaders the shader
seed does not have are left out (a player's first start could not draw them: their shader is made only after the
game first uses it); the game's own vertex programs are such (make_shader_seed.py leaves them out). Records are
written sorted by their bytes: the same inputs make the same file.

--check tells how many of a run's records the seed has.
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_shader_seed  # noqa: E402

MAGIC = b'NGBDRAW2'
HEADER = 32


def read(path):
    d = open(path, 'rb').read()
    if d[:8] != MAGIC or len(d) < HEADER:
        sys.exit('%s: not a draw-state file of this format' % path)
    size, count = struct.unpack_from('<II', d, 8)
    version = d[16:32]
    if len(d) != HEADER + size * count:
        sys.exit('%s: %d bytes, not %d records of %d' % (path, len(d), count, size))
    return size, version, [d[HEADER + i * size:HEADER + (i + 1) * size] for i in range(count)]


def vname(v):
    return v.rstrip(b'\0').decode()


def shader_keys(seed_path):
    """The bytecode keys (FNV-1a 64 of the code, shader_cache.cpp code_key) of the shader seed's entries"""
    d = open(seed_path, 'rb').read()
    if d[:8] != b'NGBSEED1':
        sys.exit('%s: not a shader seed' % seed_path)
    count, = struct.unpack_from('<I', d, 8)
    at, keys = 12, set()
    for _ in range(count):
        n, = struct.unpack_from('<I', d, at)
        at += 4
        e = make_shader_seed.parse(d[at:at + n])
        at += n
        if e and e[3]:
            keys.add(make_shader_seed.fnv1a(0xCBF29CE484222325, e[3]))
    return keys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('files', nargs='+')
    ap.add_argument('-o', '--out')
    ap.add_argument('--shader-seed')
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--seed', default=os.path.join(os.path.dirname(__file__), '..', 'seed', 'draw_states.bin'))
    a = ap.parse_args()
    size, version, recs = None, None, set()
    for f in a.files:
        s, v, r = read(f)
        if size is not None and s != size:
            sys.exit('%s: records of %d bytes, the others %d' % (f, s, size))
        if version is not None and v != version:
            sys.exit('%s: written by %s, the others by %s' % (f, vname(v), vname(version)))
        size, version = s, v
        recs.update(r)
    if a.check:
        _, _, seed = read(a.seed)
        seed = set(seed)
        have = sum(1 for r in recs if r in seed)
        print('%d of the runs\' %d draw states are in the seed (%.0f%%); the seed has %d' % (have, len(recs), 100.0 * have / max(1, len(recs)), len(seed)))
        return
    if not a.out:
        sys.exit('-o OUT, or --check')
    left = 0
    if a.shader_seed:
        keys = shader_keys(a.shader_seed)
        kept = set()
        for r in recs:
            vs, ps = struct.unpack_from('<QQ', r, 0)
            if (vs and vs not in keys) or (ps and ps not in keys):
                left += 1
            else:
                kept.add(r)
        recs = kept
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, 'wb') as f:
        f.write(MAGIC + struct.pack('<II', size, len(recs)) + version)
        for r in sorted(recs):
            f.write(r)
    print('%s: %d draw states of %d bytes, version %s (%d bytes)%s' % (a.out, len(recs), size, vname(version), os.path.getsize(a.out),
                                                       ('; left out %d whose shaders the shader seed lacks' % left) if a.shader_seed else ''))


if __name__ == '__main__':
    main()
