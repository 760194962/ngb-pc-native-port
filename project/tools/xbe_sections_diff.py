#!/usr/bin/env python3
"""逐段比对两份 XBE（原版 / x2.1）：名字、VA、vsize、原始字节 SHA-256，列出相同 / 不同 / 新增的段。

用途：证明库段（D3D、DSOUND、XPP、XONLINE、XNET …）在 x2.1 里字节未变，
由原版得到的库函数表可直接用于 x2.1。只读，不写任何文件。

用法：python -I tools/xbe_sections_diff.py <retail default.xbe> <x21 default.xbe>
"""
import hashlib
import struct
import sys


def sections(path):
    d = open(path, 'rb').read()
    base = struct.unpack_from('<I', d, 0x104)[0]
    count = struct.unpack_from('<I', d, 0x11C)[0]
    hdr = struct.unpack_from('<I', d, 0x120)[0] - base
    out = []
    for i in range(count):
        o = hdr + i * 0x38
        flags, va, vs, raw, rs, namep = struct.unpack_from('<IIIIII', d, o)
        name = d[namep - base:d.index(b'\0', namep - base)].decode()
        out.append(dict(name=name, flags=flags, va=va, vsize=vs, raw=raw, rawsize=rs,
                        sha=hashlib.sha256(d[raw:raw + rs]).hexdigest()))
    entry = struct.unpack_from('<I', d, 0x128)[0] ^ 0xA8FC57AB  # retail XOR key
    size = struct.unpack_from('<I', d, 0x10C)[0]
    return out, entry, size


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    a, ea, sa = sections(sys.argv[1])
    b, eb, sb = sections(sys.argv[2])
    bm = {s['name']: s for s in b}
    print('%-10s %-10s %-8s %-8s %-6s %-16s %-16s %s' % ('name', 'va', 'vsize', 'rawsize', 'flags', 'A', 'B', 'result'))
    for s in a:
        t = bm.get(s['name'])
        if t is None:
            r = 'ONLY IN A'
        elif t['va'] == s['va'] and t['vsize'] == s['vsize'] and t['sha'] == s['sha']:
            r = 'SAME'
        else:
            r = 'DIFF'
        print('%-10s 0x%08x 0x%06x 0x%06x 0x%04x %-16s %-16s %s' % (
            s['name'], s['va'], s['vsize'], s['rawsize'], s['flags'], s['sha'][:16], t['sha'][:16] if t else '-', r))
    for s in b:
        if s['name'] not in [q['name'] for q in a]:
            print('%-10s 0x%08x 0x%06x 0x%06x 0x%04x %-16s %-16s %s' % (
                s['name'], s['va'], s['vsize'], s['rawsize'], s['flags'], '-', s['sha'][:16], 'ONLY IN B'))
    print('entry A/B: 0x%x / 0x%x   size_of_image A/B: 0x%x / 0x%x' % (ea, eb, sa, sb))


if __name__ == '__main__':
    main()
