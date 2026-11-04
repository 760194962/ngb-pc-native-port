// x86 instructions that touch emulated device registers (net/nvnet.cpp, nv2a_display.cpp): the register pages are
// reserved inaccessible, the access faults, and the faulting instruction is decoded here and run on the device model.
// Forms: mov (all widths, moffs), movzx / movsx, mov imm, ALU m/r both ways, ALU m, imm, test; EFLAGS as the CPU sets them.
#include "mmio_decode.h"

#include <cstring>

namespace {


DWORD *gpr(CONTEXT *c, int r)
{
    switch (r) {
    case 0: return &c->Eax;
    case 1: return &c->Ecx;
    case 2: return &c->Edx;
    case 3: return &c->Ebx;
    case 4: return &c->Esp;
    case 5: return &c->Ebp;
    case 6: return &c->Esi;
    default: return &c->Edi;
    }
}

uint32_t get_reg(CONTEXT *c, int r, int size)
{
    if (size == 1) return r < 4 ? *gpr(c, r) & 0xFF : (*gpr(c, r - 4) >> 8) & 0xFF;
    uint32_t v = *gpr(c, r);
    return size == 2 ? v & 0xFFFF : v;
}

void set_reg(CONTEXT *c, int r, int size, uint32_t v)
{
    if (size == 1) {
        if (r < 4) *gpr(c, r) = (*gpr(c, r) & ~0xFFu) | (v & 0xFF);
        else *gpr(c, r - 4) = (*gpr(c, r - 4) & ~0xFF00u) | ((v & 0xFF) << 8);
    } else if (size == 2) {
        *gpr(c, r) = (*gpr(c, r) & ~0xFFFFu) | (v & 0xFFFF);
    } else {
        *gpr(c, r) = v;
    }
}

// kind: 0 add, 1 or, 2 adc, 3 sbb, 4 and, 5 sub, 6 xor, 7 cmp (x86 /digit order); sets EFLAGS
uint32_t alu(CONTEXT *c, int kind, uint32_t a, uint32_t b, int size)
{
    uint32_t mask = size == 1 ? 0xFF : size == 2 ? 0xFFFF : 0xFFFFFFFF, sign = size == 1 ? 0x80 : size == 2 ? 0x8000 : 0x80000000;
    a &= mask;
    b &= mask;
    uint32_t cf_in = c->EFlags & 1, r = 0, cf = 0, of = 0;
    switch (kind) {
    case 0: case 2: {
        uint64_t s = (uint64_t)a + b + (kind == 2 ? cf_in : 0);
        r = (uint32_t)s & mask;
        cf = s > mask;
        of = ((a ^ r) & (b ^ r) & sign) != 0;
        break;
    }
    case 3: case 5: case 7: {
        uint64_t sub = (uint64_t)b + (kind == 3 ? cf_in : 0);
        r = (uint32_t)(a - sub) & mask;
        cf = a < sub;
        of = ((a ^ b) & (a ^ r) & sign) != 0;
        break;
    }
    case 1: r = a | b; break;
    case 4: r = a & b; break;
    case 6: r = a ^ b; break;
    }
    uint32_t pf = 1;
    for (int i = 0; i < 8; i++) pf ^= (r >> i) & 1;
    uint32_t fl = (cf ? 0x1 : 0) | (pf ? 0x4 : 0) | (((a ^ b ^ r) & 0x10) && kind != 1 && kind != 4 && kind != 6 ? 0x10 : 0) |
                  (r == 0 ? 0x40 : 0) | (r & sign ? 0x80 : 0) | (of ? 0x800 : 0);
    c->EFlags = (c->EFlags & ~0x8D5u) | fl;
    return r;
}

// Steps past a ModRM memory operand (the address itself comes from the fault); false for a register operand
bool skip_modrm(const uint8_t *&p, int &reg_field)
{
    uint8_t m = *p++;
    int mod = m >> 6, rm = m & 7;
    reg_field = (m >> 3) & 7;
    if (mod == 3) return false;
    if (rm == 4) {
        uint8_t sib = *p++;
        if (mod == 0 && (sib & 7) == 5) p += 4;
    } else if (mod == 0 && rm == 5) {
        p += 4;
    }
    if (mod == 1) p += 1;
    else if (mod == 2) p += 4;
    return true;
}

} // namespace

