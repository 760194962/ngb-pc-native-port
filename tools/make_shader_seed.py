"""The shader seed shipped beside the game program (shader_seed.bin, read by loader/src/d3d/shader_cache.cpp
prewarm_shaders): the cache entries of test runs, so a player's first start does not compile them as each area
first shows them.

  make:  python -I tools/make_shader_seed.py -o seed/shader_seed.bin [--exclude TEXT ...] [--base SEED] CACHE_DIR [CACHE_DIR ...]

--base keeps an existing seed's records and adds the cache folders' entries to them (a new build's new shaders
without the test runs the seed was first made from).
  check: python -I tools/make_shader_seed.py --check CACHE_DIR [--seed seed/shader_seed.bin]

A CACHE_DIR is a cache\\shaders folder (entries <fnv64>.bin with their .gen epoch sidecars). Taken: entries that parse,
whose name is the hash of their own profile, flags and HLSL (as entry_path makes it), of the current generator epoch
(a .gen other than --epoch is left out; none means older than the sidecar, kept). Left out: the game's own vertex
programs (vs_* profiles other than the generated fixed-function ones, d3d/ffp_vs.cpp, whose HLSL starts
"// ffp_vs": their HLSL is the game's code translated, which the public source and package do not carry;
ngbrecomp's seed keeps only their place in default.xbe for the same reason) and entries containing an --exclude text
(a generator variant no build makes any more, e.g. tex2Dlod). Entries are written sorted by name: the same inputs
make the same file.

--check tells how many of a test run's entries (made by the build about to ship) the seed has: a seed that misses
most of them is stale (the generator changed), as ngbrecomp found with its own (5959e5c).
"""
import argparse
import glob
import os
import struct
import sys

MAGIC, VERSION = 0x5342474E, 1


def fnv1a(h, data):
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def parse(d):
    """(flags, profile, hlsl, code) or None"""
    try:
        magic, version, flags = struct.unpack_from('<III', d, 0)
        if magic != MAGIC or version != VERSION:
            return None
        at = 12
        n, = struct.unpack_from('<I', d, at); at += 4
        profile = d[at:at + n]; at += n
        n, = struct.unpack_from('<I', d, at); at += 4
        hlsl = d[at:at + n]; at += n
        n, = struct.unpack_from('<I', d, at); at += 4
        if at + n != len(d) or len(profile) == 0:
            return None
        return flags, profile, hlsl, d[at:]
    except struct.error:
        return None


def entry_name(flags, profile, hlsl):
    h = 0xCBF29CE484222325
    h = fnv1a(h, profile)
    h = fnv1a(h, struct.pack('<I', flags))
    h = fnv1a(h, hlsl)
    return '%016x.bin' % h


def epoch(path):
    try:
        with open(path + '.gen', 'rb') as f:
            b = f.read(4)
        return struct.unpack('<I', b)[0] if len(b) == 4 else 0
    except OSError:
        return 0


def collect(dirs, want_epoch, excludes, why):
    out = {}
    for d in dirs:
        for p in sorted(glob.glob(os.path.join(d, '*.bin'))):
            name = os.path.basename(p).lower()
            if name in out:
                continue
            data = open(p, 'rb').read()
            e = parse(data)
            if not e:
                why['unreadable'] += 1
                continue
            flags, profile, hlsl, code = e
            if entry_name(flags, profile, hlsl) != name:
                why['name is not its hash'] += 1
                continue
            ep = epoch(p)
            if ep and ep != want_epoch:
                why['another generator epoch'] += 1
                continue
            if profile.startswith(b'vs_') and not hlsl.startswith(b'// ffp_vs'):
                why['the game\'s vertex program'] += 1
                continue
            if any(x.encode() in hlsl for x in excludes):
                why['excluded text'] += 1
                continue
            out[name] = data
    return out


def seed_records(path):
    """name -> entry bytes of a seed's records (for --base)"""
    d = open(path, 'rb').read()
    if d[:8] != b'NGBSEED1':
        sys.exit('%s: not a shader seed' % path)
    count, = struct.unpack_from('<I', d, 8)
    at, out = 12, {}
    for _ in range(count):
        n, = struct.unpack_from('<I', d, at); at += 4
        data = d[at:at + n]; at += n
        e = parse(data)
        if e:
            out[entry_name(e[0], e[1], e[2])] = data
    return out


def read_seed(path):
    d = open(path, 'rb').read()
    if d[:8] != b'NGBSEED1':
        sys.exit('%s: not a shader seed' % path)
    count, = struct.unpack_from('<I', d, 8)
    at, names = 12, set()
    for _ in range(count):
        n, = struct.unpack_from('<I', d, at); at += 4
        e = parse(d[at:at + n]); at += n
        if e:
            names.add(entry_name(e[0], e[1], e[2]))
    return names


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dirs', nargs='+')
    ap.add_argument('-o', '--out')
    ap.add_argument('--epoch', type=int, default=1, help='kShaderEpoch of shader_cache.cpp')
    ap.add_argument('--exclude', action='append', default=[])
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--seed', default=os.path.join(os.path.dirname(__file__), '..', 'seed', 'shader_seed.bin'))
    ap.add_argument('--base', help='a seed whose records are kept, the CACHE_DIRs\' entries added to them')
    a = ap.parse_args()
    why = {'unreadable': 0, 'name is not its hash': 0, 'another generator epoch': 0, 'the game\'s vertex program': 0,
           'excluded text': 0}
    entries = collect(a.dirs, a.epoch, a.exclude, why)
    if a.base and not a.check:
        base = seed_records(a.base)
        added = sum(1 for n in entries if n not in base)
        base.update(entries)
        entries = base
        print('%d entries from %s, %d added from the cache folders' % (len(entries) - added, a.base, added))
    if a.check:
        seed = read_seed(a.seed)
        have = sum(1 for n in entries if n in seed)
        print('%d of the run\'s %d entries are in the seed (%.0f%%); the seed has %d' %
              (have, len(entries), 100.0 * have / max(1, len(entries)), len(seed)))
        return
    if not a.out:
        sys.exit('-o OUT, or --check')
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, 'wb') as f:
        f.write(b'NGBSEED1' + struct.pack('<I', len(entries)))
        for name in sorted(entries):
            f.write(struct.pack('<I', len(entries[name])) + entries[name])
    kinds = {}
    for d in entries.values():
        e = parse(d)
        k = ('fails ' if not e[3] else '') + e[1].decode()
        kinds[k] = kinds.get(k, 0) + 1
    print('%s: %d entries, %d bytes; %s' % (a.out, len(entries), os.path.getsize(a.out),
                                            ', '.join('%s %d' % kv for kv in sorted(kinds.items()))))
    print('left out: ' + ', '.join('%s %d' % kv for kv in why.items() if kv[1]))


if __name__ == '__main__':
    main()
