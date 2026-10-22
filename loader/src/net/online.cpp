// Online play's console identity (project/docs/09). Insignia knows a console by what xemu's registered set holds: the
// EEPROM (serial number, MAC address, online key, and the HDD key in its RC4-encrypted section) and the hard disk's
// config area (sector 8 network settings, the machine account in sector 9, the Live accounts after it). Both files
// are only read, from the paths black.ini names; nothing of them is written anywhere or logged.
#include "net/online.h"

#include "common.h"
#include "kernel/kexports.h"
#include "net/nvnet.h"
#include "sha1.h"

#include <tlhelp32.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>

namespace {

constexpr uint32_t CONFIG_AREA = 0x80000;    // the disk's first 1024 sectors
constexpr uint32_t OVERLAY_START = 8 * 512;  // sectors 8 on; 0-7 (partition info, refurb, cache table) stay the port's

bool g_enabled;
uint8_t g_eeprom[256];
std::vector<uint8_t> g_config; // CONFIG_AREA bytes of the registered disk
std::mutex g_config_lock;

// --- EEPROM: the security section's "HMAC" is SHA-1 resumed from per-kernel-version states after one 64-byte block
// (xemu hw/xbox/eeprom_generation.c); the RC4 key is that hash of the stored hash ---

struct HashStates {
    const char *name;
    uint32_t inner[5], outer[5];
};
const HashStates kStates[] = {
    {"1.0", {0x72127625, 0x336472B9, 0xBE609BEA, 0xF55E226B, 0x99958DAC}, {0x76441D41, 0x4DE82659, 0x2E8EF85E, 0xB256FACA, 0xC4FE2DE8}},
    {"debug", {0x85F9E51A, 0xE04613D2, 0x6D86A50C, 0x77C32E3C, 0x4BD717A4}, {0x5D7A9C6B, 0xE1922BEB, 0xB82CCDBC, 0x3137AB34, 0x486B52B3}},
    {"1.1-1.5", {0x39B06E79, 0xC9BD25E8, 0xDBC6B498, 0x40B4389D, 0x86BBD7ED}, {0x9B49BED3, 0x84B430FC, 0x6B8749CD, 0xEBFE5FE5, 0xD96E7393}},
    {"1.6", {0x8058763A, 0xF97D4E0E, 0x865A9762, 0x8A3D920D, 0x08995B2C}, {0x01075307, 0xA2F1E037, 0x1186EEEA, 0x88DA9992, 0x168A5609}},
};

void sha1_resumed(const uint32_t state[5], const uint8_t *data, size_t len, uint8_t out[20])
{
    Sha1Ctx c;
    sha1_init(&c);
    memcpy(c.state, state, sizeof c.state);
    c.count_lo = 64; // one block already hashed
    sha1_update(&c, data, len);
    sha1_final(&c, out);
}

void xhash(const HashStates &s, const uint8_t *data, size_t len, uint8_t out[20])
{
    uint8_t in[20];
    sha1_resumed(s.inner, data, len, in);
    sha1_resumed(s.outer, in, 20, out);
}

// The 28 decrypted bytes of the security section (confounder, HDD key, game region); nullptr if no version verifies
const char *decrypt_eeprom(const uint8_t *e, uint8_t dec[28])
{
    for (const HashStates &s : kStates) {
        uint8_t key[20], rc4[258], check[20];
        xhash(s, e, 20, key);
        memcpy(dec, e + 0x14, 28);
        xk::XcRC4Key(rc4, 20, key);
        xk::XcRC4Crypt(rc4, 28, dec);
        xhash(s, dec, 28, check);
        if (!memcmp(check, e, 20)) return s.name;
    }
    return nullptr;
}

bool read_file(const std::wstring &path, uint64_t off, void *buf, uint32_t len)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)off;
    DWORD got = 0;
    bool ok = SetFilePointerEx(h, li, nullptr, FILE_BEGIN) && ReadFile(h, buf, len, &got, nullptr) && got == len;
    CloseHandle(h);
    return ok;
}

uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }

// The disk's first CONFIG_AREA bytes from a qcow2 image (xemu's format; read only, unallocated clusters are zeros) or a
// raw image
bool read_config_area(const std::wstring &path, std::vector<uint8_t> &out, std::string &why)
{
    uint8_t h[104];
    if (!read_file(path, 0, h, sizeof h)) {
        why = "cannot read the file";
        return false;
    }
    out.assign(CONFIG_AREA, 0);
    if (memcmp(h, "QFI\xfb", 4)) { // a raw disk image
        if (!read_file(path, 0, out.data(), CONFIG_AREA)) {
            why = "raw image shorter than 512 KB";
            return false;
        }
        return true;
    }
    uint32_t cluster_bits = be32(h + 20), crypt = be32(h + 32), l1_size = be32(h + 36);
    uint64_t l1_off = be64(h + 40);
    if (crypt || cluster_bits < 9 || cluster_bits > 21) {
        why = crypt ? "encrypted qcow2" : "unexpected qcow2 cluster size";
        return false;
    }
    uint32_t cs = 1u << cluster_bits, l2_entries = cs / 8;
    std::vector<uint8_t> l2(cs);
    uint64_t cached_l2 = 0;
    for (uint32_t off = 0; off < CONFIG_AREA;) {
        uint32_t vcl = off >> cluster_bits, within = off & (cs - 1), n = std::min(CONFIG_AREA - off, cs - within);
        uint32_t l1i = vcl / l2_entries, l2i = vcl % l2_entries;
        uint8_t l1e[8];
        if (l1i < l1_size && read_file(path, l1_off + 8ull * l1i, l1e, 8)) {
            uint64_t l2_off = be64(l1e) & 0x00FFFFFFFFFFFE00ull;
            if (l2_off) {
                if (l2_off != cached_l2) {
                    if (!read_file(path, l2_off, l2.data(), cs)) {
                        why = "cannot read a qcow2 L2 table";
                        return false;
                    }
                    cached_l2 = l2_off;
                }
                uint64_t e = be64(&l2[8ull * l2i]);
                if (e & (1ull << 62)) {
                    why = "compressed qcow2 clusters (not supported)";
                    return false;
                }
                uint64_t host = e & 0x00FFFFFFFFFFFE00ull;
                if (host && !(e & 1) && !read_file(path, host + within, out.data() + off, n)) {
                    why = "cannot read a qcow2 data cluster";
                    return false;
                }
            }
        }
        off += n;
    }
    return true;
}

