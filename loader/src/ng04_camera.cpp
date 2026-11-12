// Ninja Gaiden (2004) title update 1.06.04: the right-stick camera that came with the Hurricane Packs.
//
// The camera routine (work/ng04, 2026-11-12; x2.1 has the same code at 0x47A09-0x47A9A):
// - 0x45F68 mov al, [0xE8F05C]; test; je: the right stick moves the camera only while that byte is 1. R3 flips it
//   (0x3232F-0x3240D, messages 0xFD on / 0xFC off); it is set to 0 at boot (0x2D8AA8) and again at 0x162C6A (each
//   player's records reset). ng04_cam = 1 sets it to 1 at both instead, as Ninja Gaiden Black x2.1 does at its own two
//   (0x2EC5C3 and 0x16762A). R3 still switches it.
// - 0x45F91-0x45FE6: two words from the pad record (0xDFB5C0 + port * 0x78, +0x2C and +0x2E) scaled by 1/127 and
//   curved into [esp+0x28] / [esp+0x2C]. 0x46084 multiplies the second by 0.0384 (the turn) and the first by 0.1 (the
//   height): the first is the stick's up / down, the second left / right.
// - 0x45FE8 mov al, [ecx+0x40A] (6 bytes), right after the loop: a call here flips the sign of either value
//   (ng04_cam_invert_y / _x), as x2.1's own call at 0x47A92 does for both. The flag is read every frame, so it can
//   change in play.
// - The camera is the Hurricane Packs' only where [0x5681D8] (a stage's mode, read from the stage table at
//   0x2D8931) is above 1: the Master Ninja Tournament's stages. The story's are 0 / 1, and there the right stick only
//   looks around in first person. Six branches of the camera code test it; Ninja Gaiden Black (made from this code)
//   has none of them (x2.1 0x479B0, 0x4765A). ng04_cam takes the Hurricane side of each, so the story gets the same
//   camera; the mode's other users (combat, sounds) are left as they are:
//     0x45F2C jbe (the stick camera, x2.1 0x479B0 has no test)   0x45162 jbe   0x45BC9 jbe ([edi+0x8F9] = 1)
//     0x474AE ja (the distance)   0x322D0 ja (R3 in third person)   0x323B4 ja (R3 in first person)
// Only the title update has this camera: disc 1.02.04 has no such loop.
#include "build_profile.h"
#include "common.h"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <vector>

extern "C" volatile uint32_t g_ng04c_invert = 0; // bit 0: up / down, bit 1: left / right

namespace {

constexpr uint32_t kCamVar = 0x00E8F05C; // 1: the right stick moves the camera
constexpr uint32_t kBoot = 0x002D8AA8;   // mov [kCamVar], bl and three mov byte [0x89E0FD..FF], 1 (27 bytes)
constexpr uint32_t kReset = 0x00162C6A;  // mov [kCamVar], bl, al = 1 (6 bytes)
constexpr uint32_t kStick = 0x00045FE8;  // mov al, [ecx+0x40A] (6 bytes)

// After the stick loop: [esp+0x28] up / down, [esp+0x2C] left / right (+4 here for the return address)
__declspec(naked) void stick_thunk()
{
    __asm {
        test byte ptr [g_ng04c_invert], 1
        jz no_v
        xor dword ptr [esp + 0x2C], 0x80000000
    no_v:
        test byte ptr [g_ng04c_invert], 2
        jz no_h
        xor dword ptr [esp + 0x30], 0x80000000
    no_h:
        mov al, byte ptr [ecx + 0x40A]
        ret
    }
}

bool expect(uint32_t at, const std::vector<uint8_t> &bytes)
{
    const uint8_t *p = (const uint8_t *)(uintptr_t)at;
    for (uint8_t b : bytes)
        if (*p++ != b) {
            logf("ng04 camera: code at 0x%08x is not as expected; camera options off", at);
            return false;
        }
    return true;
}

void put(uint32_t at, const uint8_t *src, uint32_t len)
{
    auto *p = (uint8_t *)(uintptr_t)at;
    DWORD old;
    VirtualProtect(p, len, PAGE_EXECUTE_READWRITE, &old);
    memcpy(p, src, len);
    VirtualProtect(p, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, len);
}


// The six mode tests (above): each after its compare of [0x5681D8], checked with it
struct Gate {
    uint32_t test; // the compare
    std::vector<uint8_t> test_bytes;
    uint32_t at; // the branch
    std::vector<uint8_t> was, now;
};
const Gate kGates[] = {
    {0x45F20, {0x3C, 0x01}, 0x45F2C, {0x0F, 0x86, 0x1E, 0x0F, 0x00, 0x00}, {0x90, 0x90, 0x90, 0x90, 0x90, 0x90}},
    {0x4515B, {0x80, 0x3D, 0xD8, 0x81, 0x56, 0x00, 0x01}, 0x45162, {0x0F, 0x86, 0x8D, 0x02, 0x00, 0x00},
     {0x90, 0x90, 0x90, 0x90, 0x90, 0x90}},
    {0x45BC2, {0x80, 0x3D, 0xD8, 0x81, 0x56, 0x00, 0x02}, 0x45BC9, {0x76, 0x07}, {0x90, 0x90}},
    {0x47490, {0xA0, 0xD8, 0x81, 0x56, 0x00, 0x3C, 0x01}, 0x474AE, {0x77, 0x03}, {0xEB, 0x03}}, // x87 only between
    {0x322C7, {0x38, 0x1D, 0xD8, 0x81, 0x56, 0x00, 0x0F, 0xB6, 0xC0}, 0x322D0, {0x77, 0x5D}, {0xEB, 0x5D}},
    {0x323AE, {0x38, 0x1D, 0xD8, 0x81, 0x56, 0x00}, 0x323B4, {0x77, 0x2E}, {0xEB, 0x2E}},
};

// The boot and reset stores: the game's, and with the camera on at start. The same four stores without a register:
// mov word [0x89E0FD], 0x0101; mov byte [0x89E0FF], 1; mov byte [kCamVar], 1; four nops (27 bytes)
const std::vector<uint8_t> kBootWas = {0x88, 0x1D, 0x5C, 0xF0, 0xE8, 0x00, 0xC6, 0x05, 0xFD, 0xE0, 0x89, 0x00, 0x01, 0xC6,
                                       0x05, 0xFE, 0xE0, 0x89, 0x00, 0x01, 0xC6, 0x05, 0xFF, 0xE0, 0x89, 0x00, 0x01};
const std::vector<uint8_t> kBootNow = {0x66, 0xC7, 0x05, 0xFD, 0xE0, 0x89, 0x00, 0x01, 0x01, 0xC6, 0x05, 0xFF, 0xE0, 0x89,
                                       0x00, 0x01, 0xC6, 0x05, 0x5C, 0xF0, 0xE8, 0x00, 0x01, 0x90, 0x90, 0x90, 0x90};
const std::vector<uint8_t> kResetWas = {0x88, 0x1D, 0x5C, 0xF0, 0xE8, 0x00};
const std::vector<uint8_t> kResetNow = {0xA2, 0x5C, 0xF0, 0xE8, 0x00, 0x90}; // mov [kCamVar], al (al = 1); nop

bool g_installed, g_gates_ok;

void put(uint32_t at, const std::vector<uint8_t> &b) { put(at, b.data(), (uint32_t)b.size()); }

} // namespace