bool mmio_decode(CONTEXT *c, uint32_t off, const MmioOps &ops)
{
    const uint8_t *p = (const uint8_t *)(uintptr_t)c->Eip;
    bool o16 = false;
    while (*p == 0x66 || *p == 0x3E || *p == 0x26 || *p == 0x2E || *p == 0x36) o16 |= *p++ == 0x66;
    int wsize = o16 ? 2 : 4, regf = 0;
    uint8_t op = *p++;
    auto finish = [&](const uint8_t *end) {
        c->Eip = (DWORD)(uintptr_t)end;
        return true;
    };
    auto imm = [&](int size) -> uint32_t {
        uint32_t v = size == 1 ? *p : size == 2 ? *(const uint16_t *)p : *(const uint32_t *)p;
        p += size;
        return v;
    };
    if (op == 0x0F) {
        uint8_t op2 = *p++;
        if (op2 == 0xB6 || op2 == 0xB7 || op2 == 0xBE || op2 == 0xBF) { // movzx / movsx r32, m8 / m16
            if (!skip_modrm(p, regf)) return false;
            int ms = (op2 & 1) ? 2 : 1;
            uint32_t v = ops.read(off, ms);
            if (op2 == 0xBE) v = (uint32_t)(int32_t)(int8_t)v;
            if (op2 == 0xBF) v = (uint32_t)(int32_t)(int16_t)v;
            set_reg(c, regf, wsize, v);
            return finish(p);
        }
        if (op2 == 0xBA || op2 == 0xA3 || op2 == 0xAB || op2 == 0xB3 || op2 == 0xBB) {
            // BT / BTS / BTR / BTC with a memory operand (the USB controller's port status is tested this way by
            // the Insignia Setup Assistant). 0F BA has an immediate bit number; the others take it from a register.
            // 0F BA's operation is its ModRM reg field (/4 BT, /5 BTS, /6 BTR, /7 BTC); the others name it in the
            // opcode. A register's bit number may reach past the operand (a bit string): it moves the address.
            if (!skip_modrm(p, regf)) return false;
            int s = o16 ? 2 : 4;
            const uint32_t nbits = (uint32_t)s * 8;
            int kind; // 0 BT, 1 BTS, 2 BTR, 3 BTC
            uint32_t bit, at = off;
            if (op2 == 0xBA) {
                if (regf < 4) return false;
                kind = regf - 4;
                bit = *p++ & (nbits - 1);
            } else {
                kind = op2 == 0xA3 ? 0 : op2 == 0xAB ? 1 : op2 == 0xB3 ? 2 : 3;
                int32_t b = (int32_t)get_reg(c, regf, wsize);
                if (o16) b = (int16_t)b;
                at = off + (uint32_t)((b >> (o16 ? 4 : 5)) * s); // arithmetic shift: negative offsets go back
                bit = (uint32_t)b & (nbits - 1);
            }
            uint32_t v = ops.read(at, s);
            const bool cf = ((v >> bit) & 1) != 0;
            if (kind == 1) v |= 1u << bit;
            else if (kind == 2) v &= ~(1u << bit);
            else if (kind == 3) v ^= 1u << bit;
            c->EFlags = (c->EFlags & ~1u) | (cf ? 1u : 0u);
            if (kind) ops.write(at, v, s);
            return finish(p);
        }
        return false;
    }
    switch (op) {
    case 0xA0: case 0xA1: { // mov al/eax, [moffs]
        int s = op == 0xA0 ? 1 : wsize;
        p += 4;
        set_reg(c, 0, s, ops.read(off, s));
        return finish(p);
    }
    case 0xA2: case 0xA3: { // mov [moffs], al/eax
        int s = op == 0xA2 ? 1 : wsize;
        p += 4;
        ops.write(off, get_reg(c, 0, s), s);
        return finish(p);
    }
    case 0x88: case 0x89: case 0x8A: case 0x8B: {
        int s = (op & 1) ? wsize : 1;
        if (!skip_modrm(p, regf)) return false;
        if (op & 2) set_reg(c, regf, s, ops.read(off, s));
        else ops.write(off, get_reg(c, regf, s), s);
        return finish(p);
    }
    case 0xC6: case 0xC7: {
        int s = op == 0xC6 ? 1 : wsize;
        if (!skip_modrm(p, regf) || regf) return false;
        ops.write(off, imm(s), s);
        return finish(p);
    }
    case 0x80: case 0x81: case 0x83: { // ALU m, imm
        int s = op == 0x80 ? 1 : wsize;
        if (!skip_modrm(p, regf)) return false;
        uint32_t b = op == 0x83 ? (uint32_t)(int32_t)(int8_t)imm(1) : imm(s);
        uint32_t r = alu(c, regf, ops.read(off, s), b, s);
        if (regf != 7) ops.write(off, r, s);
        return finish(p);
    }
    case 0x84: case 0x85: { // test m, r
        int s = op == 0x84 ? 1 : wsize;
        if (!skip_modrm(p, regf)) return false;
        alu(c, 4, ops.read(off, s), get_reg(c, regf, s), s);
        return finish(p);
    }
    case 0xF6: case 0xF7: { // test m, imm
        int s = op == 0xF6 ? 1 : wsize;
        if (!skip_modrm(p, regf) || regf) return false;
        alu(c, 4, ops.read(off, s), imm(s), s);
        return finish(p);
    }
    default:
        if (op < 0x40 && (op & 7) < 4) { // ALU r/m, r and r, r/m (add or adc sbb and sub xor cmp)
            int kind = op >> 3, s = (op & 1) ? wsize : 1;
            if (!skip_modrm(p, regf)) return false;
            uint32_t m = ops.read(off, s), r_ = get_reg(c, regf, s);
            if (op & 2) {
                uint32_t r = alu(c, kind, r_, m, s);
                if (kind != 7) set_reg(c, regf, s, r);
            } else {
                uint32_t r = alu(c, kind, m, r_, s);
                if (kind != 7) ops.write(off, r, s);
            }
            return finish(p);
        }
        return false;
    }
}

