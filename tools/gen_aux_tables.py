#!/usr/bin/env python3
"""Generate the loader tables for Ninja Gaiden Black's bundled programs (NinjaTomoe / NinjaTrilogy / NinjaFCJ / NinjaFCU).

These XBEs use the plain (non-LTCG) D3D8 and DSOUND libraries, so every HLE implementation written for BLACK or for
Ninja Gaiden Black applies to the same-named function; only the address and the parameter sources change. Inputs are
the port project's work/functions_<xbe>.csv (route A's xboxrecomp function boundaries + XbSymbolDatabase names) and
work/symbols_<xbe>.json (XbSymbolDatabase, with conventions). Outputs, in loader/src/auxtab/ ("aux" is a reserved device name on Windows):
  functions.inc   every function of the D3D, DSOUND and XPP sections per XBE (replaced or stop-on-first-call)
  layout.inc      the D3D data addresses per XBE (0 = the loader allocates it)
  <tu>_tables.inc the HLE entries per translation unit and XBE, plus a selector function
Output holds addresses, sizes, names and conventions only, no game code.

Usage: python -I tools/gen_aux_tables.py <port project work dir> loader/src
"""
import csv
import json
import struct

import capstone
import os
import re
import sys

XBES = ['NinjaTomoe', 'NinjaTrilogy', 'NinjaFCJ', 'NinjaFCU', 'NG04TU', 'NG04DISC', 'NG04DL', 'NG04UP']
# where each XBE is, relative to the work dir (default: ../game/x21/<name>.xbe). NG04TU: Ninja Gaiden (2004) title
# update 1.06.04 (TDATA/54430003/$u/default.xbe), the main program the Hurricane Packs need; NG04DISC: its disc program
# 1.02.04, run when the update is not installed; NG04DL: the disc's downloader.xbe (the XDK content downloader the
# Xbox Live "NEW NOTICE!" starts); NG04UP: the disc's update.xbe (the XDK's online updater, which fetches the title update
# when Xbox Live asks for it).
XBE_PATHS = {'NG04TU': os.path.join('ng04', 'tu_default.xbe'), 'NG04DISC': os.path.join('ng04', 'disc_default.xbe'),
             'NG04DL': os.path.join('ng04', 'downloader.xbe'), 'NG04UP': os.path.join('ng04', 'update.xbe')}
# Builds of x2.1's own libraries (same code at other addresses): their entries and D3D data layout are x2.1's, carried
# over by code matching (tools/x21_map.py), so the register conventions verified on x2.1 hold.
FROM_X21 = {'NG04TU', 'NG04DISC'}


def xbe_file(work, x):
    return os.path.join(work, XBE_PATHS[x]) if x in XBE_PATHS else os.path.join(work, '..', 'game', 'x21', x + '.xbe')
SECTIONS = ['D3D', 'DSOUND', 'XPP']
# library sections also replaced in one build: the downloader's XACT engine (its sound; the program's few calls into it
# are answered "done", so XACT's own use of the DirectSound streams never runs)
EXTRA_SECTIONS = {'NG04DL': ['XACTENG']}


# library functions that are plain CPU code (or call XAPI in .text), left to run as they are: the downloader's XPR
# loading (D3D_AllocContiguousMemory / D3D_FreeContiguousMemory go to XPhysicalAlloc / Free, Register and GetType only
# read and write the resource header) and the C runtime's vector constructor iterator and an XPP state read that the
# linker put in XPP (called from XNET / XONLINE)
NATIVE_FUNCS = {'NG04DL': [0x000404D0, 0x00040440, 0x0004C620, 0x0004C660, 0x000AEC00, 0x000B0853],
                # the updater: the same XPR loading and helpers, SetIndices (the index data and base vertex into the
                # device, read by the inline DrawIndexedPrimitive) and CreateIndexBuffer2 (an XMemAlloc and a header)
                'NG04UP': [0x00042290, 0x00042200, 0x000418C0, 0x00041900, 0x0003F320, 0x00041920, 0x0003E270,
                           0x0008C9FB]}


def sections_of(x):
    return SECTIONS + EXTRA_SECTIONS.get(x, [])
