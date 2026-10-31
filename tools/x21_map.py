"""Carry Ninja Gaiden Black x2.1's HLE entries over to another build of the same libraries by matching code.

Ninja Gaiden (2004)'s title update uses XDK 5659 libraries, but its D3D8LTCG, DSOUND and XPP functions are instruction for
instruction x2.1's (addresses aside): 740 functions pair one to one, and the D3D data is x2.1's moved by a constant
(2026-10-31, work/ng04/map_data.py). So every entry the loader has for x2.1, with the register conventions verified there,
holds at the matching function of the other build.

- x21_entries(src): x2.1's entries per translation unit, read from the loader's sources (the kNgbTable blocks with their
  #include'd ngb/*_table.inc, and d3d_ngb.cpp's kTable): (address, name, implementation, parameter sources, returns).
- CodeMap(x21 xbe, x21 functions.csv, other xbe, other functions.csv): function pairs whose instructions are equal with
  the addresses masked and unique on both sides within a section; `func(a)` maps an address inside a paired function,
  `data(a)` an address the paired functions refer to (majority vote over every operand pair).
Output holds addresses only, no game code.
"""
import collections
import csv
import os
import re
import struct

from capstone import Cs, CS_ARCH_X86, CS_MODE_32

_MD = Cs(CS_ARCH_X86, CS_MODE_32)
_NUM = re.compile(r'0x[0-9a-f]{5,8}\b')

# translation unit -> (source file, the array holding x2.1's entries)
X21_TABLES = {
    'device': ('d3d/device.cpp', 'kNgbTable'),
    'draw': ('d3d/draw.cpp', 'kNgbTable'),
    'resources': ('d3d/resources.cpp', 'kNgbTable'),
    'dsound': ('audio/dsound.cpp', 'kNgbTable'),
    'input': ('input/xinput.cpp', 'kNgbTable'),
    'ngbextra': ('ngb/d3d_ngb.cpp', 'kTable'),
}
_BRACE = re.compile(r'^\s*\{0x([0-9a-fA-F]+)u,\s*"([^"]+)",\s*\(void \*\)&(\w+),\s*"([^"]*)",\s*(true|false)\}')
_MACRO = re.compile(r'^\s*(H|HN)\(0x([0-9a-fA-F]+),\s*(?:"([^"]+)",\s*)?(\w+),\s*"([^"]*)",\s*(true|false)\)')


def _entries(lines):
    out = []
    for line in lines:
        m = _BRACE.match(line)
        if m:
            out.append((int(m[1], 16), m[2], m[3], m[4], m[5]))
            continue
        m = _MACRO.match(line)
        if m:
            out.append((int(m[2], 16), m[3] or m[4], m[4], m[5], m[6]))
    return out


def x21_entries(src):
    result = {}
    for tu, (rel, array) in X21_TABLES.items():
        text = open(os.path.join(src, rel), encoding='utf-8').read()
        start = re.search(r'static const (?:xd3d::)?Hle %s\[\] = \{' % array, text)
        assert start, (rel, array)
        body = text[start.end():text.index('\n};', start.end())]
        lines = []
        for line in body.splitlines():
            inc = re.match(r'\s*#include "([^"]+)"', line)
            if inc:
                lines += open(os.path.join(src, inc[1]), encoding='utf-8').read().splitlines()
            else:
                lines.append(line)
        result[tu] = _entries(lines)
    return result


def _load(path):
    d = open(path, 'rb').read()
    base = struct.unpack_from('<I', d, 0x104)[0]
    n, hdr = struct.unpack_from('<II', d, 0x11C)
    secs = []
    for i in range(n):
        fl, va, vs, ra, rs, nm = struct.unpack_from('<6I', d, hdr - base + i * 56)
        secs.append((d[nm - base: d.index(b'\0', nm - base)].decode(), va, rs, ra))
    return d, secs


def _insns(xbe, a, size):
    d, secs = xbe
    for name, va, rs, ra in secs:
        if va <= a < va + rs:
            return list(_MD.disasm(d[ra + a - va: ra + a - va + size], a))
    return []


def _shape(ins):
    out = []
    for i in ins:
        op = _NUM.sub('A', i.op_str)
        if i.mnemonic in ('call', 'jmp') or i.mnemonic.startswith('j'):
            op = 'T'
        out.append(i.mnemonic + ' ' + op)
    return tuple(out)


class CodeMap:
    def __init__(self, xbe21, csv21, xbe2, csv2, sections=('D3D', 'DSOUND', 'XPP', 'XGRPH', 'D3DX', '.text')):
        self.x21, self.x2 = _load(xbe21), _load(xbe2)
        f21 = [r for r in csv.DictReader(open(csv21, encoding='utf-8')) if r['section'] in sections]
        f2 = [r for r in csv.DictReader(open(csv2, encoding='utf-8')) if r['section'] in sections]
        self.funcs21 = sorted((int(r['address'], 16), int(r['size']), r['section']) for r in f21)
        ins21, ins2 = {}, {}
        by21, by2 = collections.defaultdict(list), collections.defaultdict(list)
        for a, size, sec in self.funcs21:
            ins21[a] = _insns(self.x21, a, size)
            by21[(sec, _shape(ins21[a]))].append(a)
        for r in f2:
            a = int(r['address'], 16)
            ins2[a] = _insns(self.x2, a, int(r['size']))
            by2[(r['section'], _shape(ins2[a]))].append(a)
        self.pairs = {}
        for key, a21 in by21.items():
            a2 = by2.get(key)
            if a2 and len(a21) == 1 and len(a2) == 1 and len(key[1]) >= 2:
                self.pairs[a21[0]] = a2[0]
        votes = collections.defaultdict(collections.Counter)
        for a, b in self.pairs.items():
            for i, j in zip(ins21[a], ins2[b]):
                for x, y in zip(_NUM.findall(i.op_str), _NUM.findall(j.op_str)):
                    votes[int(x, 16)][int(y, 16)] += 1
        self.votes = votes
        self._starts = [f[0] for f in self.funcs21]

    def func(self, a):
        """The other build's address for x2.1 address a (inside a paired function), or None."""
        import bisect
        i = bisect.bisect_right(self._starts, a) - 1
        if i < 0:
            return None
        start, size, sec = self.funcs21[i]
        if not start <= a < start + max(size, 1) or start not in self.pairs:
            return None
        return self.pairs[start] + (a - start)

    def data(self, a):
        """(address, votes for it, all votes) for x2.1 data address a, or None when no paired code refers to it."""
        if a not in self.votes:
            return None
        y, n = self.votes[a].most_common(1)[0]
        return y, n, sum(self.votes[a].values())
