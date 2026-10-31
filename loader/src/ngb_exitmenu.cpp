// Ninja Gaiden Black x2.1: a ninth main-menu row, 退出游戏 (EXIT GAME), that asks 要退出游戏吗？ Ⓐ是 Ⓑ否 in the game's
// own green box (the one Ranking shows for "create a new Gamertag") and leaves the program on Ⓐ.
//
// The main menu (docs/08, r399-r405):
// - A list object at 0xC075C0 (FUN_0017A8B0): eight 8-byte rows inline at +0x00 {u32 unlock id, u8 +4 / +5 shown,
//   +6 new}, then the list descriptor at +0x40 {rows, count = 8}; the cursor is +0x90. FUN_0017A9C0 (entering the
//   menu) and FUN_0017B570 (leaving it) write the inline rows; the list code reads them through the descriptor:
//   FUN_0017A010 draws, FUN_0017A400 lays a row out (its text is message 0x90000 | (base + row), base 6: rows
//   0x90006-0x9000D; the row spacing comes from the u16 table 0x45A9BA by the number of shown rows, 36 for 8, and the
//   ninth entry of that table is already another value), FUN_00179EC0 moves the cursor.
// - FUN_0017B3E0 (stdcall, the pad port; ebx = the object) runs the menu's input while it is idle: the port's record
//   0xD219A0 + port * 0x78, +8 = buttons pressed this frame, 0x510 select -> FUN_0017B010 (row -> mode through the
//   8-row table 0x3E9D70), 0x220 back -> FUN_0017B260. Sounds: FUN_001E3400(1 move / 5 select / 3 back, 0x40).
// - Text: FUN_001E6AE0 (eax = id, cdecl column): table 0x436240[id >> 16] of {id, column 0, column 1}; x2.1's
//   Chinese is column 0 for 0x9xxxx and column 1 for 0x4xxxx (the hanhua session, 2026-10-22). Codes are x2.1's own
//   two-byte table (退 9ACD 出 8A8D 游 97B1 戏 94E3 要 8EBC 吗 8ACA ？ 8FD7 是 8CCF 否 8ACB, full-width space 8982); ASCII
//   is single bytes and @BA / @BB put the button pictures in.
// - The green box: FUN_0022D920 (cdecl, a struct whose first u16 is a message number in 0x4xxxx), drawn each frame
//   by the Live screens (0x3000BA) as sprites of the 2D lists (r404: message 0x40004 fetched at 0x22D9C2).
//
// The patch: the rows move to a nine-row copy here (refreshed after the two writers), row 8 always shown with the
// message 0x9000E answered as 退出游戏 at FUN_0017A400's own call (0x9000E is Mission mode's name elsewhere); the
// spacing table gets 32 for nine rows; selecting row 8 opens the box instead of FUN_0017B010, and while it is up
// FUN_0017B3E0 draws it and waits for Ⓐ (exit) or Ⓑ (close) instead of moving the menu.
#include "build_profile.h"
#include "common.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

extern "C" {
// Read by the thunks
volatile uint32_t g_exitmenu_list;      // the list descriptor FUN_0017A400 is laying a row out for
volatile uint32_t g_exitmenu_dialog;    // 1 while the box is up
uint32_t g_exitmenu_english;            // 1: English only (the original US release: no Chinese font, and [0xDB9890]
                                        // is x2.1's language switch)
uint32_t g_exitmenu_a400_resume = 0x0017A406;
uint32_t g_exitmenu_6ae0 = 0x001E6AE0;
uint32_t g_exitmenu_6ae0_resume = 0x001E6AE6;
uint32_t g_exitmenu_a9c0 = 0x0017A9C0;
uint32_t g_exitmenu_b570 = 0x0017B570;
uint32_t g_exitmenu_b010 = 0x0017B010;
uint32_t g_exitmenu_b3e0_resume = 0x0017B3E7;
uint32_t g_exitmenu_ae80 = 0x0017AE80;
}