# translation unit -> (source file, selector function)
TUS = {
    'device': ('d3d/device.cpp', 'hle_device_table_aux'),
    'draw': ('d3d/draw.cpp', 'hle_draw_table_aux'),
    'resources': ('d3d/resources.cpp', 'hle_resource_table_aux'),
    'dsound': ('audio/dsound.cpp', 'hle_dsound_table_aux'),
    'input': ('input/xinput.cpp', 'hle_input_table_aux'),
    'ngbextra': ('ngb/d3d_ngb.cpp', 'hle_ngb_extra_table_aux'),
}
ENTRY = re.compile(r'^\s*(H|HN)\((0x[0-9a-fA-F]+),\s*(?:"([^"]+)",\s*)?(\w+),\s*"([^"]*)",\s*(true|false)\)')


def base_name(n):
    n = re.sub(r'__LTCG.*$', '', n)
    n = re.sub(r'_\d+$', '', n)
    return n


def lookup(by_name, api):
    """The symbol for an entry's name: as written, or with the prefix XbSymbolDatabase gives it."""
    for cand in (api, 'D3DDevice_' + api, 'D3D_' + api, 'Direct3D_' + api, 'IDirectSound_' + api):
        s = by_name.get(cand) or by_name.get(base_name(cand))
        if s:
            return s
    return None


def sources(sym):
    if sym['cc'] == 'stdcall':  # plain stdcall: every parameter on the stack (XbSymbolDatabase may still say eax)
        return ' '.join('s s' if q['where'] == 'psh2' else 's' for q in sym['params'])
    out = []
    for q in sym['params']:
        w = q['where']
        out.append('s' if w == 'psh' else 's s' if w == 'psh2' else w)
    return ' '.join(out)


def jump_thunks(xbe_path, rows):
    """{thunk address: target} for functions that are a single `jmp rel32` (XAPI's `jmp XInitDevices` entry)."""
    d = open(xbe_path, 'rb').read()
    base = struct.unpack_from('<I', d, 0x104)[0]
    n = struct.unpack_from('<I', d, 0x11C)[0]
    sh = struct.unpack_from('<I', d, 0x120)[0] - base
    secs = [struct.unpack_from('<6I', d, sh + i * 0x38) for i in range(n)]
    out = {}
    for r in rows:
        a = int(r['address'], 16)
        for fl, va, vs, ra, rs, na in secs:
            if va <= a < va + rs and d[ra + a - va] == 0xE9:
                out[a] = (a + 5 + struct.unpack_from('<i', d, ra + a - va + 1)[0]) & 0xFFFFFFFF
    return out


def trivial_functions(xbe_path, rows):
    """Library functions that only set registers and return (e.g. `mov eax, 1; ret 4`): no memory, no hardware, so the
    original code runs as is (an Hle entry without an implementation)."""
    d = open(xbe_path, 'rb').read()
    base = struct.unpack_from('<I', d, 0x104)[0]
    n = struct.unpack_from('<I', d, 0x11C)[0]
    sh = struct.unpack_from('<I', d, 0x120)[0] - base
    secs = [struct.unpack_from('<6I', d, sh + i * 0x38) for i in range(n)]
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    out = []
    for r in rows:
        a, size = int(r['address'], 16), int(r['size'])
        if size > 16:
            continue
        blob = None
        for fl, va, vs, ra, rs, na in secs:
            if va <= a < va + rs:
                blob = d[ra + a - va: ra + a - va + size]
        if not blob:
            continue
        ok = False
        for ins in md.disasm(blob, a):
            if '[' in ins.op_str:
                break
            if ins.mnemonic in ('ret', 'retn'):
                ok = True
                break
            if ins.mnemonic not in ('mov', 'xor', 'or', 'nop'):
                break
        if ok:
            out.append(r)
    return out


