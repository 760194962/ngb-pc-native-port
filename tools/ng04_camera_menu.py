"""Ninja Gaiden (2004) title update 1.06.04: the Hurricane Packs' third-person camera as options of the game's own
Controller Settings page, as a patch of the update's default.xbe (TDATA\\54430003\\$u\\default.xbe), so it works on the
console (an emulator or a console that runs unsigned programs) as well as in the PC port.

    python tools/ng04_camera_menu.py <1.06 default.xbe> <out.xbe>      (needs keystone-engine: pip install keystone-engine)

What the patch does (the reasons: loader/src/ng04_camera.cpp and project notes, 2026-11-12):
- Controller Settings gets two rows, after "Controls While in Water":
    3rd Person Camera         On at Start / Off at Start    options +0x14 (0 is On: new saves start with it on)
    3rd Person Camera Stages  Hurricane Packs / All Stages  options +0x15
  and "1st Person Camera Control" becomes "Camera Control" (Normal / Reverse, options +0x0B), now for both cameras as
  Ninja Gaiden Black x2.1 has it: Normal turns the right stick's third-person camera the other way round on both
  axes, Reverse also turns the first-person view's left / right (the game already turns its up / down).
  Labels are English in both languages, as the game's own; the help line has English and Japanese text.
- The options are bytes of the game's settings (menu copy 0xE7C3F0, saved copy 0xE8ED30; +0x14 / +0x15 are unused
  by the game). A change writes both copies.
- 3rd Person Camera: the camera's on / off byte (0xE8F05C, R3 flips it) is set from the option where the game sets
  it to 0 (boot 0x2D8AA8, each stage's player reset 0x162C6A).
- Stages: the six tests of the stage's mode (0x5681D8 > 1: the Hurricane Packs' stages) in the camera code take the
  Hurricane side when the option is All Stages.
- The new code and data are a section of their own (NGCAM, 0xE9D000); the section table gets one more entry (the
  header's data after it moved down 56 bytes, the pointers into it moved with it).
Every place patched is checked against its original bytes first.
"""
import hashlib
import struct
import sys

from keystone import KS_ARCH_X86, KS_MODE_32, Ks

TU_SHA256 = '86475ba502c0e7f147334b167041c3fd310f8fb34f9e77b0ee6e6866e0764ee3'

# game addresses (1.06.04)
MODE = 0x5681D8      # the stage's mode: > 1 in the Hurricane Packs' stages
CAMVAR = 0xE8F05C    # 1: the right stick moves the third-person camera (R3 flips it)
MENU = 0xE7C3F0      # the options as the menu edits them
SAVED = 0xE8ED30     # the options the game plays with
OPT_CTRL, OPT_START, OPT_ALL = 0x0B, 0x14, 0x15
LANG = 0x98ABCC      # the menu's text language (0 English, 1 Japanese)
HELP_PAGE, HELP_ROW = 0x98AB74, 0x98AB10
MSG_GROUPS, MSG_COUNTS = 0x419F2C, 0x419F4C
PAGE3 = 0x40F510     # Controller Settings' page descriptor
TOGGLE = 0x171A40    # the menu's value step (ecx value, esi min, eax max, edx direction, ebx [ebp+0x10])
SOUND = 0x1D31F0

SEC_VA, SEC_VSIZE = 0x00E9D000, 0x2000
ROW_Y, ROW_STEP = 0x68, 35  # Controller Settings' rows: 8 rows above the help line (the game's: 0x70, 40 for 6)

# new messages (group 2): id -> (English, Japanese)
MSGS = {
    0x20100: ('3rd Person Camera', '3rd Person Camera'),
    0x20101: ('3rd Person Camera Stages', '3rd Person Camera Stages'),
    0x20102: ('Camera Control', 'Camera Control'),
    0x20105: ('Sets the control direction of the 1st and 3rd person cameras.',
              '主観カメラと右スティックのカメラ操作の方向を設定します'),
    0x20106: ('Sets whether the right stick moves the camera from the start.',
              'ゲーム開始時から右スティックでカメラを操作するか設定します'),
    0x20107: ('Sets the stages where the right stick moves the camera.',
              '右スティックでカメラを操作できるステージを設定します'),
    0x20108: ('Off at Start', 'Off at Start'),
    0x20109: ('On at Start', 'On at Start'),
    0x2010A: ('Hurricane', 'Hurricane'),
    0x2010B: ('All Stages', 'All Stages'),
}
LABELS = [0x2000C, 0x2000D, 0x20102, 0x2000F, 0x20100, 0x20101, 0x20004, 0x20005]
HELP = [0x40, 0x43, 0x105, 0x45, 0x106, 0x107, 0x3A, 0x3B]


