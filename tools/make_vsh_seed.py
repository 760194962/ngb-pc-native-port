"""The game's vertex programs for the package (vertex_programs.bin beside the game program, read by
loader/src/d3d/vshader.cpp vsh_seed_prewarm): not the programs themselves (they are the game's code) but where each
one is in default.xbe, so a player's first start translates and compiles them from the player's own copy before
the game first draws with them.

  make:  python -I tools/make_vsh_seed.py -o seed/vertex_programs.bin --xbe A/default.xbe [--xbe B/default.xbe ...] REC [REC ...]
  check: python -I tools/make_vsh_seed.py --check REC [--seed seed/vertex_programs.bin]

A REC is the file a test run wrote with --vsh-record: "NGBVREC1", then per program translated u32 words, input
mask, variant (undivided stages), profile (0 vs_2_0, 1 vs_2_a), u64 bytecode key, the words.

Each program is looked for in every --xbe given (the builds of the game: US, European, Japanese, the Chinese
translations); its offset in each one that has it goes in the seed, with a hash of its words that the program
checks against the player's copy before using an offset. A program found in none of them is left out (made at run
time, then held by the player's own cache). Output sorted: the same inputs make the same file.

--check tells how many of a run's programs the seed has.
"""
import argparse
import os
import struct
import sys

REC = b'NGBVREC1'
SEED = b'NGBVSED1'


def fnv1a64(b):
    h = 0xCBF29CE484222325
    for x in b:
        h = ((h ^ x) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def read_rec(path):
    d = open(path, 'rb').read()
    if d[:8] != REC:
        sys.exit('%s: not a --vsh-record file' % path)
    at, out = 8, []
    while at + 24 <= len(d):
        words, mask, variant, prof = struct.unpack_from('<IIII', d, at)
        key, = struct.unpack_from('<Q', d, at + 16)
        at += 24
        prog = d[at:at + words * 4]
        if len(prog) != words * 4:
            sys.exit('%s: truncated' % path)
        at += words * 4
        out.append((words, mask, variant, prof, key, prog))
    return out


def read_seed(path):
    d = open(path, 'rb').read()
    if d[:8] != SEED:
        sys.exit('%s: not a vertex program seed' % path)
    count, = struct.unpack_from('<I', d, 8)
    at, out = 12, []
    for _ in range(count):
        words, mask, variant, prof, h, key, n = struct.unpack_from('<IIIIQQI', d, at)
        at += 36 + 4 * n
        out.append((words, mask, variant, prof, h, key))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('recs', nargs='+')
    ap.add_argument('-o', '--out')
    ap.add_argument('--xbe', action='append', default=[])
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--seed', default=os.path.join(os.path.dirname(__file__), '..', 'seed', 'vertex_programs.bin'))
    a = ap.parse_args()
    progs = {}
    for f in a.recs:
        for words, mask, variant, prof, key, prog in read_rec(f):
            progs[(prog, mask, variant, prof)] = (words, key)
    if a.check:
        have = {(w, m, v, p, h) for w, m, v, p, h, _ in read_seed(a.seed)}
        n = sum(1 for (prog, m, v, p), (w, _) in progs.items() if (w, m, v, p, fnv1a64(prog)) in have)
        print('%d of the runs\' %d vertex programs are in the seed (%.0f%%)' % (n, len(progs), 100.0 * n / max(1, len(progs))))
        return
    if not a.out or not a.xbe:
        sys.exit('-o OUT and at least one --xbe, or --check')
    xbes = [open(x, 'rb').read() for x in a.xbe]
    entries, left = [], 0
    for (prog, mask, variant, prof), (words, key) in progs.items():
        offs = []
        for x in xbes:
            o = x.find(prog)
            if o >= 0 and o not in offs:
                offs.append(o)
        if not offs:
            left += 1
            continue
        entries.append(struct.pack('<IIIIQQI', words, mask, variant, prof, fnv1a64(prog), key, len(offs)) +
                       struct.pack('<%dI' % len(offs), *offs))
    entries.sort()
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, 'wb') as f:
        f.write(SEED + struct.pack('<I', len(entries)))
        for e in entries:
            f.write(e)
    print('%s: %d vertex programs (%d bytes); %d found in none of the %d default.xbe given, left out' %
          (a.out, len(entries), os.path.getsize(a.out), left, len(xbes)))


if __name__ == '__main__':
    main()
