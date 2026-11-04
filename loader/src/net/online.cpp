// Online play's console identity (project/docs/09). Insignia knows a console by its EEPROM (serial number, MAC address,
// online key, and the HDD key in its RC4-encrypted section) and its hard disk's config area (sector 8 network
// settings, the machine account in sector 9, the Live accounts in 11-19). The game uses the port's own copy of both in
// the hard disk folder (online_eeprom.bin, online_accounts.bin), imported from the console the player registered in
// xemu (black.ini online_eeprom / online_hdd, only read): the first time whole, later the accounts the port lacks. What
// the game or the dashboard writes to the config area (an account made in the port's Xbox Live dashboard) goes to
// online_accounts.bin. Nothing of these files is logged.
#include "net/online.h"

#include "build_profile.h"
#include "common.h"
#include "kernel/kexports.h"
#include "net/dashfonts.h"
#include "net/nvnet.h"
#include "sha1.h"

#include <bcrypt.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>

namespace {

constexpr uint32_t CONFIG_AREA = 0x80000;    // the disk's first 1024 sectors
constexpr uint32_t OVERLAY_START = 8 * 512;  // sectors 8 on; 0-7 (partition info, refurb, cache table) stay the port's

bool g_enabled;
std::wstring g_off_reason; // online = 1 but not started: why, for the player (online_off_reason)
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

// --- the port's own console, in the hard disk folder: online_eeprom.bin (the console's EEPROM, 256 bytes) and
// online_accounts.bin = "NGBACCT1", the console's serial number (12 bytes, EEPROM 0x34), 4 zero bytes, then sectors
// 8-1023 of the config area (network settings, machine account, the Live accounts). The game reads only these; both are
// made by importing from xemu (import_xemu) and the store is written again whenever the game or the dashboard writes
// the config area. Written whole to a temporary file, then moved over the old one, so a crash or power loss mid-write
// leaves the previous one ---

constexpr char STORE_MAGIC[8] = {'N', 'G', 'B', 'A', 'C', 'C', 'T', '1'};
constexpr size_t STORE_HEAD = 24;

std::wstring store_path() { return host_full_path(g_cfg.hdd_dir) + L"\\online_accounts.bin"; }
std::wstring local_eeprom_path() { return host_full_path(g_cfg.hdd_dir) + L"\\online_eeprom.bin"; }

bool write_atomic(const std::wstring &path, const void *a, size_t na, const void *b = nullptr, size_t nb = 0)
{
    std::wstring tmp = path + L".tmp";
    CreateDirectoryW(host_full_path(g_cfg.hdd_dir).c_str(), nullptr);
    FILE *f = nullptr;
    if (_wfopen_s(&f, tmp.c_str(), L"wb") || !f) return false;
    bool ok = fwrite(a, 1, na, f) == na && (!nb || fwrite(b, 1, nb, f) == nb) && fflush(f) == 0;
    fclose(f);
    if (ok) ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

// The store into c (CONFIG_AREA bytes; sectors 0-7 zero) and its console's serial number. 0 none, -1 not a store, 1 ok
int store_load(std::vector<uint8_t> &c, uint8_t serial[12])
{
    std::vector<uint8_t> s(STORE_HEAD + CONFIG_AREA - OVERLAY_START);
    FILE *f = nullptr;
    if (_wfopen_s(&f, store_path().c_str(), L"rb") || !f) return 0;
    size_t n = fread(s.data(), 1, s.size(), f);
    fclose(f);
    if (n != s.size() || memcmp(s.data(), STORE_MAGIC, 8)) {
        logf("online: the account store %s is not one (%zu bytes)", narrow(store_path()).c_str(), n);
        return -1;
    }
    memcpy(serial, s.data() + 8, 12);
    c.assign(CONFIG_AREA, 0);
    memcpy(&c[OVERLAY_START], s.data() + STORE_HEAD, CONFIG_AREA - OVERLAY_START);
    SecureZeroMemory(s.data(), s.size());
    return 1;
}

bool store_save(const std::vector<uint8_t> &c, const uint8_t serial[12])
{
    uint8_t head[STORE_HEAD] = {};
    memcpy(head, STORE_MAGIC, 8);
    memcpy(head + 8, serial, 12);
    return write_atomic(store_path(), head, sizeof head, &c[OVERLAY_START], CONFIG_AREA - OVERLAY_START);
}

bool sector_signed(const std::vector<uint8_t> &c, int sector)
{
    uint32_t sig;
    memcpy(&sig, &c[sector * 512], 4);
    return sig == 0x79132568; // XBOX_CONFIG_SECTOR signature
}

// A config sector's checksum (dword 126, before the end mark 0xAA550000): the complement of the end-around-carry sum
// of its other dwords (checked on every sector of a registered console's disk)
void sector_checksum(std::vector<uint8_t> &c, int sector)
{
    uint32_t d[128];
    memcpy(d, &c[sector * 512], 512);
    uint64_t t = 0;
    for (int i = 0; i < 128; i++)
        if (i != 126) t += d[i], t = (t & 0xFFFFFFFF) + (t >> 32);
    uint32_t sum = ~(uint32_t)t;
    memcpy(&c[sector * 512 + 504], &sum, 4);
}

// The Live accounts: sector 11 holds a table of 12-byte entries from offset 12 (a dword, nonzero when the slot holds an
// account, and 8 bytes the account library keeps with it), entry i for the account in sector 12 + i; an account's
// sector has its ID (XUID) at 12 and its gamertag at 24. Found with the Xbox Live dashboard (r963): an account sector
// with its entry moved to another slot is still found, gaps between slots are skipped, and a sector without its entry
// is not listed. 8 slots: the XDK's XONLINE_MAX_STORED_ONLINE_USERS.
constexpr int TABLE_SECTOR = 11, FIRST_SLOT = 12, SLOTS = 8;
uint8_t *slot_entry(std::vector<uint8_t> &c, int i) { return &c[TABLE_SECTOR * 512 + 12 + 12 * i]; }
bool slot_used(const std::vector<uint8_t> &c, int i)
{
    uint32_t used;
    memcpy(&used, &c[TABLE_SECTOR * 512 + 12 + 12 * i], 4);
    return used && sector_signed(c, TABLE_SECTOR) && sector_signed(c, FIRST_SLOT + i);
}
const uint8_t *slot_xuid(const std::vector<uint8_t> &c, int i) { return &c[(FIRST_SLOT + i) * 512 + 12]; }

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

// Config sector 8, the console's network settings as the dashboard writes them (offsets in the sector, read from a
// registered console's disk, project/docs/09): 76 flags (8 = DNS entered by hand), 96 / 100 the two DNS servers
constexpr int NET_SECTOR = 8, NET_FLAGS = 76, NET_DNS1 = 96, NET_DNS2 = 100;
constexpr uint32_t NET_MANUAL_DNS = 8;

std::string ip4(const uint8_t *p)
{
    char s[16];
    snprintf(s, sizeof s, "%u.%u.%u.%u", p[0], p[1], p[2], p[3]);
    return s;
}

// The gamertag of an account sector (9 the machine account, 10 on the players'): its name at offset 24, up to 15
// characters; "" if that is not a name
std::string account_name(const std::vector<uint8_t> &c, int sector)
{
    const char *p = (const char *)&c[sector * 512 + 24];
    size_t n = strnlen(p, 16);
    if (!n || n == 16) return "";
    for (size_t i = 0; i < n; i++)
        if (p[i] < 0x20 || p[i] > 0x7E) return "";
    return std::string(p, n);
}

// The gamertag in account slot i ("" if none)
std::string slot_name(const std::vector<uint8_t> &c, int i) { return slot_used(c, i) ? account_name(c, FIRST_SLOT + i) : ""; }

// Slot of c whose account has this ID, or -1
int find_xuid(const std::vector<uint8_t> &c, const uint8_t *xuid)
{
    for (int i = 0; i < SLOTS; i++)
        if (slot_used(c, i) && !memcmp(slot_xuid(c, i), xuid, 8)) return i;
    return -1;
}

// Reads and checks the xemu EEPROM (black.ini online_eeprom) into e; "" or why not, for the player
std::wstring xemu_eeprom(uint8_t e[256])
{
    if (g_cfg.online_eeprom.empty()) return L"没有选 xemu 的 EEPROM";
    if (!read_file(g_cfg.online_eeprom, 0, e, 256)) return L"读不到 xemu 的 EEPROM";
    uint8_t dec[28];
    bool ok = kcrypto_selftest() && decrypt_eeprom(e, dec);
    SecureZeroMemory(dec, sizeof dec);
    return ok ? L"" : L"xemu 的 EEPROM 无效";
}

// Imports the console registered in xemu (black.ini online_eeprom and online_hdd, only read) into the port's own: the
// first time its EEPROM, network settings, machine account and every Live account; later only the Live accounts the
// port does not have yet (by account ID; one it has stays as it is: the port may have written it since), into free
// slots. Only the same console (the same EEPROM): an account belongs to its console. The Xbox Live dashboard's files
// are copied too (dashfonts.cpp; they never ship with the port). Lines for the launcher in r: imported / exists / full =
// gamertag, error = why. false on an error (nothing changed then).
bool import_xemu(std::string &r)
{
    auto put = [&](const char *k, const std::string &v) { r += std::string(k) + " = " + v + "\n"; };
    auto err = [&](const std::string &why, const std::wstring &player) {
        logf("online: import from xemu: %s", why.c_str());
        put("error", narrow(player));
        return false;
    };
    if (process_running(L"xemu.exe")) return err("xemu is running", L"xemu 正在运行：先关掉它再导入（它可能正在写硬盘镜像）");
    uint8_t e[256];
    std::wstring why_ee = xemu_eeprom(e);
    if (!why_ee.empty()) return err("the xemu EEPROM is missing or not valid", why_ee);
    if (g_cfg.online_hdd.empty()) return err("no xemu hard disk image chosen", L"没有选 xemu 的硬盘镜像");
    std::vector<uint8_t> img;
    std::string why;
    if (!read_config_area(g_cfg.online_hdd, img, why)) return err("the xemu hard disk image: " + why, L"读不了 xemu 的硬盘镜像");
    if (!sector_signed(img, 9)) return err("no machine account in the image", L"xemu 硬盘里没有注册过的主机：先在 xemu 里注册 Insignia");
    uint8_t mine[256];
    bool have_eeprom = read_file(local_eeprom_path(), 0, mine, sizeof mine);
    if (have_eeprom && memcmp(mine, e, sizeof e))
        return err("the port has another console", L"移植版已经是另一台主机，账户不能跨主机导入");
    std::vector<uint8_t> c;
    uint8_t serial[12];
    int st = store_load(c, serial);
    if (st == 1 && memcmp(serial, e + 0x34, 12)) return err("the account store is another console's", L"本地账户属于另一台主机");
    bool fresh = st != 1;
    if (fresh) {
        c = img; // everything: network settings, machine account, the accounts
        memset(c.data(), 0, OVERLAY_START);
        for (int i = 0; i < SLOTS; i++)
            if (slot_used(c, i)) put("imported", slot_name(c, i));
    } else {
        if (!sector_signed(c, 9)) memcpy(&c[9 * 512], &img[9 * 512], 512);
        bool table = false;
        for (int i = 0; i < SLOTS; i++) {
            if (!slot_used(img, i)) continue;
            std::string name = slot_name(img, i);
            if (find_xuid(c, slot_xuid(img, i)) >= 0) {
                put("exists", name);
                continue;
            }
            int to = -1;
            for (int j = 0; j < SLOTS && to < 0; j++)
                if (!slot_used(c, j)) to = j;
            if (to < 0) {
                put("full", name);
                continue;
            }
            if (!sector_signed(c, TABLE_SECTOR)) { // no table yet: the image's header and end mark, its entries cleared
                memcpy(&c[TABLE_SECTOR * 512], &img[TABLE_SECTOR * 512], 512);
                memset(&c[TABLE_SECTOR * 512 + 12], 0, 12 * SLOTS);
            }
            memcpy(&c[(FIRST_SLOT + to) * 512], &img[(FIRST_SLOT + i) * 512], 512);
            memcpy(slot_entry(c, to), slot_entry(img, i), 12);
            table = true;
            put("imported", name);
        }
        if (table) sector_checksum(c, TABLE_SECTOR);
    }
    bool ok = (have_eeprom || write_atomic(local_eeprom_path(), e, sizeof e)) && store_save(c, e + 0x34);
    SecureZeroMemory(e, sizeof e);
    SecureZeroMemory(img.data(), img.size());
    SecureZeroMemory(c.data(), c.size());
    if (!ok) return err("could not write the port's console files", L"写不了本地的主机文件（存档目录只读？）");
    std::wstring p2 = host_full_path(g_cfg.hdd_dir) + L"\\Partition2";
    bool dash = dash_fonts_ensure(g_cfg.online_hdd, p2) && dash_dir_ensure(g_cfg.online_hdd, p2, "xodash", L"xonlinedash.xbe") &&
                dash_file_ensure(g_cfg.online_hdd, p2, "xboxdash.xbe");
    put("dashboard", dash ? "ok" : "missing");
    logf("online: imported from xemu (%s)%s", fresh ? "the console and its accounts" : "accounts the port did not have",
         dash ? "" : "; the Xbox Live dashboard's files are not in the image");
    return true;
}

// --- a console of the port's own, without xemu: a new EEPROM made as xemu makes one (hw/xbox/eeprom_generation.c) ---

// An EEPROM section checksum: the complement of the end-around-carry sum of its dwords (checked on a registered
// console's EEPROM: the factory section 0x34-0x5F at 0x30, the user section 0x64-0xBF at 0x60)
uint32_t eeprom_checksum(const uint8_t *p, size_t n)
{
    uint64_t t = 0;
    for (size_t i = 0; i + 4 <= n; i += 4) {
        uint32_t v;
        memcpy(&v, p + i, 4);
        t += v;
        t = (t & 0xFFFFFFFF) + (t >> 32);
    }
    return ~(uint32_t)t;
}

// A new console: random serial number (12 digits: all the Setup Assistant checks, 0x11595), MAC address (Microsoft's
// 00:50:F2), online key, HDD key and confounder; North America, NTSC-M, English. The security section encrypted with
// the 1.0 kernel's states, as the registered xemu console's is. false if the system's random numbers fail.
bool make_eeprom(uint8_t e[256])
{
    uint8_t r[64];
    if (BCryptGenRandom(nullptr, r, sizeof r, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return false;
    memset(e, 0, 256);
    for (int i = 0; i < 12; i++) e[0x34 + i] = (uint8_t)('0' + r[i] % 10); // serial number (a slight bias: harmless)
    e[0x40] = 0x00, e[0x41] = 0x50, e[0x42] = 0xF2;                        // MAC address
    memcpy(e + 0x43, r + 12, 3);
    memcpy(e + 0x48, r + 15, 16); // online key
    uint32_t av = 0x00400100;     // NTSC-M
    memcpy(e + 0x58, &av, 4);
    uint32_t sum = eeprom_checksum(e + 0x34, 0x2C);
    memcpy(e + 0x30, &sum, 4);
    uint32_t lang = 1; // English
    memcpy(e + 0x90, &lang, 4);
    sum = eeprom_checksum(e + 0x64, 0x5C);
    memcpy(e + 0x60, &sum, 4);
    uint8_t plain[28];
    memcpy(plain, r + 31, 8);       // confounder
    memcpy(plain + 8, r + 39, 16);  // HDD key
    uint32_t region = 1;            // North America
    memcpy(plain + 24, &region, 4);
    const HashStates &s = kStates[0]; // 1.0
    uint8_t key[20], rc4[258];
    xhash(s, plain, 28, e);  // the stored hash
    xhash(s, e, 20, key);    // the RC4 key: the hash of it
    xk::XcRC4Key(rc4, 20, key);
    memcpy(e + 0x14, plain, 28);
    xk::XcRC4Crypt(rc4, 28, e + 0x14);
    SecureZeroMemory(r, sizeof r);
    SecureZeroMemory(plain, sizeof plain);
    SecureZeroMemory(key, sizeof key);
    uint8_t dec[28];
    bool ok = decrypt_eeprom(e, dec) != nullptr; // as the game will read it
    SecureZeroMemory(dec, sizeof dec);
    return ok;
}

} // namespace

bool online_new_console(const std::wstring &out_path)
{
    // A console of the port's own in the hard disk folder, not yet registered with Insignia (the Setup Assistant does
    // that); only when there is none (it would replace that console and leave its accounts behind)
    std::string r;
    auto finish = [&](bool ok) {
        if (FILE *f = _wfopen(out_path.c_str(), L"wb")) {
            fputs(r.c_str(), f);
            fclose(f);
        }
        return ok;
    };
    uint8_t e[256];
    std::vector<uint8_t> c;
    uint8_t serial[12];
    if (read_file(local_eeprom_path(), 0, e, sizeof e) || store_load(c, serial) != 0) {
        r = "error = " + narrow(L"本机已经有主机了") + "\n";
        return finish(false);
    }
    if (!kcrypto_selftest() || !make_eeprom(e)) {
        r = "error = " + narrow(L"生成主机失败（请回报）") + "\n";
        return finish(false);
    }
    c.assign(CONFIG_AREA, 0); // no network settings (the defaults: DHCP), no machine account, no accounts yet
    bool ok = write_atomic(local_eeprom_path(), e, sizeof e) && store_save(c, e + 0x34);
    SecureZeroMemory(e, sizeof e);
    if (!ok) {
        DeleteFileW(local_eeprom_path().c_str());
        r = "error = " + narrow(L"写不了存档目录") + "\n";
        return finish(false);
    }
    logf("online: a new console of the port's own made in %s (not registered with Insignia yet)",
         narrow(host_full_path(g_cfg.hdd_dir)).c_str());
    r = "created = 1\n";
    return finish(true);
}

bool online_dash_from(const std::wstring &image, const std::wstring &out_path)
{
    // The Xbox Live dashboard's files (C:\xodash, C:\fonts, C:\xboxdash.xbe) from any Xbox hard disk image the player has
    // (only read; it need not be a registered console's), into the hard disk folder: for a console of the port's own
    std::wstring p2 = host_full_path(g_cfg.hdd_dir) + L"\\Partition2";
    bool ok = dash_fonts_ensure(image, p2) && dash_dir_ensure(image, p2, "xodash", L"xonlinedash.xbe") &&
              dash_file_ensure(image, p2, "xboxdash.xbe");
    std::string r = ok ? "dashboard = ok\n" : "dashboard = missing\n";
    if (FILE *f = _wfopen(out_path.c_str(), L"wb")) {
        fputs(r.c_str(), f);
        fclose(f);
    }
    return ok;
}

bool online_whole_eeprom(uint8_t out[256])
{
    if (!g_enabled) return false;
    memcpy(out, g_eeprom, 256);
    return true;
}

bool online_import(const std::wstring &out_path)
{
    std::string r;
    bool ok = import_xemu(r);
    FILE *f = nullptr;
    if (_wfopen_s(&f, out_path.c_str(), L"wb") || !f) return false;
    fputs(r.c_str(), f);
    fclose(f);
    return ok;
}

bool online_check(const std::wstring &out_path)
{
    // What the launcher's 联网 page shows, one "key = value" per line: nothing secret (no key, serial or MAC)
    std::string r;
    auto put = [&](const char *k, const std::string &v) { r += std::string(k) + " = " + v + "\n"; };
    put("xemu", process_running(L"xemu.exe") ? "1" : "0");
    // The port's own console (what the game uses): local = ok / none / invalid / other (the store is another console's)
    uint8_t mine[256], serial[12];
    std::vector<uint8_t> loc;
    bool have_mine = read_file(local_eeprom_path(), 0, mine, sizeof mine);
    int st = store_load(loc, serial);
    bool local_ok = false;
    if (!have_mine && st != 1) put("local", "none");
    else {
        uint8_t dec[28];
        bool valid = have_mine && st == 1 && kcrypto_selftest() && decrypt_eeprom(mine, dec);
        SecureZeroMemory(dec, sizeof dec);
        local_ok = valid && !memcmp(serial, mine + 0x34, 12);
        put("local", local_ok ? "ok" : valid ? "other" : "invalid");
        if (local_ok) {
            put("local_registered", sector_signed(loc, 9) ? "1" : "0");
            for (int i = 0; i < SLOTS; i++)
                if (slot_used(loc, i)) put("local_account", slot_name(loc, i));
        }
    }
    // The console registered in xemu (black.ini online_eeprom / online_hdd): what can be imported from it
    uint8_t e[256];
    bool have_xemu_eeprom = false;
    if (g_cfg.online_eeprom.empty()) put("eeprom", "missing");
    else if (!read_file(g_cfg.online_eeprom, 0, e, sizeof e)) put("eeprom", "unreadable");
    else {
        uint8_t dec[28];
        have_xemu_eeprom = kcrypto_selftest() && decrypt_eeprom(e, dec);
        put("eeprom", have_xemu_eeprom ? "ok" : "invalid");
        SecureZeroMemory(dec, sizeof dec);
        if (have_xemu_eeprom && have_mine) put("same_console", memcmp(e, mine, sizeof e) ? "0" : "1");
    }
    SecureZeroMemory(e, sizeof e);
    SecureZeroMemory(mine, sizeof mine);
    std::vector<uint8_t> c;
    std::string why;
    if (g_cfg.online_hdd.empty()) put("hdd", "missing");
    else if (!read_config_area(g_cfg.online_hdd, c, why)) put("hdd", why);
    else {
        put("hdd", "ok");
        put("registered", sector_signed(c, 9) ? "1" : "0");
        for (int i = 0; i < SLOTS; i++) {
            if (!slot_used(c, i)) continue;
            put("account", slot_name(c, i));
            if (!local_ok || find_xuid(loc, slot_xuid(c, i)) < 0) put("importable", slot_name(c, i));
        }
    }
    if (local_ok) c = loc; // the network settings the game will use
    SecureZeroMemory(loc.data(), loc.size());
    if (!c.empty()) {
        if (sector_signed(c, NET_SECTOR)) {
            const uint8_t *n = &c[NET_SECTOR * 512];
            uint32_t flags;
            memcpy(&flags, n + NET_FLAGS, 4);
            put("dns", flags & NET_MANUAL_DNS ? "manual" : "auto");
            put("dns1", ip4(n + NET_DNS1));
            put("dns2", ip4(n + NET_DNS2));
        } else put("dns", "auto"); // never set up: the dashboard's defaults (DHCP)
    }
    SecureZeroMemory(c.data(), c.size());
    put("dns_relay", g_cfg.online_dns.empty() ? "46.101.64.175" : narrow(g_cfg.online_dns));
    if (!g_cfg.online_insigniadns.empty()) put("insigniadns", narrow(g_cfg.online_insigniadns));
    FILE *f = nullptr;
    if (_wfopen_s(&f, out_path.c_str(), L"wb") || !f) return false;
    fputs(r.c_str(), f);
    fclose(f);
    return true;
}

bool online_enabled() { return g_enabled; }

const std::wstring &online_off_reason() { return g_off_reason; }

bool online_init()
{
    if (!g_cfg.online) return false;
    auto fail = [](const std::string &why, const wchar_t *player) {
        logc("online: off (%s); the game runs without a network", why.c_str());
        g_off_reason = std::wstring(L"联网未开启：") + player;
        return false;
    };
    // One console identity must not be on Xbox Live twice: xemu with the same EEPROM would be logged off by this
    if (process_running(L"xemu.exe"))
        return fail("xemu is running; close it first (the same console must not sign in twice)", L"xemu 正在运行，请先关掉它再启动游戏");
    if (!kcrypto_selftest()) return fail("crypto self-test failed", L"加密自检失败（请回报）");
    // The port's own console (hard disk folder). Not there yet but xemu's files chosen (players from before 2.2, or a
    // new save folder): imported from them once, as the launcher's import does.
    uint8_t serial[12];
    if (GetFileAttributesW(local_eeprom_path().c_str()) == INVALID_FILE_ATTRIBUTES || store_load(g_config, serial) != 1) {
        if (g_cfg.online_eeprom.empty() || g_cfg.online_hdd.empty())
            return fail("no console of the port's own and no xemu files to import it from", L"还没有主机：在启动器「联网」页从 xemu 导入");
        std::string r;
        if (!import_xemu(r)) {
            size_t at = r.find("error = ");
            std::string why = at == std::string::npos ? "" : r.substr(at + 8, r.find('\n', at) - at - 8);
            return fail("importing the console from xemu failed", (L"从 xemu 导入主机失败：" + widen(why)).c_str());
        }
        logc("online: the console registered in xemu imported into %s (first start)", narrow(host_full_path(g_cfg.hdd_dir)).c_str());
    }
    if (!read_file(local_eeprom_path(), 0, g_eeprom, sizeof g_eeprom))
        return fail("cannot read " + narrow(local_eeprom_path()), L"读不到本地的主机 EEPROM");
    uint8_t dec[28];
    const char *ver = decrypt_eeprom(g_eeprom, dec);
    if (!ver) return fail("online_eeprom.bin: the security section does not verify", L"本地的主机 EEPROM 无效");
    if (store_load(g_config, serial) != 1 || memcmp(serial, g_eeprom + 0x34, 12)) {
        SecureZeroMemory(dec, sizeof dec);
        return fail("online_accounts.bin is missing or another console's", L"本地账户文件缺失或属于另一台主机（在启动器「联网」页重新导入）");
    }
    int accounts = 0;
    for (int i = 0; i < SLOTS; i++) accounts += slot_used(g_config, i);
    // No machine account yet: only the Insignia Setup Assistant (a program without a build profile: it registers the
    // console with Insignia) and the Xbox Live dashboard (its network test makes the machine account, written to config
    // sector 9: online_partition0_write) go online
    if (!sector_signed(g_config, 9) && g_cfg.profile && g_cfg.profile != &kProfileXod) {
        SecureZeroMemory(dec, sizeof dec);
        return fail("no machine account in config sector 9 (console not registered?)",
                    L"本机主机还没注册 Insignia（启动器「联网」页「注册主机」）");
    }
    if (!sector_signed(g_config, 9)) logc("online: the console is not registered with Insignia yet (no machine account)");
    memcpy(xk::XboxHDKey, dec + 8, 16);
    SecureZeroMemory(dec, sizeof dec);
    logc("online: on - the port's console (%s kernel states), %d Live account(s)", ver, accounts);
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
    bool saved = store_save(g_config, g_eeprom + 0x34); // the port's account store; the registered disk is not changed
    logf("online: config sectors %llu-%llu written: %s", offset / 512, (offset + len - 1) / 512,
         saved ? "kept in the port's account store" : "COULD NOT SAVE the account store (kept for this run only)");
    if (!g_cfg.online_config_out.empty()) { // tests: what the game wrote (an account made in the dashboard) kept in a file
        if (FILE *f = _wfopen(g_cfg.online_config_out.c_str(), L"wb")) {
            fwrite(g_config.data(), 1, g_config.size(), f);
            fclose(f);
        }
    }
    return true;
}