void ng04_camera_set_invert(int bits) { g_ng04c_invert = (uint32_t)bits & 3; }

// g_cfg's ng04_cam / ng04_cam_global / invert into the game's code; at start and from the menu (any time: each is a
// branch or a store the game reaches on its own thread, and a byte-sized write of an opcode is seen whole)
void ng04_camera_apply()
{
    if (!g_installed) return;
    const bool global = g_cfg.ng04_cam_global && g_gates_ok;
    for (const Gate &g : kGates) put(g.at, global ? g.now : g.was);
    put(kBoot, g_cfg.ng04_cam ? kBootNow : kBootWas);
    put(kReset, g_cfg.ng04_cam ? kResetNow : kResetWas);
    ng04_camera_set_invert((g_cfg.ng04_cam_invert_y ? 1 : 0) | (g_cfg.ng04_cam_invert_x ? 2 : 0));
    logf("ng04 camera: third person %s, %s, invert up/down %d, left/right %d", g_cfg.ng04_cam ? "on at start" : "after R3",
         global ? "in every stage" : "in the Hurricane Packs' stages only (as the game)", g_cfg.ng04_cam_invert_y ? 1 : 0,
         g_cfg.ng04_cam_invert_x ? 1 : 0);
}

// Changed in play (the menu): the camera's state follows at once, not only at the next stage
void ng04_camera_set_on(bool on)
{
    g_cfg.ng04_cam = on;
    ng04_camera_apply();
    if (g_installed) *(volatile uint8_t *)(uintptr_t)kCamVar = on ? 1 : 0;
}

void install_ng04_camera()
{
    if (g_cfg.profile != &kProfileNg04Tu) return;
    if (!expect(kBoot, kBootWas) || !expect(kReset - 0x15, {0xB0, 0x01, 0x5E}) || // mov al, 1 before the stores
        !expect(kReset, kResetWas) || !expect(kStick, {0x8A, 0x81, 0x0A, 0x04, 0x00, 0x00}))
        return;
    g_gates_ok = true;
    for (const Gate &g : kGates)
        if (!expect(g.test, g.test_bytes) || !expect(g.at, g.was)) g_gates_ok = false;

    uint8_t call[6] = {0xE8, 0, 0, 0, 0, 0x90};
    int32_t rel = (int32_t)((uintptr_t)&stick_thunk - (kStick + 5));
    memcpy(call + 1, &rel, 4);
    put(kStick, call, sizeof call);
    g_installed = true;
    ng04_camera_apply();
}
