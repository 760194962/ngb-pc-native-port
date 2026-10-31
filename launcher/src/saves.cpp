#include "saves.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace fs = std::filesystem;

namespace saves {

namespace {

std::wstring g_title = L"5443000d";
// UDATA (the slots) and TDATA (the system save) of the title
std::vector<std::wstring> title_dirs() { return {L"UDATA\\" + g_title, L"TDATA\\" + g_title}; }

// The title's downloadable content beside its system save (Ninja Gaiden 2004's update $u and packs $c)
bool content_entry(const fs::path &p)
{
    std::wstring n = p.filename().wstring();
    return !_wcsicmp(n.c_str(), L"$u") || !_wcsicmp(n.c_str(), L"$c");
}

// A file's or folder's last write, seconds since 1970 (0 if unreadable)
long long written(const fs::path &p)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a)) return 0;
    unsigned long long t = ((unsigned long long)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
    return (long long)((t - 116444736000000000ull) / 10000000ull); // 100 ns since 1601 -> seconds since 1970
}

// "Name=..." from a SaveMeta.xbx (UTF-16 with a byte-order mark)
std::wstring meta_name(const fs::path &meta)
{
    FILE *f = _wfopen(meta.c_str(), L"rb");
    if (!f) return L"";
    wchar_t buf[256] = {};
    size_t n = fread(buf, sizeof(wchar_t), 255, f);
    fclose(f);
    std::wstring s(buf, n);
    if (!s.empty() && s[0] == 0xFEFF) s.erase(0, 1);
    size_t a = s.find(L"Name=");
    if (a == std::wstring::npos) return L"";
    size_t b = s.find_first_of(L"\r\n", a);
    return s.substr(a + 5, b == std::wstring::npos ? std::wstring::npos : b - a - 5);
}

std::wstring stamp()
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t b[32];
    swprintf(b, 32, L"%04u-%02u-%02u_%02u-%02u-%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return b;
}

// A new, unused backup folder: <backup_root>\<date>[_note]
fs::path new_backup_dir(const std::wstring &backup_root, const std::wstring &note)
{
    std::wstring name = stamp() + (note.empty() ? L"" : L"_" + note);
    fs::path p = fs::path(backup_root) / name;
    for (int i = 2; fs::exists(p); i++) p = fs::path(backup_root) / (name + L"_" + std::to_wstring(i));
    return p;
}

bool copy_saves(const fs::path &from, const fs::path &to, std::wstring *error)
{
    std::error_code ec;
    for (const std::wstring &t : title_dirs()) {
        fs::path src = from / t;
        if (!fs::exists(src, ec)) continue;
        fs::create_directories(to / t, ec);
        for (const auto &e : fs::directory_iterator(src, ec)) {
            if (content_entry(e.path())) continue;
            fs::copy(e.path(), to / t / e.path().filename(), fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
            if (ec) {
                *error = (to / t).wstring() + L": " + std::wstring(ec.message().begin(), ec.message().end());
                return false;
            }
        }
    }
    return true;
}

// Moves root's title folders into `to` (rename; across drives copy, then remove the copied originals)
bool move_saves(const fs::path &root, const fs::path &to, std::wstring *error)
{
    std::error_code ec;
    for (const std::wstring &t : title_dirs()) {
        fs::path dir = root / t;
        if (!fs::exists(dir, ec)) continue;
        fs::create_directories(to / t, ec);
        std::vector<fs::path> entries; // listed first: the folder changes as they move
        for (const auto &e : fs::directory_iterator(dir, ec))
            if (!content_entry(e.path())) entries.push_back(e.path());
        for (const fs::path &src : entries) {
            fs::path dst = to / t / src.filename();
            fs::rename(src, dst, ec);
            if (!ec) continue;
            ec.clear();
            fs::copy(src, dst, fs::copy_options::recursive, ec);
            if (!ec) fs::remove_all(src, ec); // only once the copy is complete
            if (ec) {
                *error = src.wstring() + L": " + std::wstring(ec.message().begin(), ec.message().end());
                return false;
            }
        }
    }
    return true;
}

} // namespace

void set_title(const std::wstring &title) { g_title = title; }

std::wstring our_root(const std::wstring &hdd) { return (fs::path(hdd) / L"Partition1").wstring(); }
std::wstring xrevive_root(const std::wstring &folder) { return (fs::path(folder) / L"SaveData\\Ninja Gaiden_ Black\\HddData").wstring(); }

// Saves to speak of: a slot (a folder with SaveMeta.xbx) or the system save. A title folder with only its icons (as
// xRevive's before a first save) has none: importing it would have moved every slot here into a backup and left none.
bool has_saves(const std::wstring &root)
{
    std::error_code ec;
    if (fs::exists(fs::path(root) / title_dirs()[1] / L"system.dat", ec)) return true;
    for (const auto &e : fs::directory_iterator(fs::path(root) / title_dirs()[0], ec))
        if (e.is_directory() && fs::exists(e.path() / L"SaveMeta.xbx", ec)) return true;
    return false;
}

std::vector<Slot> slots(const std::wstring &root)
{
    std::vector<Slot> v;
    std::error_code ec;
    for (const auto &e : fs::directory_iterator(fs::path(root) / title_dirs()[0], ec)) {
        if (!e.is_directory()) continue;
        fs::path meta = e.path() / L"SaveMeta.xbx", data = e.path() / L"save000.dat";
        std::wstring name = meta_name(meta);
        if (name.empty()) continue;
        v.push_back({name, written(fs::exists(data, ec) ? data : meta)});
    }
    std::sort(v.begin(), v.end(), [](const Slot &a, const Slot &b) { return a.time > b.time; });
    return v;
}

std::vector<Backup> backups(const std::wstring &backup_root)
{
    std::vector<Backup> v;
    std::error_code ec;
    for (const auto &e : fs::directory_iterator(backup_root, ec)) {
        if (!e.is_directory() || !has_saves(e.path().wstring())) continue;
        std::wstring name = e.path().filename().wstring(); // 2026-10-22_14-03-11[_note]
        size_t u = name.find(L'_', 11);
        std::wstring note = u == std::wstring::npos ? L"" : name.substr(u + 1);
        v.push_back({e.path().wstring(), note, written(e.path()), (int)slots(e.path().wstring()).size()});
    }
    std::sort(v.begin(), v.end(), [](const Backup &a, const Backup &b) { return a.dir > b.dir; }); // dated names
    return v;
}

bool backup(const std::wstring &root, const std::wstring &backup_root, const std::wstring &note, std::wstring *made,
            std::wstring *error)
{
    if (!has_saves(root)) {
        *error = L"no saves in " + root;
        return false;
    }
    fs::path dir = new_backup_dir(backup_root, note);
    *made = dir.wstring();
    return copy_saves(root, dir, error);
}

bool replace(const std::wstring &from, const std::wstring &to, const std::wstring &backup_root, const std::wstring &note,
             std::wstring *made, std::wstring *error)
{
    if (!has_saves(from)) {
        *error = L"no saves in " + from;
        return false;
    }
    made->clear();
    if (has_saves(to)) { // what is there now is kept, not deleted
        fs::path dir = new_backup_dir(backup_root, note);
        *made = dir.wstring();
        if (!move_saves(to, dir, error)) return false;
    }
    return copy_saves(from, to, error);
}

} // namespace saves
