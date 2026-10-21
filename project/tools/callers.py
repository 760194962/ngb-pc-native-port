#!/usr/bin/env python3
"""List every direct call/jmp (E8/E9 rel32) to a target address in the XBE code.

Usage: python -I tools/callers.py <default.xbe> <target_hex> [symbols.csv]
"""
import bisect
import csv
import struct
import sys

import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from disasm import load_xbe


def main():
    xbe, target = sys.argv[1], int(sys.argv[2], 16)
    names = {}
    if len(sys.argv) > 3:
        for r in csv.DictReader(open(sys.argv[3], encoding='utf-8')):
            names[int(r['address'], 16)] = r['name']
    keys = sorted(names)
    for vaddr, vsize, data in load_xbe(xbe):
        for i in range(len(data) - 4):
            if data[i] not in (0xE8, 0xE9):
                continue
            rel = struct.unpack_from('<i', data, i + 1)[0]
            if vaddr + i + 5 + rel == target:
                va = vaddr + i
                k = bisect.bisect_right(keys, va) - 1
                who = '%s+0x%x' % (names[keys[k]], va - keys[k]) if k >= 0 else '?'
                print('%08x %s %s' % (va, 'call' if data[i] == 0xE8 else 'jmp', who))


if __name__ == '__main__':
    main()