bool sector_signed(const std::vector<uint8_t> &c, int sector)
{
    uint32_t sig;
    memcpy(&sig, &c[sector * 512], 4);
    return sig == 0x79132568; // XBOX_CONFIG_SECTOR signature
}

bool process_running(const wchar_t *exe)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe = {sizeof pe};
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe)) found = !_wcsicmp(pe.szExeFile, exe);
    CloseHandle(snap);
    return found;
}

} // namespace

bool online_enabled() { return g_enabled; }

bool online_init()
{
    if (!g_cfg.online) return false;
    auto fail = [](const std::string &why) {
        logc("online: off (%s); the game runs without a network", why.c_str());
        return false;
    };
    if (g_cfg.online_eeprom.empty() || g_cfg.online_hdd.empty()) return fail("black.ini needs online_eeprom and online_hdd");
    // One console identity must not be on Xbox Live twice: xemu with the same EEPROM would be logged off by this
    if (process_running(L"xemu.exe")) return fail("xemu is running; close it first (the same console must not sign in twice)");
    if (!kcrypto_selftest()) return fail("crypto self-test failed");
    if (!read_file(g_cfg.online_eeprom, 0, g_eeprom, sizeof g_eeprom)) return fail("cannot read online_eeprom " + narrow(g_cfg.online_eeprom));
    uint8_t dec[28];
    const char *ver = decrypt_eeprom(g_eeprom, dec);
    if (!ver) return fail("online_eeprom: the security section does not verify (not an Xbox EEPROM?)");
    std::string why;
    if (!read_config_area(g_cfg.online_hdd, g_config, why)) return fail("online_hdd: " + why);
    int accounts = 0;
    for (int s = 9; s < 32; s++) accounts += sector_signed(g_config, s);
    if (!sector_signed(g_config, 9)) return fail("online_hdd: no machine account in config sector 9 (console not registered?)");
    memcpy(xk::XboxHDKey, dec + 8, 16);
    SecureZeroMemory(dec, sizeof dec);
    logc("online: on - EEPROM verifies (%s kernel states), %d signed account sectors on the disk", ver, accounts);
    g_enabled = nvnet_install();
    return g_enabled;
}

bool online_factory_setting(uint32_t index, uint32_t *type, uint8_t *out, uint32_t *size)
{
    if (!g_enabled) return false;
    switch (index) {
    case 0x100: memcpy(out, g_eeprom + 0x34, 12); *size = 12; break; // XC_FACTORY_SERIAL_NUMBER
    case 0x101: memcpy(out, g_eeprom + 0x40, 6); *size = 6; break;   // XC_FACTORY_ETHERNET_ADDR
    case 0x102: memcpy(out, g_eeprom + 0x48, 16); *size = 16; break; // XC_FACTORY_ONLINE_KEY
    default: return false;
    }
    *type = 3; // REG_BINARY
    return true;
}

void online_partition0_read(uint64_t offset, uint8_t *buf, uint32_t len)
{
    if (!g_enabled || offset >= CONFIG_AREA || offset + len <= OVERLAY_START) return;
    std::lock_guard<std::mutex> g(g_config_lock);
    uint64_t lo = std::max<uint64_t>(offset, OVERLAY_START), hi = std::min<uint64_t>(offset + len, CONFIG_AREA);
    memcpy(buf + (lo - offset), &g_config[(size_t)lo], (size_t)(hi - lo));
    static int noted;
    if (noted++ < 16) logf("online: config sectors %llu-%llu read from the registered disk", lo / 512, (hi - 1) / 512);
}

bool online_partition0_write(uint64_t offset, const uint8_t *buf, uint32_t len)
{
    if (!g_enabled || offset < OVERLAY_START || offset + len > CONFIG_AREA) return false;
    std::lock_guard<std::mutex> g(g_config_lock);
    memcpy(&g_config[(size_t)offset], buf, len);
    logf("online: config sectors %llu-%llu written (kept for this run only; the registered disk is not changed)", offset / 512,
         (offset + len - 1) / 512);
    return true;
}