def main():
    work, src = sys.argv[1], sys.argv[2]
    out_dir = os.path.join(src, 'auxtab')
    os.makedirs(out_dir, exist_ok=True)
    funcs, syms = {}, {}
    for x in XBES:
        funcs[x] = list(csv.DictReader(open(os.path.join(work, 'functions_%s.csv' % x), encoding='utf-8')))
        syms[x] = json.load(open(os.path.join(work, 'symbols_%s.json' % x), encoding='utf-8'))

    with open(os.path.join(out_dir, 'functions.inc'), 'w', newline='\n') as f:
        f.write('// Generated by tools/gen_aux_tables.py from work/functions_<xbe>.csv. Do not edit.\n')
        f.write('// Functions in the D3D, DSOUND and XPP sections of each bundled XBE: address, size in bytes, name.\n')
        for x in XBES:
            rows = sorted((r for r in funcs[x] if r['section'] in sections_of(x)), key=lambda r: int(r['address'], 16))
            seen = set()
            f.write('static const LibFunc kAux%s[] = {\n' % x)
            for r in rows:
                a = int(r['address'], 16)
                if a in seen:
                    continue
                seen.add(a)
                f.write('    {0x%08xu, %su, "%s"},\n' % (a, r['size'], r['name']))
            f.write('};\n')
        f.write('static const AuxFuncs kAuxFuncs[] = {\n')
        for x in XBES:
            f.write('    {"%s", kAux%s, sizeof kAux%s / sizeof kAux%s[0]},\n' % (x, x, x, x))
        f.write('};\n')

    code_maps, x21 = {}, None
    if FROM_X21 & set(XBES):
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # run with -I: the tools folder is not on the path
        import x21_map
        x21 = x21_map.x21_entries(src)
        for x in FROM_X21 & set(XBES):
            code_maps[x] = x21_map.CodeMap(os.path.join(work, '..', 'game', 'x21', 'default.xbe'),
                                           os.path.join(work, 'functions_x21.csv'), xbe_file(work, x),
                                           os.path.join(work, 'functions_%s.csv' % x))
        x21_by_name = {}
        for q in json.load(open(os.path.join(work, 'symbols_x21.json'), encoding='utf-8')):
            if q['kind'] == 'FUN':
                x21_by_name.setdefault(q['name'], q)
                x21_by_name.setdefault(base_name(q['name']), q)
        ngb_layout = [int(v, 16) for v in re.search(r'kLayoutNgb = \{([^}]*)\}', open(
            os.path.join(src, 'd3d', 'xd3d.h'), encoding='utf-8').read())[1].replace(' ', '').split(',')]

    with open(os.path.join(out_dir, 'layout.inc'), 'w', newline='\n') as f:
        f.write('// Generated by tools/gen_aux_tables.py from XbSymbolDatabase variables. Do not edit.\n')
        f.write('// {xbe, {g_pdevice, device_object, push_segment, render_state, texture_state, streams, dirty_flags, vs_constants}},\n')
        f.write('// 0 = not a fixed address in this build: the loader allocates it.\n')
        for x in XBES:
            v = {s['name']: s['addr'] for s in syms[x] if s['kind'] == 'VAR'}
            lay = [v.get('D3D_g_pDevice', 0), 0, 0, v.get('D3D_g_RenderState', 0), v.get('D3D_g_DeferredTextureState', 0),
                   v.get('D3D_g_Stream', 0), 0, 0]
            if x in code_maps:  # x2.1's fixed layout at the addresses its paired code uses here (unanimous votes only)
                lay = []
                for a in ngb_layout:
                    m = code_maps[x].data(a)
                    assert m and m[1] == m[2], (x, hex(a), m)
                    lay.append(m[0])
            f.write('{"%s", {%s}},\n' % (x, ', '.join('0x%08Xu' % a for a in lay)))

    report = []
    for tu, (rel, selector) in TUS.items():
        entries = [m.groups() for m in map(ENTRY.match, open(os.path.join(src, rel), encoding='utf-8').read().splitlines()) if m]
        name = tu.capitalize()
        with open(os.path.join(out_dir, tu + '_tables.inc'), 'w', newline='\n') as f:
            f.write('// Generated by tools/gen_aux_tables.py from the H() entries in %s and XbSymbolDatabase. Do not edit.\n' % rel)
            f.write('// {address, name, implementation, parameter sources, returns a value}\n')
            for x in XBES:
                by_name = {}
                for s in syms[x]:
                    if s['kind'] == 'FUN':
                        by_name.setdefault(s['name'], s)
                        by_name.setdefault(base_name(s['name']), s)
                done = set()
                out = []
                if x in code_maps:
                    for a21, api, impl, params, ret in x21[tu]:
                        a = code_maps[x].func(a21)
                        how = 'x2.1 0x%08x' % a21
                        if a is None:  # no unique code match: the same name, if XbSymbolDatabase's parameters agree with x2.1's
                            s, s21 = lookup(by_name, api), lookup(x21_by_name, api)
                            same_params = s and s21 and [q['name'] for q in s['params']] == [q['name'] for q in s21['params']]
                            if s and sources(s) == params:
                                a, how = s['addr'], 'by name; x2.1 0x%08x' % a21
                            elif same_params and sources(s21) == params:  # same parameters, other registers (LTCG)
                                a, how, params = s['addr'], 'by name, its registers; x2.1 0x%08x "%s"' % (a21, params), sources(s)
                            else:
                                report.append('  %s %s: %s (x2.1 0x%08x, "%s") has no match here%s' % (
                                    x, tu, api, a21, params, ': named %s "%s"' % (s['name'], sources(s)) if s else ''))
                                continue
                        if a in done:
                            continue
                        done.add(a)
                        out.append('    {0x%08xu, "%s", (void *)&%s, "%s", %s}, // %s' % (a, api, impl, params, ret, how))
                for kind, addr, api, impl, params, ret in ([] if x in code_maps else entries):
                    api = api or impl
                    s = None
                    for cand in (api, 'D3DDevice_' + api, 'D3D_' + api, 'Direct3D_' + api, 'IDirectSound_' + api,
                                 api[4:] if api.startswith('D3D_') else api):  # XbSymbolDatabase: Get2DSurfaceDesc
                        s = by_name.get(cand) or by_name.get(base_name(cand))
                        if s:
                            break
                    if not s or s['addr'] in done:
                        continue
                    p = sources(s)
                    if len(p.split()) != len(params.split()):
                        report.append('  %s %s: %s at 0x%08x has %d parameters, the implementation takes %d' %
                                      (x, tu, s['name'], s['addr'], len(p.split()), len(params.split())))
                        continue
                    done.add(s['addr'])
                    out.append('    {0x%08xu, "%s", (void *)&%s, "%s", %s},' % (s['addr'], s['name'], impl, p, ret))
                xbe_path = xbe_file(work, x)
                by_addr = {int(line.split(',')[0].strip().split('{')[1][2:-1], 16): line for line in out}
                for a, target in jump_thunks(xbe_path, [r for r in funcs[x] if r['section'] in sections_of(x)]).items():
                    if a not in done and target in by_addr:
                        done.add(a)
                        line = by_addr[target].replace('0x%08xu' % target, '0x%08xu' % a, 1)
                        out.append(line + ' // jmp thunk to 0x%08x' % target)
                if tu == 'device':  # trivial register-only functions run natively (listed once, in the device table)
                    rows = [r for r in funcs[x] if r['section'] in sections_of(x)]
                    for a in NATIVE_FUNCS.get(x, []):
                        if a not in done:
                            done.add(a)
                            name_a = next((r['name'] for r in rows if int(r['address'], 16) == a), 'FUN_%08x' % a)
                            out.append('    {0x%08xu, "%s", nullptr, "", false}, // plain code: runs natively' % (a, name_a))
                    for r in trivial_functions(xbe_file(work, x), rows):
                        a = int(r['address'], 16)
                        if a not in done:
                            done.add(a)
                            out.append('    {0x%08xu, "%s", nullptr, "", false}, // only sets registers and returns: runs natively' % (a, r['name']))
                f.write('static const Hle kAux%s%s[] = {\n%s\n    {0, nullptr, nullptr, "", false},\n};\n' % (name, x, '\n'.join(out)))
                report.append('%s %s: %d entries' % (x, tu, len(out)))
            f.write('const Hle *%s(const char *xbe, size_t *count)\n{\n' % selector)
            for x in XBES:
                f.write('    if (!strcmp(xbe, "%s")) {\n        *count = sizeof kAux%s%s / sizeof kAux%s%s[0] - 1;\n        return kAux%s%s;\n    }\n'
                        % (x, name, x, name, x, name, x))
            f.write('    *count = 0;\n    return nullptr;\n}\n')
    print('\n'.join(report))


if __name__ == '__main__':
    main()