class Xbe:
    def __init__(self, data):
        self.d = bytearray(data)
        self.base, = struct.unpack_from('<I', self.d, 0x104)

    def secs(self):
        n, addr = struct.unpack_from('<II', self.d, 0x11C)
        for i in range(n):
            o = addr - self.base + i * 56
            yield o, struct.unpack_from('<9I', self.d, o)

    def off(self, va):
        for _, (fl, v, vs, ra, rs, *_r) in self.secs():
            if v <= va < v + rs:
                return ra + va - v
        raise ValueError('0x%x not in a section' % va)

    def rd(self, va, n):
        o = self.off(va)
        return bytes(self.d[o:o + n])

    def wr(self, va, b):
        o = self.off(va)
        self.d[o:o + len(b)] = b

    def expect(self, va, b):
        got = self.rd(va, len(b))
        if got != bytes(b):
            raise SystemExit('0x%08x: expected %s, found %s' % (va, bytes(b).hex(' '), got.hex(' ')))


def main():
    src, out = sys.argv[1:3]
    data = open(src, 'rb').read()
    if hashlib.sha256(data).hexdigest() != TU_SHA256:
        raise SystemExit('not the 1.06.04 title update default.xbe')
    x = Xbe(data)
    ks = Ks(KS_ARCH_X86, KS_MODE_32)

    # --- data of the new section, laid out after the code (code first: its size from a first assembly) -------------
    # The original bytes of every place patched
    sites = {
        'label': (0x20DADB, '8b 84 bc a0 00 00 00'),
        'rows': (0x20DD0B, '83 ff 06'),
        'row_y': (0x20D8ED, 'bb 70 00 00 00'),
        'row_step': (0x20DD04, '03 1d 3c ee 41 00'),
        'value': (0x20DC63, '89 44 24 18 8b c7'),
        'help1': (0x210B8A, '66 8b 04 42 66 89 01'),
        'help2': (0x210BB2, '66 8b 04 42 66 89 01'),
        'input': (0x172F6B, '8a 47 0c 3c ff'),
        'defaults': (0x1730EA, '88 0d 00 c4 e7 00'),
        'boot': (0x2D8AA8, '88 1d 5c f0 e8 00 c6 05 fd e0 89 00 01 c6 05 fe e0 89 00 01 c6 05 ff e0 89 00 01'),
        'reset': (0x162C6A, '88 1d 5c f0 e8 00'),
        'stick': (0x45FE8, '8a 81 0a 04 00 00'),
        'fp': (0x4E17F, 'f3 0f 10 6c 24 2c'),
        'g1': (0x45F2C, '0f 86 1e 0f 00 00'),
        'g2': (0x4515B, '80 3d d8 81 56 00 01 0f 86 8d 02 00 00'),
        'g3': (0x45BC2, '80 3d d8 81 56 00 02 76 07'),
        'g4': (0x47490, 'a0 d8 81 56 00 3c 01'),
        'g5': (0x322C7, '38 1d d8 81 56 00'),
        'g6': (0x323AE, '38 1d d8 81 56 00'),
        'page3': (PAGE3, '03 00 05 01 01 01 00 02 00 03 00 00 b0 f0 40 00'),
        'reset_al': (0x162C55, 'b0 01 5e'),
        'g1_cmp': (0x45F20, '3c 01'),
        'g4_ja': (0x474AE, '77 03'),
        'g5_ja': (0x322D0, '77 5d'),
        'g6_ja': (0x323B4, '77 2e'),
    }
    for va, b in sites.values():
        x.expect(va, bytes.fromhex(b))

    rows_old = x.rd(0x40F0B0, 6 * 28)
    gtbl, = struct.unpack('<I', x.rd(MSG_GROUPS + 8, 4))
    gcnt, = struct.unpack('<H', x.rd(MSG_COUNTS + 4, 2))
    msgs_old = x.rd(gtbl, gcnt * 12)

    def asm(code, at):
        enc, _ = ks.asm(code, at)
        return bytes(enc)

    S = lambda o: '0x%x' % (SAVED + o)
    M = lambda o: '0x%x' % (MENU + o)

    def code_text(d):  # d: data addresses
        return f'''
        cave_val:                       ; Controller Settings' values: rows 4 and 5 are ours
            mov [esp+0x1c], eax
            cmp edi, 4
            je val4
            cmp edi, 5
            je val5
            mov eax, edi
            ret
        val4:
            add esp, 4
            movzx eax, byte ptr [{M(OPT_START)}]
            mov eax, [eax*4+{d['v4']:#x}]
            jmp valc
        val5:
            add esp, 4
            movzx eax, byte ptr [{M(OPT_ALL)}]
            mov eax, [eax*4+{d['v5']:#x}]
        valc:
            movzx edx, byte ptr [{LANG:#x}]
            push edx
            jmp 0x20DCB5

        cave_help:                      ; the help line: Controller Settings' ids from our table
            cmp word ptr [{HELP_PAGE:#x}], 2
            jne help_orig
            cmp eax, 8
            jae help_orig
            mov ax, [eax*2+{d['help']:#x}]
            mov [ecx], ax
            ret
        help_orig:
            mov ax, [edx+eax*2]
            mov [ecx], ax
            ret

        cave_in:                        ; left / right on a row: ours step their option
            mov al, [edi+0xc]
            cmp al, 0xff
            jne in_out
            cmp cl, 4
            je in4
            cmp cl, 5
            je in5
        in_out:
            cmp al, 0xff
            ret
        in4:
            push {OPT_START}
            jmp in_step
        in5:
            push {OPT_ALL}
        in_step:
            pushad
            mov ecx, [esp+32]
            push ecx
            movzx ecx, byte ptr [ecx+{MENU:#x}]
            xor esi, esi
            mov eax, 1
            movsx edx, byte ptr [edi+0x10]
            mov ebx, [ebp+0x10]
            push ecx
            call {TOGGLE:#x}
            add esp, 4
            pop ecx
            mov [ecx+{MENU:#x}], al
            mov [ecx+{SAVED:#x}], al
            mov al, [edi+0x10]
            test al, al
            je in_quiet
            push 0x40
            push 1
            call {SOUND:#x}
            add esp, 8
        in_quiet:
            popad
            add esp, 4
            xor ecx, ecx
            mov al, [edi+0xc]
            cmp al, 0xff
            ret

        cave_def:                       ; Default Settings: ours too
            mov [0xE7C400], cl
            mov byte ptr [{M(OPT_START)}], 0
            mov byte ptr [{M(OPT_ALL)}], 0
            mov byte ptr [{S(OPT_START)}], 0
            mov byte ptr [{S(OPT_ALL)}], 0
            ret

        cave_cam:                       ; where the game turns the camera off: the option's choice (0 is On)
            push eax
            mov al, [{S(OPT_START)}]
            xor al, 1
            mov [{CAMVAR:#x}], al
            pop eax
            ret

        cave_stick:                     ; third person: Normal turns both axes round (x2.1)
            cmp byte ptr [{S(OPT_CTRL)}], 0
            jne stick_out
            test dword ptr [esp+0x2c], 0x7fffffff
            je stick_h
            xor dword ptr [esp+0x2c], 0x80000000
        stick_h:
            test dword ptr [esp+0x30], 0x7fffffff
            je stick_out
            xor dword ptr [esp+0x30], 0x80000000
        stick_out:
            mov al, [ecx+0x40a]
            ret

        cave_fp:                        ; first person: Reverse also turns left / right (x2.1)
            cmp byte ptr [{S(OPT_CTRL)}], 0
            je fp_out
            test dword ptr [esp+0x30], 0x7fffffff
            je fp_out
            xor dword ptr [esp+0x30], 0x80000000
        fp_out:
            movss xmm5, dword ptr [esp+0x30]
            ret

        cave_g1:                        ; jbe 0x46E50 after cmp al, 1 (the stick camera)
            pushfd
            cmp byte ptr [{S(OPT_ALL)}], 0
            jne g1_go
            popfd
            ja g1_ret
            mov dword ptr [esp], 0x46E50
        g1_ret:
            ret
        g1_go:
            popfd
            ret

        cave_g2:                        ; cmp [mode], 1; jbe 0x453F5
            cmp byte ptr [{S(OPT_ALL)}], 0
            jne g2_ret
            cmp byte ptr [{MODE:#x}], 1
            ja g2_ret
            mov dword ptr [esp], 0x453F5
        g2_ret:
            ret

        cave_g3:                        ; cmp [mode], 2; jbe 0x45BD2
            cmp byte ptr [{S(OPT_ALL)}], 0
            jne g3_ret
            cmp byte ptr [{MODE:#x}], 2
            ja g3_ret
            mov dword ptr [esp], 0x45BD2
        g3_ret:
            ret

        cave_g4:                        ; mov al, [mode]; cmp al, 1 (its ja comes later)
            mov al, [{MODE:#x}]
            cmp byte ptr [{S(OPT_ALL)}], 0
            je g4_cmp
            mov al, 2
        g4_cmp:
            cmp al, 1
            ret

        cave_g56:                       ; cmp [mode], bl (bl = 1); ja
            cmp byte ptr [{S(OPT_ALL)}], 0
            jne g56_all
            cmp byte ptr [{MODE:#x}], bl
            ret
        g56_all:
            push eax
            mov al, 2
            cmp al, bl
            pop eax
            ret
        '''

    # data blob (addresses relative to its start, placed after the code)
    def data_blob(at):
        d, blob = {}, bytearray()

        def put(name, b, align=4):
            while (at + len(blob)) % align:
                blob.append(0)
            d[name] = at + len(blob)
            blob.extend(b)
        put('labels', struct.pack('<8I', *LABELS))
        put('step', struct.pack('<i', ROW_STEP))
        put('help', struct.pack('<8H', *HELP))
        put('v4', struct.pack('<2I', 0x20109, 0x20108))  # 0 On at Start (a new save's and Default Settings'), 1 Off
        put('v5', struct.pack('<2I', 0x2010A, 0x2010B))
        rows = bytearray(rows_old[:4 * 28])
        for rid in (4, 5):
            r = bytearray(rows_old[3 * 28:4 * 28])
            r[0] = rid
            rows += r
        for rid, src_row in ((6, 4), (7, 5)):
            r = bytearray(rows_old[src_row * 28:(src_row + 1) * 28])
            r[0] = rid
            rows += r
        put('rows', bytes(rows))
        strs = {}
        for mid, (en, ja) in MSGS.items():
            for k, s in (('en', en), ('ja', ja)):
                put('s%x%s' % (mid, k), s.encode('cp932') + b'\0', 1)
                strs[(mid, k)] = d['s%x%s' % (mid, k)]
        table = bytearray(msgs_old)
        for mid in MSGS:
            table += struct.pack('<III', mid, strs[(mid, 'en')], strs[(mid, 'ja')])
        put('msgs', bytes(table))
        d['msg_count'] = gcnt + len(MSGS)
        return d, bytes(blob)

    # each cave assembled on its own, one after the other (their labels are their own)
    def caves(d):
        text = code_text(d)
        parts, cur = [], None
        for line in text.splitlines():
            t = line.split(';')[0].strip()
            if t.startswith('cave_') and t.endswith(':'):
                cur = [t[:-1], []]
                parts.append(cur)
            elif cur is not None:
                cur[1].append(line)
        at, out, entries = SEC_VA, b'', {}
        for name, lines in parts:
            b = asm('\n'.join(l.split(';')[0] for l in lines), at)
            entries[name] = at
            out += b + b'\xCC' * ((-len(b)) % 4)
            at = SEC_VA + len(out)
        return out, entries

    d0, _ = data_blob(SEC_VA + 0x1000)
    code, _ = caves(d0)
    data_at = SEC_VA + ((len(code) + 15) & ~15)
    d, blob = data_blob(data_at)
    code, entries = caves(d)
    assert SEC_VA + len(code) <= data_at
    section = code + b'\0' * (data_at - SEC_VA - len(code)) + blob
    assert len(section) <= SEC_VSIZE

    def entry(name):
        return entries[name]

    def call(at, target, length):
        b = b'\xE8' + struct.pack('<i', target - (at + 5))
        x.wr(at, b + b'\x90' * (length - 5))

    # --- the section --------------------------------------------------------------------------------------------
    add_section(x, b'NGCAM', SEC_VA, SEC_VSIZE, section)

    # --- the menu -----------------------------------------------------------------------------------------------
    x.wr(0x20DADB, b'\x8B\x04\xBD' + struct.pack('<I', d['labels']))  # mov eax, [edi*4+labels]
    x.wr(0x20DD0B, bytes.fromhex('83 ff 08'))                          # eight rows drawn
    x.wr(0x20D8ED, bytes([0xBB]) + struct.pack('<I', ROW_Y))           # ... from a little higher
    x.wr(0x20DD04, bytes([0x03, 0x1D]) + struct.pack('<I', d['step']))  # ... closer (add ebx, [step])
    call(0x20DC63, entry('cave_val'), 6)
    call(0x210B8A, entry('cave_help'), 7)
    call(0x210BB2, entry('cave_help'), 7)
    call(0x172F6B, entry('cave_in'), 5)
    call(0x1730EA, entry('cave_def'), 6)
    page = bytearray(x.rd(PAGE3, 16))
    page[2] = 7
    page[12:16] = struct.pack('<I', d['rows'])
    x.wr(PAGE3, bytes(page))
    x.wr(MSG_GROUPS + 8, struct.pack('<I', d['msgs']))
    x.wr(MSG_COUNTS + 4, struct.pack('<H', d['msg_count']))

    # --- the camera -----------------------------------------------------------------------------------------------
    boot = bytes.fromhex('66 c7 05 fd e0 89 00 01 01 c6 05 ff e0 89 00 01')  # the three stores of 1, as two
    x.wr(0x2D8AA8, boot)
    call(0x2D8AA8 + len(boot), entry('cave_cam'), 27 - len(boot))
    call(0x162C6A, entry('cave_cam'), 6)
    call(0x45FE8, entry('cave_stick'), 6)
    call(0x4E17F, entry('cave_fp'), 6)
    call(0x45F2C, entry('cave_g1'), 6)
    call(0x4515B, entry('cave_g2'), 13)
    call(0x45BC2, entry('cave_g3'), 9)
    call(0x47490, entry('cave_g4'), 7)
    call(0x322C7, entry('cave_g56'), 6)
    call(0x323AE, entry('cave_g56'), 6)

    open(out, 'wb').write(x.d)
    print('%s: section NGCAM %d bytes (code %d), sha256 %s' % (out, len(section), len(code),
                                                               hashlib.sha256(x.d).hexdigest()))


