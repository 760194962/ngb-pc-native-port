#!/usr/bin/env python3
"""Disassemble a VA range of any XBE (section-mapped). Usage: python -I tools/xdis.py <xbe> <start_hex> <end_hex> [functions.csv]"""
import bisect, csv, struct, sys
import capstone
xbe, a0, a1 = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3], 16)
d = open(xbe, 'rb').read()
base = struct.unpack_from('<I', d, 0x104)[0]
n = struct.unpack_from('<I', d, 0x11C)[0]; sh = struct.unpack_from('<I', d, 0x120)[0] - base
secs = [struct.unpack_from('<6I', d, sh + i*0x38) for i in range(n)]
names = []
if len(sys.argv) > 4:
    rows = sorted((int(r['address'], 16), r['name']) for r in csv.DictReader(open(sys.argv[4], encoding='utf-8')))
    names = rows
starts = [a for a, _ in names]
def where(a):
    i = bisect.bisect_right(starts, a) - 1
    return '' if i < 0 else '%s+0x%x' % (names[i][1], a - names[i][0]) if a != names[i][0] else names[i][1]
for fl, va, vs, ra, rs, na in secs:
    if va <= a0 < va + vs:
        blob = d[ra + a0 - va: ra + min(a1, va + rs) - va]
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        for ins in md.disasm(blob, a0):
            note = ''
            if ins.mnemonic.startswith('call') or ins.mnemonic.startswith('j'):
                try: note = '  ; ' + where(int(ins.op_str, 16))
                except ValueError: pass
            print('%08x  %-24s %s %s%s' % (ins.address, ins.bytes.hex(), ins.mnemonic, ins.op_str, note))
        break
