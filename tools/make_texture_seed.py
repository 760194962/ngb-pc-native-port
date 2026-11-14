"""The texture pool seed shipped beside the game program (texture_pool.txt, read by loader/src/d3d/texture.cpp
texture_pool_start): for each texture shape, how many the pool keeps made ahead.

  python -I tools/make_texture_seed.py -o seed/texture_pool.txt LOG [LOG ...]

Each LOG is a black.log of a run with --hitch-detail, whose "[tex] made" lines give every texture made, its frame and
shape. A shape's count is the most of it made in one burst -- textures less than 30 frames apart, the quiet gap the
pool waits for before it is filled again (kQuietFrames in texture.cpp) -- the largest over the logs. Left out as the loader leaves them: cube maps, textures over 512 KB a copy,
and shapes past 24 MB in all (the largest counts first). Lines: width height levels d3dformat count.
"""
import argparse
import collections
import re
import sys

FMT = {'A8R8G8B8': 21, 'DXT1': 827611204, 'DXT3': 861165636, 'DXT5': 894720068}
MAX_TEXTURE, MAX_BYTES, QUIET = 512 << 10, 24 << 20, 30
MADE = re.compile(r'\[tex\] made \S+ frame (\d+): (\d+)x(\d+) x(\d+) \S+?( cube)? -> (\w+),')


def nbytes(w, h, levels, f):
    b = 0
    for i in range(levels):
        lw, lh = max(1, w >> i), max(1, h >> i)
        b += lw * lh * 4 if f == 21 else ((lw + 3) // 4) * ((lh + 3) // 4) * (8 if f == FMT['DXT1'] else 16)
    return b


def burst_counts(path):
    need, burst, last, made = collections.Counter(), collections.Counter(), None, 0
    for line in open(path, encoding='utf-8', errors='replace'):
        m = MADE.search(line)
        if not m or m[6] not in FMT:
            continue
        frame = int(m[1])
        if last is None or frame - last >= QUIET:  # every texture made counts for the gap, as in pool_take
            burst = collections.Counter()
        last = frame
        made += 1
        shape = (int(m[2]), int(m[3]), int(m[4]), FMT[m[6]])
        if m[5] or nbytes(*shape) > MAX_TEXTURE:
            continue
        burst[shape] += 1
        need[shape] = max(need[shape], burst[shape])
    return need, made


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('logs', nargs='+')
    ap.add_argument('-o', '--out', required=True)
    a = ap.parse_args()
    need = collections.Counter()
    for p in a.logs:
        n, made = burst_counts(p)
        print('%s: %d textures made, %d shapes' % (p, made, len(n)))
        for k, v in n.items():
            need[k] = max(need[k], v)
    if not need:
        sys.exit('no "[tex] made" lines: record with --hitch-detail')
    kept, total = [], 0
    for k, n in sorted(need.items(), key=lambda kv: -kv[1]):
        b = nbytes(*k)
        n = min(n, (MAX_BYTES - total) // b)
        if n <= 0:
            continue
        kept.append((k, n))
        total += n * b
    with open(a.out, 'w', newline='\n') as f:
        f.write('# texture pool seed (tools/make_texture_seed.py): width height levels d3dformat count\n')
        for k, n in sorted(kept):
            f.write('%d %d %d %d %d\n' % (*k, n))
    print('%s: %d shapes, %d textures, %.1f MB a copy' % (a.out, len(kept), sum(n for _, n in kept), total / 2**20))


if __name__ == '__main__':
    main()
