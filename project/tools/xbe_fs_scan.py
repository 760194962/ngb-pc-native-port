#!/usr/bin/env python3
"""扫描 XBE 各可执行段里带 fs: 段前缀的指令（线性扫描，capstone），按位移分类并列出位置。

用途：black-pc 在加载时把 fs:[0x04/0x20/0x24/0x28/0x58]（Xbox KPCR 字段）改写到 Windows TLS 槽；
本脚本预先给出黑之章用到哪些位移、在哪里，用来核对加载器改写数是否与预期一致。
线性扫描会把数据误当指令（.rdata/.data 里的命中是噪声），只有代码段的结果有意义。只读。

用法：<venv>/python.exe -I tools/xbe_fs_scan.py <default.xbe> [--sites]
"""
import collections
import struct
import sys

from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone.x86 import X86_OP_MEM, X86_REG_FS


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    show_sites = '--sites' in sys.argv
    d = open(sys.argv[1], 'rb').read()
    base = struct.unpack_from('<I', d, 0x104)[0]
    count = struct.unpack_from('<I', d, 0x11C)[0]
    hdr = struct.unpack_from('<I', d, 0x120)[0] - base
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    total = collections.Counter()
    sites = collections.defaultdict(list)
    for i in range(count):
        o = hdr + i * 0x38
        flags, va, vs, raw, rs, namep = struct.unpack_from('<IIIIII', d, o)
        name = d[namep - base:d.index(b'\0', namep - base)].decode()
        if not flags & 4:  # not executable
            continue
        code = d[raw:raw + rs]
        c = collections.Counter()
        off = 0
        while off < len(code):
            try:
                ins = next(md.disasm(code[off:off + 16], va + off, count=1))
            except StopIteration:
                off += 1
                continue
            for op in ins.operands:
                if op.type == X86_OP_MEM and op.mem.segment == X86_REG_FS:
                    if op.mem.base == 0 and op.mem.index == 0:
                        key = 'fs:[0x%x]' % op.mem.disp
                        sites[key].append((name, ins.address, ins.mnemonic + ' ' + ins.op_str))
                    else:
                        key = 'fs:[reg]'
                    c[key] += 1
            off += ins.size
        if c:
            print('%-10s %s' % (name, dict(c)))
            total.update(c)
    print('TOTAL', dict(total))
    if show_sites:
        for key in sorted(sites):
            for sec, addr, text in sites[key]:
                print('%-14s %-8s 0x%08x  %s' % (key, sec, addr, text))


if __name__ == '__main__':
    main()