def add_section(x, name, va, vsize, raw):
    """One more section: its header after the others (what followed them moved down 56 bytes), its name and page
    reference counters at the end of the headers, its data at the end of the file."""
    d, base = x.d, x.base
    n, saddr = struct.unpack_from('<II', d, 0x11C)
    hdr_size, = struct.unpack_from('<I', d, 0x108)
    first_raw = min(s[3] for _, s in x.secs())
    tail = saddr - base + n * 56          # what follows the section table
    if any(d[hdr_size:hdr_size + 56 + 12]):
        raise SystemExit('no room after the headers')
    if hdr_size + 56 + 12 > first_raw:
        raise SystemExit('the headers would run into the first section')
    moved = bytes(d[tail:hdr_size])
    d[tail + 56:hdr_size + 56] = moved
    lo, hi = base + tail, base + hdr_size

    def fix(off):
        v, = struct.unpack_from('<I', d, off)
        if lo <= v < hi:
            struct.pack_into('<I', d, off, v + 56)
    for off in (0x14C, 0x150, 0x154, 0x164, 0x168, 0x16C, 0x170):  # debug paths, library versions, logo
        fix(off)
    for i in range(n):
        o = saddr - base + i * 56
        for k in (20, 28, 32):  # name, head / tail page reference counters
            fix(o + k)
    end = hdr_size + 56
    name_va = base + end
    d[end:end + 8] = name.ljust(8, b'\0')
    refs = base + end + 8
    new_hdr_size = end + 12
    raw_at = (len(d) + 0xFFF) & ~0xFFF
    d.extend(b'\0' * (raw_at - len(d)))
    d.extend(raw)
    digest = hashlib.sha1(struct.pack('<I', len(raw)) + raw).digest()
    hdr = struct.pack('<9I', 0x07, va, vsize, raw_at, len(raw), name_va, 0, refs, refs + 2) + digest
    d[tail:tail + 56] = hdr
    struct.pack_into('<I', d, 0x11C, n + 1)
    struct.pack_into('<I', d, 0x108, new_hdr_size)
    struct.pack_into('<I', d, 0x10C, va + vsize - base)  # size of image


if __name__ == '__main__':
    main()
