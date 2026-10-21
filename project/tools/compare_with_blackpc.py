#!/usr/bin/env python3
"""把黑之章 x2.1 与 black-pc（BLACK）作两项对照，只读：

1. 内核导入集合：用 black-pc 的 tools/xbeinfo.py 解析 x2.1，与 black-pc notes/phase1-xbe-report.txt 里 BLACK 的导入表比对，
   列出"黑之章独有"（black-pc 没实现、要新写的 HLE）与"BLACK 独有"。
2. D3D 函数：ngbrecomp 的 src/d3d/d3d_funcs.h（黑之章调用的 94 个 D3D8LTCG 符号）对照 black-pc loader 里已实现的 HLE 表
   （d3d/device.cpp、draw.cpp、resources.cpp 的 H(...) 条目），列出已有 / 缺少 / BLACK 独有。

用法：python -I tools/compare_with_blackpc.py <black-pc 目录> <x21 default.xbe> <ngb-hurricane 目录>
"""
import os
import re
import subprocess
import sys


def imports_from_report(text):
    m = re.search(r'Kernel imports \(\d+\)\n(.*)', text, re.S)
    out = {}
    for line in m.group(1).splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[0].isdigit():
            out[parts[1]] = int(parts[0])
    return out


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    bp, xbe, ngb = sys.argv[1:]
    rep = subprocess.run([sys.executable, '-I', os.path.join(bp, 'tools', 'xbeinfo.py'), xbe],
                         capture_output=True, text=True, check=True).stdout
    ngb_imp = imports_from_report(rep)
    blk_imp = imports_from_report(open(os.path.join(bp, 'notes', 'phase1-xbe-report.txt'), encoding='utf-8', errors='replace').read())
    print('== kernel imports: NGB %d, BLACK %d, shared %d' % (len(ngb_imp), len(blk_imp), len(set(ngb_imp) & set(blk_imp))))
    only_ngb = sorted(set(ngb_imp) - set(blk_imp), key=lambda n: ngb_imp[n])
    only_blk = sorted(set(blk_imp) - set(ngb_imp), key=lambda n: blk_imp[n])
    print('NGB only (%d):' % len(only_ngb))
    for n in only_ngb:
        print('  %4d %s' % (ngb_imp[n], n))
    print('BLACK only (%d): %s' % (len(only_blk), ' '.join(only_blk)))

    funcs_h = open(os.path.join(ngb, 'src', 'd3d', 'd3d_funcs.h'), encoding='utf-8').read()
    ngb_d3d = [n for n in re.findall(r'D3DF_([A-Za-z0-9_]+) = \d+', funcs_h) if n != 'COUNT']
    src = ''
    for f in ('device.cpp', 'draw.cpp', 'resources.cpp'):
        src += open(os.path.join(bp, 'loader', 'src', 'd3d', f), encoding='utf-8').read()
    bp_d3d = set(re.findall(r'H\(0x[0-9a-f]+, ([A-Za-z0-9_]+),', src))
    bp_d3d |= set(re.findall(r'NATIVE\(0x[0-9a-f]+, "([A-Za-z0-9_]+)', src))
    have = [n for n in ngb_d3d if n in bp_d3d]
    miss = [n for n in ngb_d3d if n not in bp_d3d]
    print('\n== D3D: NGB uses %d symbols, black-pc implements %d' % (len(ngb_d3d), len(bp_d3d)))
    print('already in black-pc (%d): %s' % (len(have), ' '.join(have)))
    print('missing in black-pc (%d): %s' % (len(miss), ' '.join(miss)))
    print('BLACK only (%d): %s' % (len(bp_d3d - set(ngb_d3d)), ' '.join(sorted(bp_d3d - set(ngb_d3d)))))


if __name__ == '__main__':
    main()