namespace {

constexpr uint32_t kMenuObj = 0x00C075C0, kMenuDesc = kMenuObj + 0x40;
constexpr uint32_t kRowMessage = 0x0009000E;  // the ninth row's text: base 6 + row 8
constexpr uint32_t kBoxMessage = 0x0004F0E1;  // the box's text (category 4: the green box adds 0x40000 itself)

struct Row {
    uint32_t id;
    uint8_t flag[4];
};
Row g_rows[9];
uint16_t g_spacing[16];
uint16_t g_box[8] = {(uint16_t)(kBoxMessage & 0xFFFF)};
uint32_t g_box_frames;

// 退出游戏 / 要退出游戏吗？ then the buttons, as Ranking's 0x40004 lays them out (a blank line, full-width spaces)
const char kRowZh[] = "\x9A\xCD\x8A\x8D\x97\xB1\x94\xE3";
const char kRowEn[] = "EXIT GAME";
const char kBoxZh[] = "\x8E\xBC\x9A\xCD\x8A\x8D\x97\xB1\x94\xE3\x8A\xCA\x8F\xD7\n\n"
                      "\x89\x82\x89\x82\x89\x82\x89\x82\x89\x82\x89\x82\x89\x82\x89\x82\x89\x82\x89\x82"
                      "@BA\x8C\xCF\x89\x82\x89\x82@BB\x8A\xCB";
const char kBoxEn[] = "Exit the game?\n\n                    @BA Yes   @BB No";

using SoundFn = void(__cdecl *)(int, int);
using BoxFn = void(__cdecl *)(void *);
const SoundFn kSound = (SoundFn)(uintptr_t)0x001E3400;
const BoxFn kBox = (BoxFn)(uintptr_t)0x0022D920;

// The nine rows from the object's eight (after the game wrote them), and the descriptor pointed at them
extern "C" void __cdecl exitmenu_sync()
{
    uint32_t *desc = (uint32_t *)(uintptr_t)kMenuDesc;
    memcpy(g_rows, (const void *)(uintptr_t)kMenuObj, 8 * sizeof(Row));
    g_rows[8] = {6, {1, 1, 0, 0}}; // unlock id 6 (always), shown, not new
    desc[0] = (uint32_t)(uintptr_t)g_rows;
    desc[1] = 9;
}

extern "C" void __cdecl exitmenu_open()
{
    // The four resource slots (0x8DBAA8, 0x20 each: state 2 = loaded, +2 the id): Ranking has 0x1E there
    const uint8_t *slot = (const uint8_t *)(uintptr_t)0x008DBAA8;
    logf("exit menu: resource slots %02x/%02x %02x/%02x %02x/%02x %02x/%02x (state/id)", slot[0], slot[2], slot[32], slot[34],
         slot[64], slot[66], slot[96], slot[98]);
    kSound(5, 0x40);
    g_box_frames = 0;
    g_exitmenu_dialog = 1;
    logf("exit menu: 退出游戏 selected, asking");
}

// One idle frame of the menu while the box is up: Ⓐ / Ⓑ instead of the menu's input
extern "C" void __cdecl exitmenu_frame(uint32_t port)
{
    uint32_t pressed = *(const uint32_t *)(uintptr_t)(0x00D219A0 + (port & 0xFF) * 0x78 + 8);
    if (g_box_frames++ > 0) { // not the press that opened it
        if (pressed & 0x510) {
            if (!exit_requested()) {
                kSound(5, 0x40);
                logf("exit menu: yes, leaving");
                request_exit(0, "EXIT GAME on the main menu");
            }
        } else if (pressed & 0x220) {
            kSound(3, 0x40);
            g_exitmenu_dialog = 0;
            logf("exit menu: no, back to the menu");
        }
    }
}

// The box, after the menu has drawn its rows (so it is on top). Ranking's set-up (FUN_0022D250) gives the box its
// text: [0x8F9488] = 1 (write the text), [0x8F94CE] = the text column (1, Chinese, when [0xDB9890] is 0), the style
// [0x43B6C0] / [0x43B4A0] (1 Chinese, 2 English). They are set for this call only and put back.
extern "C" void __cdecl exitmenu_draw()
{
    if (!g_exitmenu_dialog) return;
    auto *text_on = (uint32_t *)(uintptr_t)0x008F9488;
    auto *column = (uint8_t *)(uintptr_t)0x008F94CE;
    auto *style1 = (uint32_t *)(uintptr_t)0x0043B6C0, *style2 = (uint32_t *)(uintptr_t)0x0043B4A0;
    bool chinese = !g_exitmenu_english && *(const uint8_t *)(uintptr_t)0x00DB9890 == 0;
    uint32_t old_on = *text_on, old_s1 = *style1, old_s2 = *style2;
    uint8_t old_col = *column;
    *text_on = 1;
    *column = chinese ? 1 : 0;
    *style1 = *style2 = chinese ? 1 : 2;
    kBox(g_box);
    *text_on = old_on, *column = old_col, *style1 = old_s1, *style2 = old_s2;
}

// FUN_0017A400's entry (mov edx,[esp+8]; push ebx; push ebp): note the list, then the replaced instructions
__declspec(naked) void list_thunk()
{
    __asm {
        mov edx, dword ptr [esp + 8]
        mov dword ptr [g_exitmenu_list], edx
        push ebx
        push ebp
        jmp dword ptr [g_exitmenu_a400_resume]
    }
}

// FUN_0017A400's text lookup (call FUN_001E6AE0, eax = id, [esp+4] = column): the ninth main-menu row's text
__declspec(naked) void row_text_thunk()
{
    __asm {
        cmp eax, 0x0009000E // kRowMessage (inline assembly takes numbers, not constexpr names)
        jne game
        cmp dword ptr [g_exitmenu_list], 0x00C07600 // kMenuDesc
        jne game
        cmp dword ptr [g_exitmenu_english], 0
        jne english
        cmp byte ptr [esp + 4], 0
        jne english
        mov eax, offset kRowZh
        ret
    english:
        mov eax, offset kRowEn
        ret
    game:
        jmp dword ptr [g_exitmenu_6ae0]
    }
}

// FUN_001E6AE0's entry (push ebx; mov bl,[esp+8]; push edi): the box's text, else the replaced instructions
__declspec(naked) void message_thunk()
{
    __asm {
        cmp eax, 0x0004F0E1 // kBoxMessage
        je ours
        push ebx
        mov bl, byte ptr [esp + 8]
        push edi
        jmp dword ptr [g_exitmenu_6ae0_resume]
    ours:
        cmp dword ptr [g_exitmenu_english], 0
        jne english
        cmp byte ptr [esp + 4], 1
        jne english
        mov eax, offset kBoxZh
        ret
    english:
        mov eax, offset kBoxEn
        ret
    }
}

// The calls of the two row writers: the original, then the nine rows refreshed
__declspec(naked) void a9c0_thunk()
{
    __asm {
        call dword ptr [g_exitmenu_a9c0]
        pushad
        call exitmenu_sync
        popad
        ret
    }
}

__declspec(naked) void b570_thunk()
{
    __asm {
        call dword ptr [g_exitmenu_b570]
        pushad
        call exitmenu_sync
        popad
        ret
    }
}

// The menu's select (call FUN_0017B010, esi = the object): row 8 opens the box
__declspec(naked) void select_thunk()
{
    __asm {
        cmp dword ptr [esi + 0x90], 8
        jne game
        pushad
        call exitmenu_open
        popad
        ret
    game:
        jmp dword ptr [g_exitmenu_b010]
    }
}

// FUN_0017B3E0's entry (cmp dword ptr [ebx+0xB4],2; stdcall, the port): the box's frame while it is up
__declspec(naked) void input_thunk()
{
    __asm {
        cmp dword ptr [g_exitmenu_dialog], 0
        jne box
        cmp dword ptr [ebx + 0xB4], 2
        jmp dword ptr [g_exitmenu_b3e0_resume]
    box:
        pushad
        push dword ptr [esp + 0x24] // the port (above the 8 saved registers and the return address)
        call exitmenu_frame
        add esp, 4
        popad
        ret 4
    }
}

// The menu's frame (call FUN_0017AE80, edi = the object: the rows and their cursor): with the box up the rows are
// left out, as Ranking hides its list behind the box (the box is translucent), and the box is drawn instead
__declspec(naked) void draw_thunk()
{
    __asm {
        cmp dword ptr [g_exitmenu_dialog], 0
        jne box
        call dword ptr [g_exitmenu_ae80]
    box:
        pushad
        call exitmenu_draw
        popad
        ret
    }
}

struct Site {
    uint32_t addr;
    uint8_t expect[8];
    uint32_t len;
};
// The bytes each patch replaces in x2.1 (anything else: another build, nothing is patched)
const Site kSites[] = {
    {0x0017A400, {0x8B, 0x54, 0x24, 0x08, 0x53, 0x55}, 6},       // FUN_0017A400: mov edx,[esp+8]; push ebx; push ebp
    {0x0017A4EC, {0xE8, 0xEF, 0xC5, 0x06, 0x00}, 5},             // call FUN_001E6AE0 (a row's text)
    {0x001E6AE0, {0x53, 0x8A, 0x5C, 0x24, 0x08, 0x57}, 6},       // FUN_001E6AE0: push ebx; mov bl,[esp+8]; push edi
    {0x0017AB13, {0xE8, 0xA8, 0xFE, 0xFF, 0xFF}, 5},             // call FUN_0017A9C0
    {0x0017AB39, {0xE8, 0x82, 0xFE, 0xFF, 0xFF}, 5},             // call FUN_0017A9C0
    {0x0017AF71, {0xE8, 0xFA, 0x05, 0x00, 0x00}, 5},             // call FUN_0017B570
    {0x0017B554, {0xE8, 0xB7, 0xFA, 0xFF, 0xFF}, 5},             // call FUN_0017B010
    {0x0017B3E0, {0x83, 0xBB, 0xB4, 0x00, 0x00, 0x00, 0x02}, 7}, // FUN_0017B3E0: cmp dword ptr [ebx+0xB4],2
    {0x0017A44C, {0x66, 0x8B, 0x3C, 0x75, 0xBA, 0xA9, 0x45, 0x00}, 8}, // mov di,[esi*2+0x45A9BA] (row spacing)
    {0x0017B67D, {0xE8, 0xFE, 0xF7, 0xFF, 0xFF}, 5},             // call FUN_0017AE80 (the menu's drawing)
};

void put_branch(uint32_t at, uint8_t op, const void *to, uint32_t len)
{
    auto *p = (uint8_t *)(uintptr_t)at;
    DWORD old;
    VirtualProtect(p, len, PAGE_EXECUTE_READWRITE, &old);
    p[0] = op; // E8 call / E9 jmp
    int32_t rel = (int32_t)((uintptr_t)to - (at + 5));
    memcpy(p + 1, &rel, 4);
    for (uint32_t i = 5; i < len; i++) p[i] = 0x90;
    VirtualProtect(p, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, len);
}

} // namespace

