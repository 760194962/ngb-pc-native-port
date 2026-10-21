#!/usr/bin/env python3
"""附带 XBE 的函数表（black-pc functions.csv 格式：address,name,size,section,refs）。

来源（都在本项目 work/ 下）：
  1. 路线 A xboxrecomp 反汇编器对该 XBE 的输出 work/disasm_<名>/functions.json（start/end/section/called_by）；
  2. XbSymbolDatabase 输出 work/xbsymbols-<名>.txt（名字、调用约定、参数位置）。
输出：work/functions_<名>.csv、work/symbols_<名>.json，以及每个区段的命名覆盖统计。

用法：python -I tools/xbe_function_table.py NinjaTrilogy
"""
import collections
import csv
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
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
        syms.append(dict(lib=m.group('lib'), kind=m.group('kind'), cc=m.group('cc') or '', name=m.group('name'), params=params,
                         addr=int(m.group('addr'), 16)))
    return syms


def main():
    name = sys.argv[1]
    funcs = json.load(open(os.path.join(ROOT, 'work', 'disasm_' + name, 'functions.json')))
    if isinstance(funcs, dict):
        funcs = funcs.get('functions', list(funcs.values()))
    syms = parse_xbsym(os.path.join(ROOT, 'work', 'xbsymbols-%s.txt' % name))
    names = {}
    for s in syms:
        if s['kind'] == 'FUN':
            names.setdefault(s['addr'], s['name'])
    rows = []
    starts = set()
    for f in funcs:
        a = int(f['start'], 16)
        starts.add(a)
        rows.append(dict(address='%08x' % a, name=names.get(a, 'FUN_%08x' % a), size=f['size'], section=f['section'],
                         refs=len(f.get('called_by', []))))
    rows.sort(key=lambda r: int(r['address'], 16))
    out_csv = os.path.join(ROOT, 'work', 'functions_%s.csv' % name)
    with open(out_csv, 'w', newline='\n', encoding='utf-8') as fh:
        w = csv.DictWriter(fh, fieldnames=['address', 'name', 'size', 'section', 'refs'])
        w.writeheader()
        w.writerows(rows)
    json.dump(syms, open(os.path.join(ROOT, 'work', 'symbols_%s.json' % name), 'w', encoding='utf-8'), indent=1)
    print('%s: %d functions -> %s' % (name, len(rows), out_csv))
    tot = collections.Counter(r['section'] for r in rows)
    named = collections.Counter(r['section'] for r in rows if not r['name'].startswith('FUN_'))
    for s in tot:
        print('  %-8s %5d functions, %4d named' % (s, tot[s], named[s]))
    missing = [s for s in syms if s['kind'] == 'FUN' and s['addr'] not in starts]
    print('  FUN symbols not at a detected function start: %d %s' % (len(missing), ' '.join('%s@%x' % (s['name'], s['addr']) for s in missing[:12])))


if __name__ == '__main__':
    main()
