"""Ninja Gaiden Black Hurricane vb-jp-x2.1 (Japanese / English): two more Controller Settings options, as a patch of its
default.xbe (the console, emulators and the PC port alike).

    python tools/ngb_x21_menu.py <vb-jp-x2.1 default.xbe> <out.xbe>      (needs keystone-engine)

- Intercept gets a third value, Off. Intercept is item 0x13C (x2.1 gives it in chapter 1 where the US release gave
  0xBE; yasik_cmn.afs, 2026-11-12): Off takes it out of the inventory, Hurricane Pack / Black put it in when it is
  missing, from whatever save; both run where x2.1 switches the move's data (0x32F31, each frame in play).
- Ultimate Technique Camera: Off / Left-Right / All. Lucaz's A' (github.com/760194962/Ninja-Gaiden-Black-Patch-
  Research): while an Ultimate Technique runs ([0x5895E4] != 0) the camera loop at 0x47A37 reads the stick pair four
  bytes earlier; Left-Right also zeroes its first (up / down) axis, All keeps both. Ninpo is not touched.
- The option is the settings' byte +0x12 (menu copy 0xDA42F2, saved copy 0xDB9412), unused by the game and by x2.1.
- New code and data: a section of its own (NGBX, 0xE17000), as tools/ng04_camera_menu.py does it.
"""
import hashlib
import os
import struct
import sys

from keystone import KS_ARCH_X86, KS_MODE_32, Ks

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ng04_camera_menu import Xbe, add_section  # noqa: E402

JP_SHA256 = '535778c8a4623195c8f320b85d2d139f20ff13c020d40150adaed54cfdeffc38'

MENU, SAVED = 0xDA42E0, 0xDB9400
OPT_UT, OPT_IC = 0x12, 0x15
LANG = 0x8DE524
UT_ACTIVE = 0x5895E4
INV, INV_END = 0x5891D0, 0x5893D0
HAS_ITEM, ADD_ITEM = 0x1542C0, 0x154300   # esi = id -> count; eax = id, ebx = count
TOGGLE, SOUND = 0x17D910, 0x1E3400
ITEM_IC = 0x13C
MSG_GROUPS, MSG_COUNTS = 0x436240, 0x43626C
PAGE3 = 0x429D00
SEC_VA, SEC_VSIZE = 0xE17000, 0x2000
ROW_Y, ROW_STEP = 0x68, 35

# x2.1's own code this builds on (vb-jp-x2.1 addresses)
X_VAL, X_INPUT, X_DEF, X_SWITCH = 0xDC7C20, 0xDC7D10, 0xDC7D70, 0xDC80A0
X_GRANTS = (0xDC71C0, 0xDC7B98, 0xDC7BE9, 0xDC81C7)  # x2.1's mov eax, 0x13C; mov ebx, 1; call AddItem
X_IC_HELP_JA = 0xDC835C  # x2.1's Japanese help line for Intercept (in its own glyph encoding), kept as it is

MSGS = {
    0x20100: ('Ultimate Tech Camera', 'Ultimate Tech Camera'),
    0x20102: ('Left / Right', 'Left / Right'),
    0x20103: ('All Directions', 'All Directions'),
    0x20104: ('Lets the left stick move the camera during Ultimate Techniques.',
              'アルティメットテクニック中に左スティックでカメラ操作'),  # kana: the font has no 義
    0x20105: ('Selects the Intercept move: Hurricane Pack, Black, or Off.', X_IC_HELP_JA),
}
LABELS = [0x2000C, 0x2000D, 0x20F11, 0x20F15, 0x2000F, 0x20100, 0x20004, 0x20005]
HELP = [0x40, 0x43, 0xF13, 0x105, 0x45, 0x104, 0x3A, 0x3B]
UT_VALUES = [0x20017, 0x20102, 0x20103]          # Off (the game's), Left / Right, All Directions
IC_VALUES = [0x20F17, 0x20F18, 0x20017]          # x2.1's Hurricane Pack, Black, then Off