void install_ngb_exitmenu()
{
    for (const Site &s : kSites)
        if (memcmp((const void *)(uintptr_t)s.addr, s.expect, s.len) != 0) {
            logf("exit menu: code at 0x%08x does not match x2.1; no EXIT GAME row", s.addr);
            return;
        }
    g_exitmenu_english = g_cfg.profile == &kProfileNgbUs;
    memcpy(g_spacing, (const void *)(uintptr_t)0x0045A9BA, 9 * sizeof(uint16_t)); // 0 rows .. 8 rows
    g_spacing[9] = 32;                                                             // nine rows (36 for eight, -4 a row)
    put_branch(0x0017A400, 0xE9, list_thunk, 6);
    put_branch(0x0017A4EC, 0xE8, row_text_thunk, 5);
    put_branch(0x001E6AE0, 0xE9, message_thunk, 6);
    put_branch(0x0017AB13, 0xE8, a9c0_thunk, 5);
    put_branch(0x0017AB39, 0xE8, a9c0_thunk, 5);
    put_branch(0x0017AF71, 0xE8, b570_thunk, 5);
    put_branch(0x0017B554, 0xE8, select_thunk, 5);
    put_branch(0x0017B3E0, 0xE9, input_thunk, 7);
    put_branch(0x0017B67D, 0xE8, draw_thunk, 5);
    DWORD old;
    auto *disp = (uint32_t *)(uintptr_t)0x0017A450;
    VirtualProtect(disp, 4, PAGE_EXECUTE_READWRITE, &old);
    *disp = (uint32_t)(uintptr_t)g_spacing;
    VirtualProtect(disp, 4, old, &old);
    logf("exit menu: EXIT GAME row installed (nine main-menu rows, the box from Ranking)");
}
