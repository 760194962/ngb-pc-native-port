"""Ninja Gaiden Black Hurricane vb-jp-x2.1 (Japanese / English): more options in the game's own OPTIONS menu, as a patch
of its default.xbe (the console, emulators and the PC port alike).

    python tools/ngb_x21_menu.py <vb-jp-x2.1 default.xbe> <out.xbe>      (needs keystone-engine)

General Settings (rows 5-7, before Default Settings / Exit):
- Essence-Only Charging (settings +0x05 bit 0): holding the button no longer counts the Ultimate Technique charge up
  (0x23B04); absorbed essence still does, as in Ninja Gaiden (2004).
- Skip Any Cutscene (+0x0A bit 0): START skips cutscenes not watched before (the check at 0x1DC052).
- Intercept (x2.1's byte +0x15), moved here from Controller Settings, with a third value, Off. Intercept is item 0x13C:
  Off takes it out of the inventory, Hurricane Pack / Black put it in when it is missing, from whatever save; both run
  where x2.1 switches the move's data (0x32F31, each frame in play).
Controller Settings, in Intercept's row:
- Ultimate Tech Camera (+0x12): Off / Left-Right / All. Lucaz's A' (github.com/760194962/Ninja-Gaiden-Black-Patch-
  Research): while an Ultimate Technique runs ([0x5895E4] != 0) the camera loop at 0x47A37 reads the stick pair four
  bytes earlier; Left-Right also zeroes its first (up / down) axis, All keeps both. Ninpo is not touched.
x2.1's chapter-1 corpse (yasik_cmn.afs: it checks for Intercept and gives it besides the elixir 0xBE, so an owned
Intercept leaves the corpse "can't carry more") is not this patch's: the release keeps the US yasik_cmn.afs, the only
place x2.1 changed it. The settings bytes used are unused by the game and by x2.1 (menu copy 0xDA42E0, saved copy
0xDB9400). New code and data: a section of its own (NGBX, 0xE17000), as tools/ng04_camera_menu.py does it.
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
PAGE1, G_ROWS = 0x429CD0, 0x4296F8   # General Settings' page and its seven rows
SEC_VA, SEC_VSIZE = 0xE17000, 0x2000

# x2.1's own code this builds on (vb-jp-x2.1 addresses)
X_VAL, X_INPUT, X_DEF, X_SWITCH = 0xDC7C20, 0xDC7D10, 0xDC7D70, 0xDC80A0
X_GRANTS = (0xDC71C0, 0xDC7B98, 0xDC7BE9, 0xDC81C7)  # x2.1's mov eax, 0x13C; mov ebx, 1; call AddItem
X_IC_HELP_JA = 0xDC835C  # x2.1's Japanese help line for Intercept (in its own glyph encoding), kept as it is

# General Settings: three more rows (5-7) before Default Settings / Exit. Essence-Only Charging is the settings' byte
# +0x05 bit 0 and Skip Any Cutscene +0x0A bit 0 (menu copy and saved copy), unused by the game and by x2.1; Intercept,
# x2.1's own byte +0x15, moves here from Controller Settings (its row there now holds the Ultimate Tech Camera).
OPT_EO, OPT_SK = 0x05, 0x0A
ON, OFF = 0x20016, 0x20017
IC_VALUES = [0x20110, 0x20111, OFF]              # Hurricane Pack, Black, Off (ours: x2.1's 0x20Fxx texts only
G_LABELS = [0x20006, 0x20001, 0x20002, 0x20003, 0x20007, 0x20106, 0x20107, 0x2010F, 0x20004, 0x20005]  # its code reads)
G_HELP = [0x109, 0x10A, 0x105]
G_ROW_Y, G_ROW_STEP = 0x60, 31   # ten rows above the help line (the game's: 0x70, 40 for seven)
G_VALUES = [(OPT_EO, 1, 1, [OFF, ON]),           # row 5: byte, mask, last value, names
            (OPT_SK, 1, 1, [OFF, ON]),           # row 6
            (OPT_IC, 3, 2, IC_VALUES)]           # row 7
UT_COUNT = 0x5895E4      # Ultimate Technique charge, counted up each frame while the button is held (0x23B04)
WATCHED = 0xDB9477       # one byte per cutscene: seen before (0x1DC052 lets Start skip only those)

MSGS = {
    0x20106: ('Essence-Only Charging', 'Essence-Only Charging'),
    0x20107: ('Skip Any Cutscene', 'Skip Any Cutscene'),
    0x2010F: ('Intercept', 'Intercept'),
    0x20110: ('Hurricane Pack', 'Hurricane Pack'),
    0x20111: ('Black', 'Black'),
    0x20109: ('Ultimate Techniques charge only by absorbing essence, as in NINJA GAIDEN (2004).',
              'アルティメットテクニックは魂をとりこんだ時のみためられます'),  # the font has no 取
    0x2010A: ('Lets the START button skip cutscenes not watched before.',
              '見ていないムービーもスタートボタンでスキップできます'),
    0x20100: ('Ultimate Tech Camera', 'Ultimate Tech Camera'),
    0x20102: ('Left / Right', 'Left / Right'),
    0x20103: ('All Directions', 'All Directions'),
    0x20104: ('Lets the left stick move the camera during Ultimate Techniques.',
              'アルティメットテクニック中に左スティックでカメラ操作'),  # kana: the font has no 義
    0x20105: ('Selects the Intercept move: Hurricane Pack, Black, or Off.', X_IC_HELP_JA),
}
# Controller Settings: x2.1's seven rows, Intercept's (3) now the Ultimate Tech Camera
LABELS = [0x2000C, 0x2000D, 0x20F11, 0x20100, 0x2000F, 0x20004, 0x20005]
HELP = [0x40, 0x43, 0xF13, 0x104, 0x45, 0x3A, 0x3B]
UT_VALUES = [OFF, 0x20102, 0x20103]              # Off (the game's), Left / Right, All Directions


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
        'help_n': (0xDC7D8A, '83 f8 07'),
        'help_t': (0xDC7D8F, '66 8b 04 45 c4 83 dc 00'),
        'x_def': (X_DEF, '88 0d f0 42 da 00 c6 05 f5 42 da 00 00 c3'),
        'page1': (PAGE1, '01 01 06 01 01 01 00 00 00 03 00 00 f8 96 42 00'),
        'g_in': (0x17E923, '0f b6 c1 48 83 f8 03 0f 87'),
        'g_def': (0x17EAB2, 'e8 79 bd 05 00'),
        'g_label': (0x21C9BD, '8b 84 b4 b4 00 00 00'),
        'g_val': (0x21CB38, '83 fe 04 89 44 24 20 0f 87 13 02 00 00 ff 24 b5 88 cd 21 00'),
        'g_rows': (0x21CD5F, '83 fe 07'),
        'g_row_y': (0x21C7CB, 'bb 70 00 00 00'),
        'g_row_step': (0x21CD58, '03 1d 78 b4 43 00'),
        'g_mark': (0x17FE5A, 'c6 44 04 34 03'),
        'help_a': (0x2209FA, 'e8 81 73 ba 00 90 90'),
        'help_b': (0x220A22, 'e8 59 73 ba 00 90 90'),
        'eo': (0x23B04, '66 40 66 a3 e4 95 58 00'),
        'cs': (0x1DC052, '8a 82 77 94 db 00 84 c0'),
    }
    for va, b in sites.values():
        x.expect(va, bytes.fromhex(b))
    g_rows_old = x.rd(G_ROWS, 7 * 28)
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

        cave_val:                       ; Controller Settings' values: row 3 (Intercept's in x2.1) is ours
            cmp edi, 3
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

        cave_in:                        ; left / right: row 3 steps our option, the rest x2.1's
            cmp byte ptr [edi+0xc], 0xff
            jne in_x
            cmp cl, 3
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

        cave_def:                       ; Controller Settings' Default Settings: x2.1's less Intercept (General's now)
            mov [0xDA42F0], cl
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

        cave_gin:                       ; General Settings, left / right on a toggle row: rows 5-7 are ours
            cmp cl, 5
            jb gin_orig
            cmp cl, 7
            ja gin_orig
            cmp byte ptr [edi+0x10], 0
            je gin_none
            pushad
            movzx ecx, cl
            lea ebp, [ecx*8+{d['g_vals'] - 40:#x}]  ; {{byte, mask, last value, 0, names}}
            movzx eax, byte ptr [ebp]
            push eax
            movzx ecx, byte ptr [eax+{MENU:#x}]
            movzx eax, byte ptr [ebp+1]
            and ecx, eax
            movzx eax, byte ptr [ebp+2]
            xor esi, esi
            movsx edx, byte ptr [edi+0x10]
            mov ebx, [edi+0x2c]
            mov ebx, [ebx+0x10]
            push ecx
            call {TOGGLE:#x}
            add esp, 4
            pop edx
            mov cl, [ebp+1]
            not cl
            and cl, [edx+{MENU:#x}]
            or cl, al
            mov [edx+{MENU:#x}], cl
            mov [edx+{SAVED:#x}], cl
            push 0x40
            push 1
            call {SOUND:#x}
            add esp, 8
            popad
        gin_none:
            or eax, -1                  ; the caller's ja then leaves
            cmp eax, 3
            ret
        gin_orig:
            movzx eax, cl
            dec eax
            cmp eax, 3
            ret

        cave_gdef:                      ; General Settings' Default Settings: ours too
            call 0x1DA830
            and byte ptr [{MENU + OPT_EO:#x}], 0xfe
            and byte ptr [{SAVED + OPT_EO:#x}], 0xfe
            and byte ptr [{MENU + OPT_SK:#x}], 0xfe
            and byte ptr [{SAVED + OPT_SK:#x}], 0xfe
            mov byte ptr [{MENU + OPT_IC:#x}], 0
            mov byte ptr [{SAVED + OPT_IC:#x}], 0
            ret

        cave_gmark:                     ; General Settings' row marks: five bytes on the stack below its cookie; the
            cmp eax, 5                  ; game's rows 5 and 6 fell in the padding, row 8 reached the cookie (and
            jae gmark_out               ; leaving the page then stopped the game)
            mov byte ptr [esp+eax+0x38], 3
        gmark_out:
            ret

        cave_gval:                      ; General Settings' values (jumped to from 0x21CB38)
            mov [esp+0x20], eax
            cmp esi, 4
            ja gval_ours
            jmp dword ptr [esi*4+0x21CD88]
        gval_ours:
            cmp esi, 7
            ja 0x21CD58
            lea edx, [esi*8+{d['g_vals'] - 40:#x}]
            movzx eax, byte ptr [edx]
            movzx eax, byte ptr [eax+{MENU:#x}]
            and al, [edx+1]
            mov edx, [edx+4]
            mov eax, [edx+eax*4]
            movzx ecx, byte ptr [{LANG:#x}]
            push ecx
            call 0x1E6AE0
            add esp, 4
            mov ebx, eax
            jmp 0x21CCBA

        cave_help:                      ; help line: General rows 5-7 are ours, Default / Exit moved down by three
            cmp word ptr [0x8DE4CC], 0
            jne help_orig
            cmp eax, 5
            jb help_orig
            cmp eax, 8
            jb help_ours
            sub eax, 3
        help_orig:
            jmp 0xDC7D80
        help_ours:
            mov ax, [eax*2+{d['g_help'] - 10:#x}]
            mov [ecx], ax
            ret

        cave_eo:                        ; Essence-Only Charging: holding the button no longer counts up
            test byte ptr [{SAVED + OPT_EO:#x}], 1
            jne eo_out
            inc ax
            mov [{UT_COUNT:#x}], ax
        eo_out:
            ret

        cave_cs:                        ; Skip Any Cutscene: every cutscene counts as watched for the Start check
            mov al, [edx+{WATCHED:#x}]
            test byte ptr [{SAVED + OPT_SK:#x}], 1
            je cs_out
            mov al, 1
        cs_out:
            ret

        '''

    def data_blob(at):
        d, blob = {}, bytearray()

        def put(name, b, align=4):
            while (at + len(blob)) % align:
                blob.append(0)
            d[name] = at + len(blob)
            blob.extend(b)
        put('labels', struct.pack('<7I', *LABELS))
        put('help', struct.pack('<7H', *HELP))
        put('ut_vals', struct.pack('<3I', *UT_VALUES))
        put('g_labels', struct.pack('<10I', *G_LABELS))
        put('g_help', struct.pack('<3H', *G_HELP))
        names = []
        for i, (_, _, _, vals) in enumerate(G_VALUES):
            put('g_names%d' % i, struct.pack('<%dI' % len(vals), *vals))
            names.append(d['g_names%d' % i])
        put('g_vals', b''.join(struct.pack('<BBBxI', o, m, last, n) for (o, m, last, _), n in zip(G_VALUES, names)))
        put('g_step', struct.pack('<i', G_ROW_STEP))
        g_rows = bytearray(g_rows_old[:5 * 28])
        for rid in (5, 6, 7):
            r = bytearray(g_rows_old[3 * 28:4 * 28])  # a copy of Subtitles' row (a toggle)
            r[0] = rid
            g_rows += r
        for rid, src_row in ((8, 5), (9, 6)):
            r = bytearray(g_rows_old[src_row * 28:(src_row + 1) * 28])
            r[0] = rid
            g_rows += r
        put('g_rows', bytes(g_rows))
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

    # Controller Settings: x2.1's seven rows, our labels and help lines (row 3 the Ultimate Tech Camera)
    x.wr(0x21D71B, bytes([0x8B, 0x04, 0xBD]) + struct.pack('<I', d['labels']))
    x.wr(0xDC7D8F, bytes([0x66, 0x8B, 0x04, 0x45]) + struct.pack('<I', d['help']))
    x.wr(MSG_GROUPS + 8, struct.pack('<I', d['msgs']))
    x.wr(MSG_COUNTS + 4, struct.pack('<H', d['msg_count']))
    call(0x21D8A3, entries['cave_val'], 6)
    call(0x17EF0B, entries['cave_in'], 5)
    call(0x17F08A, entries['cave_def'], 6)

    call(0x32F31, entries['cave_item'], 7)
    # x2.1 tops Intercept up in four places (Eternal Legend each frame, mission starts): mov eax, 0x13C; mov ebx, 1;
    # call AddItem. With Off those adds and our removal fought every frame, and loading such a mission went back to
    # the title; now they ask the option first.
    for at in X_GRANTS:
        x.expect(at, bytes.fromhex('b8 3c 01 00 00 bb 01 00 00 00 e8') + struct.pack('<i', ADD_ITEM - (at + 15)))
        call(at, entries['cave_grant'], 15)

    # the Ultimate Technique camera
    call(0x47A37, entries['cave_ut'], 9)

    # General Settings: ten rows (Total Play Time ... Flag Select, ours, Default Settings, Exit)
    page = bytearray(x.rd(PAGE1, 16))
    page[2] = 9
    page[12:16] = struct.pack('<I', d['g_rows'])
    x.wr(PAGE1, bytes(page))
    call(0x17E923, entries['cave_gin'], 7)
    call(0x17EAB2, entries['cave_gdef'], 5)
    x.wr(0x21C9BD, bytes([0x8B, 0x04, 0xB5]) + struct.pack('<I', d['g_labels']))
    x.wr(0x21CB38, b'\xE9' + struct.pack('<i', entries['cave_gval'] - 0x21CB3D) + b'\x90' * 15)
    x.wr(0x21CD5F, bytes.fromhex('83 fe 0a'))
    call(0x17FE5A, entries['cave_gmark'], 5)
    x.wr(0x21C7CB, bytes([0xBB]) + struct.pack('<I', G_ROW_Y))
    x.wr(0x21CD58, bytes([0x03, 0x1D]) + struct.pack('<I', d['g_step']))
    call(0x2209FA, entries['cave_help'], 7)
    call(0x220A22, entries['cave_help'], 7)
    call(0x23B04, entries['cave_eo'], 8)
    call(0x1DC052, entries['cave_cs'], 6)

    open(out, 'wb').write(x.d)
    print('%s: section NGBX %d bytes (code %d), sha256 %s' % (out, len(section), len(code), hashlib.sha256(x.d).hexdigest()))


if __name__ == '__main__':
    main()
