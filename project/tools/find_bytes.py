#!/usr/bin/env python3
"""Search every file under a directory for a byte string (first hit per file, chunked, no interpreter dirs).

Usage: python -I tools/find_bytes.py <dir> <needle.bin> [skip_bytes] [needle_len]
"""
import os
import sys


def main():
    root, needle_path = sys.argv[1], sys.argv[2]
    skip = int(sys.argv[3]) if len(sys.argv) > 3 else 0
    n = int(sys.argv[4]) if len(sys.argv) > 4 else 64
    needle = open(needle_path, 'rb').read()[skip:skip + n]
    print('needle %d bytes: %s' % (len(needle), needle[:16].hex()))
    hits = 0
    for d, _, files in os.walk(root):
        for f in files:
            p = os.path.join(d, f)
            try:
                with open(p, 'rb') as fh:
                    off = 0
                    tail = b''
                    while True:
                        chunk = fh.read(1 << 24)
                        if not chunk:
                            break
                        buf = tail + chunk
                        i = buf.find(needle)
                        if i >= 0:
                            print('HIT %s @ 0x%x' % (p, off - len(tail) + i))
                            hits += 1
                            break
                        tail = buf[-(len(needle) - 1):]
                        off += len(chunk)
            except OSError as e:
                print('skip', p, e)
    print('done, %d hits' % hits)


if __name__ == '__main__':
    main()
