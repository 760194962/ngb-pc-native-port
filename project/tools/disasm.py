#!/usr/bin/env python3
"""Disassemble a virtual-address range of an XBE with capstone.

Usage: python -I tools/disasm.py <default.xbe> <start_hex> <end_hex> [symbols.csv]
Prints one instruction per line; call/jmp targets are named from the symbol table (tools/lib_function_table.py output).
"""
import csv
import struct
import sys

from capstone import Cs, CS_ARCH_X86, CS_MODE_32


def load_xbe(path):
    d = open(path, 'rb').read()
    base = struct.unpack_from('<I', d, 0x104)[0]
    nsec = struct.unpack_from('<I', d, 0x11C)[0]
    sec_hdr = struct.unpack_from('<I', d, 0x120)[0] - base
    secs = []
    for i in range(nsec):
        o = sec_hdr + i * 0x38
        flags, vaddr, vsize, raw, rawsize = struct.unpack_from('<IIIII', d, o)
        secs.append((vaddr, vsize, d[raw:raw + rawsize]))
    return secs


def read(secs, va, n):
    for vaddr, vsize, data in secs:
        if vaddr <= va < vaddr + vsize:
            off = va - vaddr
            return data[off:off + n]
    return b''


def main():
    xbe, start, end = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3], 16)
    names = {}
    if len(sys.argv) > 4:
        for r in csv.DictReader(open(sys.argv[4], encoding='utf-8')):
            names[int(r['address'], 16)] = r['name']
    secs = load_xbe(xbe)
    code = read(secs, start, end - start)
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    for ins in md.disasm(code, start):
        extra = ''
        if ins.mnemonic.startswith(('call', 'j')) and ins.op_str.startswith('0x'):
            t = int(ins.op_str, 16)
            if t in names:
                extra = '  ; ' + names[t]
            else:
                # nearest preceding symbol
                lo = max((a for a in names if a <= t), default=None)
                if lo is not None and t - lo < 0x4000:
                    extra = '  ; %s+0x%x' % (names[lo], t - lo)
        print('%08x  %-24s %s %s%s' % (ins.address, ins.bytes.hex(), ins.mnemonic, ins.op_str, extra))


if __name__ == '__main__':
    main()
