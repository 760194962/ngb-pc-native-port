// The kernel's crypto exports that only Xbox Live reaches (Ninja Gaiden Black's XONLINE / XNET, project/docs/09): RC4
// (Kerberos), modular exponentiation (the Diffie-Hellman key exchange with the security gateway, RSA), DES / 3DES-CBC
// (the gateway's packet encryption) and the PKCS #1 signature check. Formats follow the kernel's: little-endian big
// numbers of dwN dwords, the RSA1 public key blob of XePublicKeyData, RC4 and DES key tables private to these functions
// (the caller only allocates them: 258 bytes for RC4, 128 per DES key).
#include "kernel/kexports.h"
#include "sha1.h"

#include <cstring>
#include <vector>

namespace {

// --- big numbers: little-endian uint32 limbs, n of them ---

using Limbs = std::vector<uint32_t>;

bool geq(const uint32_t *a, const uint32_t *b, int n)
{
    for (int i = n - 1; i >= 0; i--)
        if (a[i] != b[i]) return a[i] > b[i];
    return true;
}

void sub_in_place(uint32_t *a, const uint32_t *b, int n)
{
    uint64_t borrow = 0;
    for (int i = 0; i < n; i++) {
        uint64_t d = (uint64_t)a[i] - b[i] - borrow;
        a[i] = (uint32_t)d;
        borrow = (d >> 32) & 1;
    }
}

// Montgomery product (CIOS): out = a * b / R mod m, R = 2^(32n), m odd, a * b < m * R
void mont_mul(uint32_t *out, const uint32_t *a, const uint32_t *b, const uint32_t *m, uint32_t m0inv, int n)
{
    std::vector<uint32_t> t(n + 2, 0);
    for (int i = 0; i < n; i++) {
        uint64_t c = 0;
        for (int j = 0; j < n; j++) {
            uint64_t s = (uint64_t)t[j] + (uint64_t)a[j] * b[i] + c;
            t[j] = (uint32_t)s;
            c = s >> 32;
        }
        uint64_t s = (uint64_t)t[n] + c;
        t[n] = (uint32_t)s;
        t[n + 1] = (uint32_t)(s >> 32);
        uint32_t q = t[0] * m0inv;
        c = ((uint64_t)t[0] + (uint64_t)q * m[0]) >> 32;
        for (int j = 1; j < n; j++) {
            s = (uint64_t)t[j] + (uint64_t)q * m[j] + c;
            t[j - 1] = (uint32_t)s;
            c = s >> 32;
        }
        s = (uint64_t)t[n] + c;
        t[n - 1] = (uint32_t)s;
        t[n] = t[n + 1] + (uint32_t)(s >> 32);
    }
    if (t[n] || geq(t.data(), m, n)) sub_in_place(t.data(), m, n);
    memcpy(out, t.data(), n * 4);
}

// out = b ^ e mod m (all n limbs); false if m is even or zero
bool mod_exp(uint32_t *out, const uint32_t *b, const uint32_t *e, const uint32_t *m, int n)
{
    while (n > 1 && !m[n - 1]) n--; // significant limbs only (the caller's arrays may be zero-padded)
    if (!(m[0] & 1)) return false;
    uint32_t inv = 1; // m[0]^-1 mod 2^32 by Newton's iteration
    for (int i = 0; i < 5; i++) inv *= 2 - m[0] * inv;
    uint32_t m0inv = (uint32_t)(0 - inv);
    // R^2 mod m by doubling 1 (64n times)
    Limbs r2(n + 1, 0);
    r2[0] = 1;
    for (int i = 0; i < 64 * n; i++) {
        uint32_t carry = 0;
        for (int j = 0; j < n; j++) {
            uint32_t v = r2[j];
            r2[j] = (v << 1) | carry;
            carry = v >> 31;
        }
        if (carry || geq(r2.data(), m, n)) sub_in_place(r2.data(), m, n);
    }
    Limbs base(b, b + n), x(n), one(n, 0);
    one[0] = 1;
    mont_mul(base.data(), base.data(), r2.data(), m, m0inv, n); // b * R mod m (b < R, R^2 mod m < m)
    mont_mul(x.data(), one.data(), r2.data(), m, m0inv, n);     // 1 * R mod m
    for (int i = n * 32 - 1; i >= 0; i--) {
        mont_mul(x.data(), x.data(), x.data(), m, m0inv, n);
        if (e[i / 32] >> (i % 32) & 1) mont_mul(x.data(), x.data(), base.data(), m, m0inv, n);
    }
    mont_mul(x.data(), x.data(), one.data(), m, m0inv, n);
    memcpy(out, x.data(), n * 4);
    return true;
}

// --- DES ---

const uint8_t kIP[64] = {58, 50, 42, 34, 26, 18, 10, 2, 60, 52, 44, 36, 28, 20, 12, 4, 62, 54, 46, 38, 30, 22, 14, 6,
                         64, 56, 48, 40, 32, 24, 16, 8, 57, 49, 41, 33, 25, 17, 9,  1, 59, 51, 43, 35, 27, 19, 11, 3,
                         61, 53, 45, 37, 29, 21, 13, 5, 63, 55, 47, 39, 31, 23, 15, 7};
const uint8_t kFP[64] = {40, 8, 48, 16, 56, 24, 64, 32, 39, 7, 47, 15, 55, 23, 63, 31, 38, 6, 46, 14, 54, 22, 62, 30,
                         37, 5, 45, 13, 53, 21, 61, 29, 36, 4, 44, 12, 52, 20, 60, 28, 35, 3, 43, 11, 51, 19, 59, 27,
                         34, 2, 42, 10, 50, 18, 58, 26, 33, 1, 41, 9,  49, 17, 57, 25};
const uint8_t kE[48] = {32, 1,  2,  3,  4,  5,  4,  5,  6,  7,  8,  9,  8,  9,  10, 11, 12, 13, 12, 13, 14, 15, 16, 17,
                        16, 17, 18, 19, 20, 21, 20, 21, 22, 23, 24, 25, 24, 25, 26, 27, 28, 29, 28, 29, 30, 31, 32, 1};
const uint8_t kP[32] = {16, 7, 20, 21, 29, 12, 28, 17, 1, 15, 23, 26, 5, 18, 31, 10,
                        2,  8, 24, 14, 32, 27, 3,  9,  19, 13, 30, 6, 22, 11, 4,  25};
const uint8_t kPC1[56] = {57, 49, 41, 33, 25, 17, 9,  1,  58, 50, 42, 34, 26, 18, 10, 2,  59, 51, 43,
                          35, 27, 19, 11, 3,  60, 52, 44, 36, 63, 55, 47, 39, 31, 23, 15, 7,  62, 54,
                          46, 38, 30, 22, 14, 6,  61, 53, 45, 37, 29, 21, 13, 5,  28, 20, 12, 4};
const uint8_t kPC2[48] = {14, 17, 11, 24, 1,  5,  3,  28, 15, 6,  21, 10, 23, 19, 12, 4,
                          26, 8,  16, 7,  27, 20, 13, 2,  41, 52, 31, 37, 47, 55, 30, 40,
                          51, 45, 33, 48, 44, 49, 39, 56, 34, 53, 46, 42, 50, 36, 29, 32};
const uint8_t kShifts[16] = {1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1};
const uint8_t kS[8][64] = {
    {14, 4,  13, 1, 2,  15, 11, 8,  3,  10, 6,  12, 5,  9,  0, 7, 0, 15, 7,  4,  14, 2,  13, 1,  10, 6, 12, 11, 9,  5,  3,  8,
     4,  1,  14, 8, 13, 6,  2,  11, 15, 12, 9,  7,  3,  10, 5, 0, 15, 12, 8,  2,  4,  9,  1,  7,  5,  11, 3,  14, 10, 0, 6,  13},
    {15, 1,  8,  14, 6,  11, 3,  4,  9,  7, 2,  13, 12, 0, 5,  10, 3,  13, 4,  7,  15, 2,  8,  14, 12, 0, 1,  10, 6,  9,  11, 5,
     0,  14, 7,  11, 10, 4,  13, 1,  5,  8, 12, 6,  9,  3, 2,  15, 13, 8,  10, 1,  3,  15, 4,  2,  11, 6, 7,  12, 0,  5,  14, 9},
    {10, 0,  9,  14, 6, 3,  15, 5,  1,  13, 12, 7,  11, 4,  2,  8,  13, 7,  0,  9,  3,  4, 6,  10, 2,  8,  5,  14, 12, 11, 15, 1,
     13, 6,  4,  9,  8, 15, 3,  0,  11, 1,  2,  12, 5,  10, 14, 7,  1,  10, 13, 0,  6,  9, 8,  7,  4,  15, 14, 3,  11, 5,  2,  12},
    {7,  13, 14, 3, 0,  6,  9,  10, 1,  2, 8, 5,  11, 12, 4,  15, 13, 8,  11, 5,  6,  15, 0,  3,  4, 7,  2,  12, 1,  10, 14, 9,
     10, 6,  9,  0, 12, 11, 7,  13, 15, 1, 3, 14, 5,  2,  8,  4,  3,  15, 0,  6,  10, 1,  13, 8,  9, 4,  5,  11, 12, 7,  2,  14},
    {2,  12, 4,  1,  7,  10, 11, 6,  8,  5,  3,  15, 13, 0, 14, 9,  14, 11, 2,  12, 4,  7,  13, 1,  5,  0,  15, 10, 3,  9, 8,  6,
     4,  2,  1,  11, 10, 13, 7,  8,  15, 9,  12, 5,  6,  3, 0,  14, 11, 8,  12, 7,  1,  14, 2,  13, 6,  15, 0,  9,  10, 4, 5,  3},
    {12, 1,  10, 15, 9, 2,  6,  8,  0,  13, 3,  4,  14, 7,  5,  11, 10, 15, 4,  2,  7,  12, 9,  5,  6,  1,  13, 14, 0,  11, 3,  8,
     9,  14, 15, 5,  2, 8,  12, 3,  7,  0,  4,  10, 1,  13, 11, 6,  4,  3,  2,  12, 9,  5,  15, 10, 11, 14, 1,  7,  6,  0,  8,  13},
    {4,  11, 2,  14, 15, 0, 8,  13, 3,  12, 9, 7,  5,  10, 6, 1,  13, 0,  11, 7,  4,  9,  1,  10, 14, 3,  5,  12, 2,  15, 8,  6,
     1,  4,  11, 13, 12, 3, 7,  14, 10, 15, 6, 8,  0,  5,  9, 2,  6,  11, 13, 8,  1,  4,  10, 7,  9,  5,  0,  15, 14, 2,  3,  12},
    {13, 2,  8,  4, 6,  15, 11, 1,  10, 9,  3,  14, 5,  0,  12, 7,  1,  15, 13, 8,  10, 3,  7,  4,  12, 5,  6,  11, 0,  14, 9,  2,
     7,  11, 4,  1, 9,  12, 14, 2,  0,  6,  10, 13, 15, 3,  5,  8,  2,  1,  14, 7,  4,  10, 8,  13, 15, 12, 9,  0,  3,  5,  6,  11}};

// bit i of the result (1 = most significant of `outbits`) is bit table[i] of x (1 = most significant of `inbits`)
uint64_t permute(uint64_t x, const uint8_t *table, int outbits, int inbits)
{
    uint64_t r = 0;
    for (int i = 0; i < outbits; i++) r = (r << 1) | ((x >> (inbits - table[i])) & 1);
    return r;
}

struct DesKey {
    uint64_t sub[16]; // 48-bit round keys
};
static_assert(sizeof(DesKey) == 128, "one DES key table is 128 bytes (DES3: three)");

void des_schedule(DesKey *k, const uint8_t key[8])
{
    uint64_t kk = 0;
    for (int i = 0; i < 8; i++) kk = (kk << 8) | key[i];
    uint64_t cd = permute(kk, kPC1, 56, 64);
    uint32_t c = (uint32_t)(cd >> 28) & 0xFFFFFFF, d = (uint32_t)cd & 0xFFFFFFF;
    for (int r = 0; r < 16; r++) {
        for (int s = 0; s < kShifts[r]; s++) {
            c = ((c << 1) | (c >> 27)) & 0xFFFFFFF;
            d = ((d << 1) | (d >> 27)) & 0xFFFFFFF;
        }
        k->sub[r] = permute((uint64_t)c << 28 | d, kPC2, 48, 56);
    }
}

uint32_t des_f(uint32_t r, uint64_t sub)
{
    uint64_t x = permute(r, kE, 48, 32) ^ sub;
    uint32_t out = 0;
    for (int i = 0; i < 8; i++) {
        int six = (int)(x >> (42 - 6 * i)) & 63;
        int row = ((six & 32) >> 4) | (six & 1), col = (six >> 1) & 15;
        out = (out << 4) | kS[i][row * 16 + col];
    }
    return (uint32_t)permute(out, kP, 32, 32);
}

uint64_t des_block(const DesKey *k, uint64_t block, bool encrypt)
{
    uint64_t x = permute(block, kIP, 64, 64);
    uint32_t l = (uint32_t)(x >> 32), r = (uint32_t)x;
    for (int i = 0; i < 16; i++) {
        uint32_t t = r;
        r = l ^ des_f(r, k->sub[encrypt ? i : 15 - i]);
        l = t;
    }
    return permute((uint64_t)r << 32 | l, kFP, 64, 64);
}

uint64_t load_be64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
    return v;
}

