#!/usr/bin/env python3
"""x2.1 代码普查（capstone 线性扫描可执行段，只读）。回答阶段 2 的几个问题：

  A. 库段调用普查：.text / HANZI（以及各库段之间）对 D3D、DSOUND、XPP、XONLINE、XNET、XGRPH、D3DX、PSFD00、DOLBY 的直接 call/jmp 目标，按目标汇总次数；
  B. 内核导入普查：对 thunk 表 0x3E1420 的每个槽位的引用（call/jmp/mov [slot]），按调用者所在段汇总；
  C. 特权 / 特殊指令：cli sti wbinvd invd in out rdtsc cpuid hlt int iretd lgdt lidt + mov cr/dr 的位置与所在函数；
  D. fs: 段访问（带位移）的位置与所在函数；
  E. 对 XBE 头区域（0x10000–头长）的引用（判断 PE 头副本可放的位置）；
  F. .text / HANZI 对库段数据地址（不是函数入口）的内存引用，按地址汇总（LTCG 内联写库全局的证据）。

输入：game/x21/default.xbe、work/functions_x21.csv（函数边界 + 名字）。输出：work/census_x21.json + 终端摘要。
线性扫描会把代码里的数据误当指令；凡命中都带函数名，落在"无函数"处的要人工复核。

用法：<venv>/python.exe -I tools/xbe_census.py
"""
import bisect
import collections
import csv
import json
import os
import struct

from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone.x86 import X86_OP_MEM, X86_OP_IMM, X86_REG_FS, X86_REG_CR0, X86_REG_DR7

ROOT = r"E:\games\emu\ninja gaiden black Hurricane recomp2"
XBE = os.path.join(ROOT, r"game\x21\default.xbe")
FUNCS = os.path.join(ROOT, r"work\functions_x21.csv")
OUT = os.path.join(ROOT, r"work\census_x21.json")
ROUTE_A_FUNCS = r"E:\games\emu\ninja gaiden black hurricane recomp\build\lift_x21\analysis\disasm\functions.json"

LIB_SECTIONS = ['D3DX', 'DSOUND', 'XONLINE', 'XNET', 'PSFD00', 'XGRPH', 'D3D', 'XPP', 'DOLBY', 'XON_RD']
SPECIAL = {'cli', 'sti', 'wbinvd', 'invd', 'in', 'out', 'insb', 'insd', 'outsb', 'outsd', 'rdtsc', 'cpuid', 'hlt', 'int',
           'int3', 'iretd', 'lgdt', 'lidt', 'sgdt', 'sidt', 'rdmsr', 'wrmsr', 'invlpg', 'ltr', 'lldt', 'clts', 'ud2'}


def load_xbe():
    d = open(XBE, 'rb').read()
    base = struct.unpack_from('<I', d, 0x104)[0]
    hdr_size = struct.unpack_from('<I', d, 0x108)[0]
    count = struct.unpack_from('<I', d, 0x11C)[0]
    hp = struct.unpack_from('<I', d, 0x120)[0] - base
    thunk = struct.unpack_from('<I', d, 0x158)[0] ^ 0x5B6D40B6
    secs = []
    for i in range(count):
        o = hp + i * 0x38
        flags, va, vs, raw, rs, namep = struct.unpack_from('<IIIIII', d, o)
        name = d[namep - base:d.index(b'\0', namep - base)].decode()
        secs.append(dict(name=name, flags=flags, va=va, vsize=vs, raw=raw, rawsize=rs))
    # kernel import ordinals from the thunk table (in .rdata raw)
    ordinals = []
    for s in secs:
        if s['va'] <= thunk < s['va'] + s['vsize']:
            off = s['raw'] + (thunk - s['va'])
            while True:
                v = struct.unpack_from('<I', d, off)[0]
                if v == 0:
                    break
                ordinals.append(v & 0x7FFFFFFF)
                off += 4
    return d, base, hdr_size, secs, thunk, ordinals