def main():
    src, out = sys.argv[1:3]
    data = open(src, 'rb').read()
    if hashlib.sha256(data).hexdigest() != JP_SHA256:
        raise SystemExit('not vb-jp-x2.1 default.xbe')
    x = Xbe(data)
    ks = Ks(KS_ARCH_X86, KS_MODE_32)
    sites = {
        'ut': (0x47A37, 'eb 07 8d a4 24 00 00 00 00 0f bf 30'),
        'switch': (0x32F31, 'e8 6a 51 d9 00 90 90'),
        'value': (0x21D8A3, 'e8 78 a3 ba 00 90'),
        'input': (0x17EF0B, 'e8 00 8e c4 00'),
        'defaults': (0x17F08A, 'e8 e1 8c c4 00 90'),
        'label': (0x21D71B, '8b 04 bd a8 83 dc 00'),
        'rows': (0x21D94B, '83 ff 07'),
        'row_y': (0x21D52D, 'bb 70 00 00 00'),
        'row_step': (0x21D944, '03 1d 78 b4 43 00'),
        'help_n': (0xDC7D8A, '83 f8 07'),
        'help_t': (0xDC7D8F, '66 8b 04 45 c4 83 dc 00'),
        'ic_val': (0xDC7C42, '31 c0 80 3d f5 42 da 00 00 0f 95 c0 8b 04 85 c0 84 dc 00'),
        'ic_in': (0xDC7D28, '31 c9 80 3d f5 42 da 00 00 0f 95 c1 33 f6 b8 01 00 00 00'),
        'page3': (PAGE3, '03 00 06 01 01 01 00 02 00 03 00 00 e0 83 dc 00'),
    }
    for va, b in sites.values():
        x.expect(va, bytes.fromhex(b))
    rows_old = x.rd(0xDC83E0, 7 * 28)
    gtbl, = struct.unpack('<I', x.rd(MSG_GROUPS + 8, 4))
    gcnt, = struct.unpack('<H', x.rd(MSG_COUNTS + 4, 2))
    msgs_old = x.rd(gtbl, gcnt * 12)

    def code_text(d):
        return f'''
        cave_ut:                        ; Ultimate Technique camera (A')
            cmp word ptr [{UT_ACTIVE:#x}], 0
            je ut_out
            cmp byte ptr [{SAVED + OPT_UT:#x}], 0
            je ut_out
            sub eax, 4
            cmp byte ptr [{SAVED + OPT_UT:#x}], 1
            jne ut_out
            mov word ptr [eax], 0
        ut_out:
            ret

        cave_item:                      ; after x2.1's switch: Intercept (item 0x13C) per the option
            call {X_SWITCH:#x}
            pushfd
            pushad
            mov ebx, [0x4B7528]
            test ebx, ebx
            je item_out
            cmp dword ptr [ebx], 0x5f726863
            jne item_out
            cmp dword ptr [ebx+4], 0x32746164
            jne item_out
            cmp byte ptr [{SAVED + OPT_IC:#x}], 2
            jne item_give
            mov esi, {ITEM_IC:#x}
            call {HAS_ITEM:#x}
            test eax, eax
            je item_out
            mov eax, {ITEM_IC:#x}               ; removed through AddItem with a negative count: it also sets the
            mov ebx, -128                       ; inventory-changed flag [0x7F1A94]; writing the slot directly fails
            call {ADD_ITEM:#x}                  ; the game's inventory check, which drops back to the title screen
            jmp item_out
        item_give:
            mov esi, {ITEM_IC:#x}
            call {HAS_ITEM:#x}
            test eax, eax
            jne item_out
            mov eax, {ITEM_IC:#x}
            mov ebx, 1
            call {ADD_ITEM:#x}
        item_out:
            popad
            popfd
            ret

        cave_val:                       ; Controller Settings' values: row 5 is ours, the rest x2.1's
            cmp edi, 5
            je val_ut
            jmp {X_VAL:#x}
        val_ut:
            mov [esp+0x1c], eax
            add esp, 4
            movzx eax, byte ptr [{MENU + OPT_UT:#x}]
            mov eax, [eax*4+{d['ut_vals']:#x}]
            movzx edx, byte ptr [{LANG:#x}]
            push edx
            jmp 0x21D8F5

        cave_in:                        ; left / right: row 5 steps our option, the rest x2.1's
            cmp byte ptr [edi+0xc], 0xff
            jne in_x
            cmp cl, 5
            je in_ut
        in_x:
            jmp {X_INPUT:#x}
        in_ut:
            pushad
            movzx ecx, byte ptr [{MENU + OPT_UT:#x}]
            xor esi, esi
            mov eax, 2
            movsx edx, byte ptr [edi+0x10]
            mov ebx, [ebp+0x10]
            push ecx
            call {TOGGLE:#x}
            add esp, 4
            mov [{MENU + OPT_UT:#x}], al
            mov [{SAVED + OPT_UT:#x}], al
            mov al, [edi+0x10]
            test al, al
            je in_quiet
            push 0x40
            push 1
            call {SOUND:#x}
            add esp, 8
        in_quiet:
            popad
            xor ecx, ecx
            mov al, [edi+0xc]
            cmp al, 0xff
            ret

        cave_def:                       ; Default Settings: ours too
            call {X_DEF:#x}
            mov byte ptr [{MENU + OPT_UT:#x}], 0
            mov byte ptr [{SAVED + OPT_UT:#x}], 0
            ret

        cave_grant:                     ; x2.1's own top-ups of Intercept: none while it is Off
            cmp byte ptr [{SAVED + OPT_IC:#x}], 2
            je grant_out
            mov eax, {ITEM_IC:#x}
            mov ebx, 1
            call {ADD_ITEM:#x}
        grant_out:
            ret
        '''

    def data_blob(at):
        d, blob = {}, bytearray()

        def put(name, b, align=4):
            while (at + len(blob)) % align:
                blob.append(0)
            d[name] = at + len(blob)
            blob.extend(b)
        put('labels', struct.pack('<8I', *LABELS))
        put('help', struct.pack('<8H', *HELP))
        put('ut_vals', struct.pack('<3I', *UT_VALUES))
        put('ic_vals', struct.pack('<3I', *IC_VALUES))
        put('step', struct.pack('<i', ROW_STEP))
        rows = bytearray(rows_old[:5 * 28])
        r = bytearray(rows_old[4 * 28:5 * 28])  # a copy of Controls While in Water's row (two-way toggle)
        r[0] = 5
        rows += r
        for rid, src_row in ((6, 5), (7, 6)):
            r = bytearray(rows_old[src_row * 28:(src_row + 1) * 28])
            r[0] = rid
            rows += r
        put('rows', bytes(rows))
        strs = {}
        for mid, (en, ja) in MSGS.items():
            for k, s in (('en', en), ('ja', ja)):
                if isinstance(s, int):  # a string the program already has
                    strs[(mid, k)] = s
                    continue
                put('s%x%s' % (mid, k), s.encode('cp932') + b'\0', 1)
                strs[(mid, k)] = d['s%x%s' % (mid, k)]
        table = bytearray(msgs_old)
        for mid in MSGS:
            table += struct.pack('<III', mid, strs[(mid, 'en')], strs[(mid, 'ja')])
        put('msgs', bytes(table))
        d['msg_count'] = gcnt + len(MSGS)
        return d, bytes(blob)

    def caves(d):
        parts, cur = [], None
        for line in code_text(d).splitlines():
            t = line.split(';')[0].strip()
            if t.startswith('cave_') and t.endswith(':'):
                cur = [t[:-1], []]
                parts.append(cur)
            elif cur is not None:
                cur[1].append(line.split(';')[0])
        at, out_b, entries = SEC_VA, b'', {}
        for name, lines in parts:
            enc, _ = ks.asm('\n'.join(lines), at)
            entries[name] = at
            out_b += bytes(enc) + b'\xCC' * ((-len(enc)) % 4)
            at = SEC_VA + len(out_b)
        return out_b, entries

    d0, _ = data_blob(SEC_VA + 0x1000)
    code, _ = caves(d0)
    data_at = SEC_VA + ((len(code) + 15) & ~15)
    d, blob = data_blob(data_at)
    code, entries = caves(d)
    section = code + bytes(data_at - SEC_VA - len(code)) + blob
    assert len(section) <= SEC_VSIZE

    def call(at, target, length):
        x.wr(at, b'\xE8' + struct.pack('<i', target - (at + 5)) + b'\x90' * (length - 5))

    add_section(x, b'NGBX', SEC_VA, SEC_VSIZE, section)

    # the page: eight rows
    page = bytearray(x.rd(PAGE3, 16))
    page[2] = 7
    page[12:16] = struct.pack('<I', d['rows'])
    x.wr(PAGE3, bytes(page))
    x.wr(0x21D71B, bytes([0x8B, 0x04, 0xBD]) + struct.pack('<I', d['labels']))
    x.wr(0x21D94B, bytes.fromhex('83 ff 08'))
    x.wr(0x21D52D, bytes([0xBB]) + struct.pack('<I', ROW_Y))
    x.wr(0x21D944, bytes([0x03, 0x1D]) + struct.pack('<I', d['step']))
    x.wr(0xDC7D8A, bytes.fromhex('83 f8 08'))
    x.wr(0xDC7D8F, bytes([0x66, 0x8B, 0x04, 0x45]) + struct.pack('<I', d['help']))
    x.wr(MSG_GROUPS + 8, struct.pack('<I', d['msgs']))
    x.wr(MSG_COUNTS + 4, struct.pack('<H', d['msg_count']))
    call(0x21D8A3, entries['cave_val'], 6)
    call(0x17EF0B, entries['cave_in'], 5)
    call(0x17F08A, entries['cave_def'], 6)

    # Intercept: three values (x2.1's value and step read the byte as 0 / 1)
    x.wr(0xDC7C42, bytes([0x0F, 0xB6, 0x05]) + struct.pack('<I', MENU + OPT_IC) + b'\x90' * 5 +
         bytes([0x8B, 0x04, 0x85]) + struct.pack('<I', d['ic_vals']))
    x.wr(0xDC7D28, bytes([0x0F, 0xB6, 0x0D]) + struct.pack('<I', MENU + OPT_IC) + b'\x90' * 5 +
         bytes([0x33, 0xF6, 0xB8, 0x02, 0x00, 0x00, 0x00]))
    call(0x32F31, entries['cave_item'], 7)
    # x2.1 tops Intercept up in four places (Eternal Legend each frame, mission starts): mov eax, 0x13C; mov ebx, 1;
    # call AddItem. With Off those adds and our removal fought every frame, and loading such a mission went back to
    # the title; now they ask the option first.
    for at in X_GRANTS:
        x.expect(at, bytes.fromhex('b8 3c 01 00 00 bb 01 00 00 00 e8') + struct.pack('<i', ADD_ITEM - (at + 15)))
        call(at, entries['cave_grant'], 15)

    # the Ultimate Technique camera
    call(0x47A37, entries['cave_ut'], 9)

    open(out, 'wb').write(x.d)
    print('%s: section NGBX %d bytes (code %d), sha256 %s' % (out, len(section), len(code), hashlib.sha256(x.d).hexdigest()))


if __name__ == '__main__':
    main()