void store_be64(uint8_t *p, uint64_t v)
{
    for (int i = 7; i >= 0; i--, v >>= 8) p[i] = (uint8_t)v;
}

} // namespace

// Known-answer checks, run once when online play starts (net/online.cpp): DES (FIPS 81 example), 3DES with three equal
// keys = DES, RC4 (the "Key" / "Plaintext" vector) and a small modular exponentiation.
bool kcrypto_selftest()
{
    const uint8_t key[8] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};
    DesKey k;
    des_schedule(&k, key);
    uint64_t c = des_block(&k, 0x4E6F772069732074ull, true); // "Now is t"
    bool des_ok = c == 0x3FA40E8A984D4815ull && des_block(&k, c, false) == 0x4E6F772069732074ull;
    uint8_t rc4[258];
    const uint8_t pt[9] = {'P', 'l', 'a', 'i', 'n', 't', 'e', 'x', 't'}, expect[9] = {0xBB, 0xF3, 0x16, 0xE8, 0xD9, 0x40, 0xAF, 0x0A, 0xD3};
    uint8_t buf[9];
    memcpy(buf, pt, 9);
    xk::XcRC4Key(rc4, 3, (const uint8_t *)"Key");
    xk::XcRC4Crypt(rc4, 9, buf);
    bool rc4_ok = !memcmp(buf, expect, 9);
    uint32_t b = 4, e = 13, m = 497, r = 0; // 4^13 mod 497 = 445
    xk::XcModExp(&r, &b, &e, &m, 1);
    bool exp_ok = r == 445;
    uint32_t b2[2] = {0x12345678, 0x9}, e2[2] = {65537, 0}, m2[2] = {0xFFFFFFFB, 0xFFFFFFFF}, r2[2] = {}; // vs 128-bit math
    xk::XcModExp(r2, b2, e2, m2, 2);
    unsigned __int64 bb = 0x912345678ull, mm = 0xFFFFFFFFFFFFFFFBull, acc = 1, base = bb % mm;
    auto mulmod = [](unsigned __int64 a, unsigned __int64 x, unsigned __int64 mod) {
        unsigned __int64 res = 0;
        a %= mod;
        while (x) {
            if (x & 1) res = (res >= mod - a) ? res - (mod - a) : res + a;
            a = (a >= mod - a) ? a - (mod - a) : a + a;
            x >>= 1;
        }
        return res;
    };
    for (unsigned ex = 65537; ex; ex >>= 1) {
        if (ex & 1) acc = mulmod(acc, base, mm);
        base = mulmod(base, base, mm);
    }
    exp_ok = exp_ok && (((unsigned __int64)r2[1] << 32) | r2[0]) == acc;
    logf("[crypto] self-test: DES %s, RC4 %s, ModExp %s", des_ok ? "ok" : "FAILED", rc4_ok ? "ok" : "FAILED", exp_ok ? "ok" : "FAILED");
    return des_ok && rc4_ok && exp_ok;
}

