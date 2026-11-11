# usage: dsp_unscramble.py IMAGE OUT
# The DirectSound library's unscrambling of a DSP effects image's code segments, instruction for instruction
# (x2.1: FUN_0037DAAE calls FUN_00382386 / FUN_003823B3, whose generator is FUN_0038230D).
import struct, sys

M = 0xFFFFFFFF
state = [0, 0]  # the generator's global state (0x396DB0 lo, 0x396DB4 hi)


def gen(lo, hi, n):
    """FUN_0038230D(lo, hi, out, n): from (lo, hi), or the global state when both are 0; returns n bytes of the new state."""
    if (lo | hi) == 0:
        lo, hi = state
    ecx, eax = lo, hi
    edx = (ecx << 2) & M
    edx ^= ecx
    edx = (edx << 1) & M
    edx ^= ecx
    edx = (edx << 28) & M
    edx ^= eax
    ecx = ((ecx >> 1) | (eax << 31)) & M  # shrd ecx, eax, 1
    edx &= 0x80000000
    eax >>= 1
    new_lo, new_hi = ecx, edx | eax
    state[0], state[1] = new_lo, new_hi
    return struct.pack('<II', new_lo, new_hi)[:n]


def key():
    """FUN_00382386: seeded with 0x49DE12BA / 0x7FA49BCA, then 0x50 steps; the last 8 bytes."""
    gen(0x49DE12BA, 0x7FA49BCA, 0)
    out = b''
    for _ in range(0x50):
        out = gen(0, 0, 8)
    return out


def unscramble(iv, buf):
    """FUN_003823B3 with flag 0: iv = the 8 bytes given (T); returns the plain bytes."""
    K = key()
    lo, hi = struct.unpack('<II', K)
    gen(lo, hi, 0)  # reseed with the key
    out = bytearray(len(buf))
    for i in range(len(buf)):
        k = K[i & 7]
        al = (iv[i & 7] * k) & 0xFF  # imul bl: low byte
        c = (buf[i] - al) & 0xFF
        r = gen(0, 0, 1)[0]
        out[i] = k ^ r ^ c
    return bytes(out)


img = bytearray(open(sys.argv[1], 'rb').read())
w = lambda o: struct.unpack_from('<I', img, o)[0]
code_words, other = w(0x804), w(0x80C)
desc = 0x818 + (other + code_words) * 4
count = w(desc)
ivs_at = desc + 0x28 + (count - 1) * 32
K = key()
ivs = unscramble(K, img[ivs_at:ivs_at + count * 8])
print('key', K.hex(), 'ivs at', hex(ivs_at))
cur = 0x818
for e in range(count):
    size = w(desc + 8 + 32 * e + 4)
    plain = unscramble(ivs[8 * e:8 * e + 8], img[cur:cur + size])
    hi = sum(1 for j in range(0, len(plain), 4) if plain[j + 3] != 0)
    print('effect', e, 'code at', hex(cur), 'size', hex(size), 'words with a non-zero top byte:', hi, 'of', len(plain) // 4)
    img[cur:cur + size] = plain
    cur += size
open(sys.argv[2], 'wb').write(img)