def main():
    d, base, hdr_size, secs, thunk, ordinals = load_xbe()
    kexports = json.load(open(os.path.join(ROOT, r"ngb-recomp-native\tools\kernel_exports.json")))
    kname = {}
    if isinstance(kexports, dict):
        for k, v in kexports.items():
            try:
                kname[int(k)] = v if isinstance(v, str) else v.get('name', str(v))
            except ValueError:
                pass
    elif isinstance(kexports, list):
        for e in kexports:
            if isinstance(e, dict) and 'ordinal' in e:
                kname[int(e['ordinal'])] = e.get('name', '')
    slot_of = {thunk + 4 * i: o for i, o in enumerate(ordinals)}

    funcs = []
    for r in csv.DictReader(open(FUNCS, encoding='utf-8')):
        funcs.append((int(r['address'], 16), int(r['size']), r['name'], r['section']))
    funcs.sort()
    starts = [f[0] for f in funcs]

    def func_at(a):
        i = bisect.bisect_right(starts, a) - 1
        if i >= 0 and funcs[i][0] <= a < funcs[i][0] + funcs[i][1]:
            return funcs[i]
        return None

    def sec_of(a):
        for s in secs:
            if s['va'] <= a < s['va'] + s['vsize']:
                return s['name']
        return None

    func_starts = set(starts)
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True

    lib_calls = collections.defaultdict(lambda: collections.Counter())  # target -> Counter(caller section)
    kern_refs = collections.defaultdict(lambda: collections.Counter())  # ordinal -> Counter(caller section)
    special = []
    fs_sites = []
    hdr_refs = []
    lib_data_refs = collections.defaultdict(lambda: collections.Counter())  # (lib, addr) -> Counter(caller section)
    hdr_end = base + hdr_size

    # Disassemble function by function (each from its own start, so the decoder stays in sync); bytes that belong to no
    # detected function are skipped and only counted, because a raw linear sweep of them reads data as instructions.
    sec_by_name = {s['name']: s for s in secs}
    gap_bytes = collections.Counter()
    for s in secs:
        if s['flags'] & 4:
            covered = sum(f[1] for f in funcs if f[3] == s['name'])
            gap_bytes[s['name']] = s['rawsize'] - covered
    stream = []
    for f in funcs:
        s = sec_by_name.get(f[3])
        if not s or not s['flags'] & 4:
            continue
        start, size = f[0], f[1]
        raw = s['raw'] + (start - s['va'])
        end_raw = min(raw + size, s['raw'] + s['rawsize'])
        if raw < s['raw'] or end_raw <= raw:
            continue
        stream.append((s, f, d[raw:end_raw], start))
    # Recursive descent inside each function: follow fall-through, direct branches and switch jump tables whose entries
    # point inside the function; stop at ret / indirect jmp. MSVC puts jump tables in .text right after the code, so a
    # linear decode read them as insb / outsd / hlt and the like.
    def decode_function(s, f, code, va):
        end = va + len(code)
        seen = set()
        work = [va]
        out = []
        while work:
            a = work.pop()
            while va <= a < end and a not in seen:
                seen.add(a)
                try:
                    ins = next(md.disasm(code[a - va:a - va + 16], a, count=1))
                except StopIteration:
                    break
                out.append(ins)
                m = ins.mnemonic
                ops = ins.operands
                if m == 'ret' or m == 'retn' or m == 'iretd' or m == 'hlt':
                    break
                if m.startswith('j') or m == 'call' or m.startswith('loop'):
                    if ops and ops[0].type == X86_OP_IMM:
                        t = ops[0].imm
                        if va <= t < end and m != 'call':
                            work.append(t)
                    elif m == 'jmp' and ops and ops[0].type == X86_OP_MEM and ops[0].mem.base == 0 and ops[0].mem.scale == 4:
                        tbl = ops[0].mem.disp & 0xFFFFFFFF
                        if s['va'] <= tbl < s['va'] + s['rawsize']:
                            o = s['raw'] + (tbl - s['va'])
                            while o + 4 <= len(d):
                                t = struct.unpack_from('<I', d, o)[0]
                                if not (va <= t < end):
                                    break
                                work.append(t)
                                o += 4
                    if m == 'jmp':
                        break
                a = ins.address + ins.size
        return out

    for s, f, code, va in stream:
        for ins in decode_function(s, f, code, va):
            here = ins.address
            fname = f[2]
            if ins.mnemonic in SPECIAL or ins.op_str.startswith(('cr', 'dr')) or ', cr' in ins.op_str or ', dr' in ins.op_str:
                special.append(dict(addr='0x%08x' % here, section=s['name'], insn=ins.mnemonic + ' ' + ins.op_str, func=fname))
            for op in ins.operands:
                if op.type == X86_OP_IMM and ins.mnemonic in ('call', 'jmp'):
                    t = op.imm
                    ts = sec_of(t)
                    if ts in LIB_SECTIONS and ts != s['name']:
                        lib_calls[(ts, t)][s['name']] += 1
                if op.type == X86_OP_MEM:
                    if op.mem.segment == X86_REG_FS and op.mem.base == 0 and op.mem.index == 0 and op.mem.disp != 0:
                        fs_sites.append(dict(addr='0x%08x' % here, section=s['name'], disp='0x%x' % op.mem.disp,
                                             insn=ins.mnemonic + ' ' + ins.op_str, func=fname))
                    if op.mem.base == 0 and op.mem.index == 0 and op.mem.segment != X86_REG_FS:
                        a = op.mem.disp & 0xFFFFFFFF
                        if a in slot_of:
                            kern_refs[slot_of[a]][s['name']] += 1
                        elif base <= a < hdr_end:
                            hdr_refs.append(dict(addr='0x%08x' % here, section=s['name'], target='0x%08x' % a, insn=ins.mnemonic + ' ' + ins.op_str, func=fname))
                        else:
                            ts = sec_of(a)
                            if ts in LIB_SECTIONS and s['name'] in ('.text', 'HANZI') and a not in func_starts:
                                lib_data_refs[(ts, a)][s['name']] += 1
                elif op.type == X86_OP_IMM and ins.mnemonic not in ('call', 'jmp') and not ins.mnemonic.startswith('j'):
                    a = op.imm & 0xFFFFFFFF
                    if base <= a < hdr_end and s['name'] in ('.text', 'HANZI', 'XPP', 'XONLINE', 'XNET'):
                        hdr_refs.append(dict(addr='0x%08x' % here, section=s['name'], target='0x%08x' % a, insn=ins.mnemonic + ' ' + ins.op_str, func=fname))

    name_of = {f[0]: f[2] for f in funcs}
    out = dict(
        lib_calls=[dict(section=k[0], target='0x%08x' % k[1], name=name_of.get(k[1], 'FUN_%08x (not a function start)' % k[1]),
                        callers=dict(v), total=sum(v.values())) for k, v in sorted(lib_calls.items())],
        kernel_refs=[dict(ordinal=o, name=kname.get(o, '?'), callers=dict(v)) for o, v in sorted(kern_refs.items())],
        kernel_unreferenced=[dict(ordinal=o, name=kname.get(o, '?')) for o in ordinals if o not in kern_refs],
        special=special, fs=fs_sites, header_refs=hdr_refs,
        lib_data_refs=[dict(section=k[0], addr='0x%08x' % k[1], callers=dict(v), total=sum(v.values())) for k, v in sorted(lib_data_refs.items())],
    )
    json.dump(out, open(OUT, 'w', encoding='utf-8'), indent=1)

    print('== A. direct call/jmp targets into library sections (by caller section)')
    per = collections.defaultdict(lambda: collections.Counter())
    for e in out['lib_calls']:
        for cs, n in e['callers'].items():
            per[e['section']][cs] += 1
    for sec in LIB_SECTIONS:
        if per[sec]:
            print('  %-8s distinct targets called from: %s' % (sec, dict(per[sec])))
    unnamed = [e for e in out['lib_calls'] if '.text' in e['callers'] and e['name'].startswith('FUN_')]
    print('  targets called from .text without a name: %d' % len(unnamed))
    for e in unnamed[:30]:
        print('    %s %s %s' % (e['section'], e['target'], e['callers']))
    print('== B. kernel imports: %d referenced, %d never referenced' % (len(out['kernel_refs']), len(out['kernel_unreferenced'])))
    print('  unreferenced:', ' '.join('%d:%s' % (e['ordinal'], e['name']) for e in out['kernel_unreferenced']))
    print('== bytes of executable sections outside detected functions (skipped; negative = overlapping aliases):', dict(gap_bytes))
    # Route A's detector also emits low-confidence "functions" (gap_prologue, imm_ref_target, cc_boundary, tail aliases)
    # that are often data; a special instruction only counts when it sits in a called or prologue-detected function
    # or a function named by XbSymbolDatabase.
    route_a = json.load(open(ROUTE_A_FUNCS))
    method = {int(x['start'], 16): (x['detection_method'], len(x.get('called_by', []))) for x in route_a}
    trusted_methods = {'call_target', 'prologue', 'entry_point', 'indirect_call_slot', 'tail_jump_target'}

    def trusted(fname):
        a = next((f[0] for f in funcs if f[2] == fname), None)
        if a is None:
            return False
        if not fname.startswith('FUN_'):
            return True
        m = method.get(a)
        return bool(m) and (m[0] in trusted_methods or m[1] > 0)

    real = [e for e in special if e['insn'].split()[0] not in ('int3', 'ud2')]
    print('== C. special instructions (excluding int3/ud2): %d, in trusted functions: %d' % (len(real), sum(1 for e in real if trusted(e['func']))))
    for e in real:
        if trusted(e['func']):
            print('  %s %-8s %-22s %s  (%s)' % (e['addr'], e['section'], e['insn'], e['func'], method.get(next(f[0] for f in funcs if f[2] == e['func']), ('named',))[0]))
    print('  untrusted hits (likely data): %s' % dict(collections.Counter(e['section'] for e in real if not trusted(e['func']))))
    print('== D. fs sites: %d' % len(fs_sites))
    print('  by disp:', dict(collections.Counter(e['disp'] for e in fs_sites if e['section'] in ('.text', 'HANZI') or e['section'] in LIB_SECTIONS)))
    print('== E. references to the XBE header range: %d' % len(hdr_refs))
    for e in hdr_refs[:40]:
        print('  %s %-8s -> %s  %-32s %s' % (e['addr'], e['section'], e['target'], e['insn'], e['func']))
    print('== F. .text/HANZI memory references to library-section data (non-function addresses): %d distinct' % len(out['lib_data_refs']))
    top = sorted(out['lib_data_refs'], key=lambda e: -e['total'])[:40]
    for e in top:
        print('  %-7s %s %s' % (e['section'], e['addr'], e['callers']))
    print('written', OUT)


if __name__ == '__main__':
    main()