namespace xk {

void __stdcall XcRC4Key(uint8_t *KeyStruct, uint32_t KeyLength, const uint8_t *Key)
{
    KTRACE("%p, %u, %p", KeyStruct, KeyLength, Key);
    uint8_t *s = KeyStruct;
    for (int i = 0; i < 256; i++) s[i] = (uint8_t)i;
    uint8_t j = 0;
    for (int i = 0; i < 256; i++) {
        j = (uint8_t)(j + s[i] + Key[KeyLength ? i % KeyLength : 0]);
        uint8_t t = s[i];
        s[i] = s[j];
        s[j] = t;
    }
    s[256] = s[257] = 0; // i, j
}

void __stdcall XcRC4Crypt(uint8_t *KeyStruct, uint32_t InputLength, uint8_t *Input)
{
    KTRACE("%p, %u, %p", KeyStruct, InputLength, Input);
    uint8_t *s = KeyStruct, i = s[256], j = s[257];
    for (uint32_t n = 0; n < InputLength; n++) {
        i++;
        j = (uint8_t)(j + s[i]);
        uint8_t t = s[i];
        s[i] = s[j];
        s[j] = t;
        Input[n] ^= s[(uint8_t)(s[i] + s[j])];
    }
    s[256] = i;
    s[257] = j;
}

// A = B ^ C mod D, each dwN dwords little-endian
uint32_t __stdcall XcModExp(uint32_t *A, const uint32_t *B, const uint32_t *C, const uint32_t *D, uint32_t N)
{
    KTRACE("%p, %p, %p, %p, %u", A, B, C, D, N);
    if (!N) return 0;
    Limbs out(N, 0);
    if (!mod_exp(out.data(), B, C, D, (int)N)) {
        KNOTE_ONCE("XcModExp: even modulus (%u dwords); result zero", N);
        memset(A, 0, N * 4);
        return 0;
    }
    memcpy(A, out.data(), N * 4);
    return 1;
}

// PKCS #1 v1.5 signature (little-endian, as the kernel keeps it) of a SHA-1 digest, checked with an RSA1 blob:
// "RSA1", modulus bytes (incl. 8 bytes of padding), bits, max bytes, public exponent, modulus (little-endian).
BOOLEAN __stdcall XcVerifyPKCS1Signature(const uint8_t *Signature, const uint8_t *PublicKey, const uint8_t *Digest)
{
    KTRACE("%p, %p, %p", Signature, PublicKey, Digest);
    if (memcmp(PublicKey, "RSA1", 4)) {
        KNOTE_ONCE("XcVerifyPKCS1Signature: no RSA1 public key (XePublicKeyData is empty here): signature not accepted");
        return FALSE;
    }
    uint32_t mod_bytes, bits, exp;
    memcpy(&mod_bytes, PublicKey + 4, 4);
    memcpy(&bits, PublicKey + 8, 4);
    memcpy(&exp, PublicKey + 16, 4);
    uint32_t len = bits / 8, n = (mod_bytes + 3) / 4;
    if (len < 64 || len > mod_bytes || n > 128) return FALSE;
    Limbs sig(n, 0), m(n, 0), e(n, 0), r(n, 0);
    memcpy(sig.data(), Signature, len);
    memcpy(m.data(), PublicKey + 20, mod_bytes);
    e[0] = exp;
    if (!mod_exp(r.data(), sig.data(), e.data(), m.data(), (int)n)) return FALSE;
    const uint8_t *le = (const uint8_t *)r.data(); // little-endian: le[len-1] is the most significant byte
    // big-endian 00 01 FF..FF 00 [DigestInfo] digest; the digest sits at the low end, its bytes reversed
    for (int i = 0; i < 20; i++)
        if (le[i] != Digest[19 - i]) return FALSE;
    if (le[len - 1] != 0x00 || le[len - 2] != 0x01) return FALSE;
    return TRUE;
}

// The length of an RSA1 public key blob's modulus in bytes, as the blob gives it (its second field, which includes
// 8 bytes of padding): what XcPKEncPublic writes and the size of buffers for it (Ninja Gaiden 2004's downloader).
uint32_t __stdcall XcPKGetKeyLen(const uint8_t *PublicKey)
{
    KTRACE("%p", PublicKey);
    if (!PublicKey || memcmp(PublicKey, "RSA1", 4)) return 0;
    uint32_t mod_bytes;
    memcpy(&mod_bytes, PublicKey + 4, 4);
    return mod_bytes;
}

void __stdcall XcDESKeyParity(uint8_t *Key, uint32_t KeyLength)
{
    KTRACE("%p, %u", Key, KeyLength);
    for (uint32_t i = 0; i < KeyLength; i++) {
        uint8_t b = Key[i] & 0xFE;
        uint8_t ones = 0;
        for (int k = 1; k < 8; k++) ones += (b >> k) & 1;
        Key[i] = b | ((ones & 1) ? 0 : 1); // odd parity
    }
}

// Cipher 0 = DES (8-byte key), 1 = 3DES (24 bytes: three keys)
void __stdcall XcKeyTable(uint32_t Cipher, uint8_t *KeyTable, const uint8_t *Key)
{
    KTRACE("%u, %p, %p", Cipher, KeyTable, Key);
    DesKey *k = (DesKey *)KeyTable;
    int keys = Cipher == 1 ? 3 : 1;
    for (int i = 0; i < keys; i++) des_schedule(&k[i], Key + 8 * i);
}

// Op 1 = encrypt, 0 = decrypt; Feedback = the IV in, the last cipher block out
void __stdcall XcBlockCryptCBC(uint32_t Cipher, uint32_t InputLength, uint8_t *Output, const uint8_t *Input,
                               const uint8_t *KeyTable, uint32_t Op, uint8_t *Feedback)
{
    KTRACE("%u, %u, %p, %p, %p, %u, %p", Cipher, InputLength, Output, Input, KeyTable, Op, Feedback);
    const DesKey *k = (const DesKey *)KeyTable;
    bool triple = Cipher == 1;
    uint64_t iv = load_be64(Feedback);
    for (uint32_t off = 0; off + 8 <= InputLength; off += 8) {
        uint64_t in = load_be64(Input + off), out;
        if (Op) {
            uint64_t x = in ^ iv;
            x = des_block(&k[0], x, true);
            if (triple) x = des_block(&k[2], des_block(&k[1], x, false), true);
            out = iv = x;
        } else {
            uint64_t x = in;
            if (triple) x = des_block(&k[0], des_block(&k[1], des_block(&k[2], x, false), true), false);
            else x = des_block(&k[0], x, false);
            out = x ^ iv;
            iv = in;
        }
        store_be64(Output + off, out);
    }
    store_be64(Feedback, iv);
}

} // namespace xk
