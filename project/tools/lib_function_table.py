#!/usr/bin/env python3
"""生成黑之章 x2.1 的函数表（black-pc `notes/functions.csv` 格式：address,name,size,section,refs），并统计库段的命名覆盖。

来源（都只读）：
  1. 路线 A 的 xboxrecomp 函数识别结果 `build/lift_x21/analysis/disasm/functions.json`（start/end/section/called_by）；
  2. XbSymbolDatabase 对 x2.1 的输出 `work/xbsymbols-x21.txt`（名字、调用约定、寄存器参数）；
  3. ngbrecomp 的 `tools/d3d_symbols.txt`（94 个 D3D8LTCG 名）。
输出：work/functions_x21.csv、work/symbols_x21.json（名字 → 地址、约定、参数），以及终端统计。

用法：python -I tools/lib_function_table.py
"""
import csv
import json
import os
import re
import collections

ROOT = r"E:\games\emu\ninja gaiden black Hurricane recomp2"
ROUTE_A = r"E:\games\emu\ninja gaiden black hurricane recomp"
FUNCS = os.path.join(ROUTE_A, r"build\lift_x21\analysis\disasm\functions.json")
XBSYM = os.path.join(ROOT, r"work\xbsymbols-x21.txt")
NGB_D3D = os.path.join(ROUTE_A, r"ngb-hurricane\tools\d3d_symbols.txt")
OUT_CSV = os.path.join(ROOT, r"work\functions_x21.csv")
OUT_SYM = os.path.join(ROOT, r"work\symbols_x21.json")

SYM_RE = re.compile(r'^(?P<lib>[A-Z0-9]+)__(?P<kind>FUN|VAR)__(?:(?P<cc>[a-z]+)__)?(?P<name>[^(=\s]+)(?:\((?P<params>[^)]*)\))?\s*=\s*0x(?P<addr>[0-9a-fA-F]+)')


def parse_xbsym(path):
    syms = []
    for line in open(path, encoding='utf-8', errors='replace'):
        m = SYM_RE.match(line.strip())
        if not m:
            continue
        params = []
        if m.group('params'):
            for p in m.group('params').split(','):
                p = p.strip().split()
                if len(p) >= 2:
                    params.append({'where': p[0], 'name': p[1]})
        syms.append(dict(lib=m.group('lib'), kind=m.group('kind'), cc=m.group('cc') or '', name=m.group('name'),
                         params=params, addr=int(m.group('addr'), 16)))
    return syms


def main():
    funcs = json.load(open(FUNCS))
    by_start = {int(f['start'], 16): f for f in funcs}
    syms = parse_xbsym(XBSYM)
    ngb = {}
    for line in open(NGB_D3D, encoding='utf-8'):
        m = re.match(r'^D3D8LTCG__(\S+) = 0x([0-9a-fA-F]+)', line.strip())
        if m:
            ngb[int(m.group(2), 16)] = m.group(1)

    names = {}
    for s in syms:
        if s['kind'] == 'FUN':
            names.setdefault(s['addr'], s['name'])
    for a, n in ngb.items():
        names.setdefault(a, n)

    rows = []
    for f in funcs:
        a = int(f['start'], 16)
        rows.append(dict(address='%08x' % a, name=names.get(a, 'FUN_%08x' % a), size=f['size'], section=f['section'],
                         refs=len(f.get('called_by', []))))
    rows.sort(key=lambda r: int(r['address'], 16))
    with open(OUT_CSV, 'w', newline='\n', encoding='utf-8') as fh:
        w = csv.DictWriter(fh, fieldnames=['address', 'name', 'size', 'section', 'refs'])
        w.writeheader()
        w.writerows(rows)
    json.dump(syms, open(OUT_SYM, 'w', encoding='utf-8'), indent=1)

    print('functions: %d  written %s' % (len(rows), OUT_CSV))
    sec_total = collections.Counter(r['section'] for r in rows)
    sec_named = collections.Counter(r['section'] for r in rows if not r['name'].startswith('FUN_'))
    print('%-9s %6s %6s' % ('section', 'funcs', 'named'))
    for s in sec_total:
        print('%-9s %6d %6d' % (s, sec_total[s], sec_named[s]))

    # symbols whose address is not a function start in route A's detection
    missing = [s for s in syms if s['kind'] == 'FUN' and s['addr'] not in by_start]
    print('\nFUN symbols not at a detected function start: %d' % len(missing))
    for s in missing[:40]:
        inside = [f for f in funcs if int(f['start'], 16) < s['addr'] < int(f['end'], 16)]
        print('  0x%08x %-12s %s  %s' % (s['addr'], s['lib'], s['name'], ('inside ' + inside[0]['name']) if inside else 'in no function'))
    ngb_missing = [a for a in ngb if a not in by_start]
    print('ngbrecomp D3D symbols not at a detected function start: %d %s' % (len(ngb_missing), ' '.join('0x%x' % a for a in ngb_missing)))
    cc = collections.Counter((s['lib'], s['cc']) for s in syms if s['kind'] == 'FUN')
    print('\ncalling conventions by library:', dict(cc))
    reg = [s for s in syms if s['kind'] == 'FUN' and any(p['where'] != 'psh' for p in s['params'])]
    print('functions with register parameters: %d' % len(reg))


if __name__ == '__main__':
    main()
