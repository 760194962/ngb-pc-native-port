#pragma once
// The game's saves (project/docs/08 phase 3): Ninja Gaiden Black keeps them in the Xbox hard disk's UDATA\5443000d
// (one folder per slot, SaveMeta.xbx names it) and TDATA\5443000d (system.dat). Our hard disk has them under
// <hdd>\Partition1, xRevive under <its folder>\SaveData\Ninja Gaiden_ Black\HddData: both standard Xbox layout.
// Nothing here deletes a save: what a restore, import or export replaces is first moved into a dated backup.
#include <string>
#include <vector>

namespace saves {

struct Slot {
    std::wstring name;   // SaveMeta.xbx "Name=": "18. CHAPTER 15 009:56" (slot, chapter, play time)
    long long time;      // last written, seconds since 1970
    std::wstring folder; // its folder in UDATA\<title> (12 hex digits: the game's hash of the whole name)
    bool here = true;    // signed for this port (signed_here; filled only where it is shown)
};

struct Backup {
    std::wstring dir;  // the backup's folder (UDATA, TDATA inside)
    std::wstring note; // what made it: "" by hand, "before-restore", "before-import", "xrevive-before-export"
    long long time;    // when it was made
    int slots;
};

// The game whose saves these are: its title id folder name ("5443000d" Ninja Gaiden Black, the default; "54430003"
// Ninja Gaiden 2004). Downloadable content in TDATA ($u, $c) is no save: nothing here copies or moves it.
void set_title(const std::wstring &title);
std::wstring our_root(const std::wstring &hdd);           // <hdd>\Partition1
std::wstring xrevive_root(const std::wstring &folder);    // <xRevive folder>\SaveData\Ninja Gaiden_ Black\HddData
bool has_saves(const std::wstring &root);                 // a slot or the system save there (not just icons)
std::vector<Slot> slots(const std::wstring &root);        // newest first
std::vector<Backup> backups(const std::wstring &backup_root); // newest first

// Ninja Gaiden Black's slots that claim the same slot number, each group oldest first. The game files a slot under
// the number its name starts with (x2.1 0x1DA220: wcstol(name) - 1 into a 30-entry table), a later one with the same
// number replacing the earlier in the table, so only one of them is shown; overwriting it deletes only that one. The
// game itself never makes two (a new save takes the first free number, 0x1DA0B0; an overwrite deletes the old first,
// 0x1DA1B0); they come from copying another save folder's slots in by hand: a slot's folder is a hash of its whole
// name (0x328CCB), so two names with the same number but another chapter or time never collide as folders.
std::vector<std::vector<Slot>> same_number(const std::vector<Slot> &slots);

// Whether a slot's save000.dat is signed for this port (resign below): false for one from another Xbox, xemu or player
bool signed_here(const std::wstring &root, const std::wstring &folder);

// Removes one of those: the slot folder `folder` of root's UDATA\<title>, after checking, on the disk as it is now,
// that the name is one the game makes (12 hex digits), that it is a real folder straight under UDATA\<title> (not a
// link), that it still holds the save named `name`, and that its number still has another save, so a number's last
// save is never removed; then root's saves are copied whole into a backup ("before-remove-dup", *made) and only then
// the folder goes (read-only files too). *changed: the check failed (nothing was touched; read the saves again).
bool remove_duplicate(const std::wstring &root, const std::wstring &backup_root, const std::wstring &folder,
                      const std::wstring &name, std::wstring *made, std::wstring *error, bool *changed);

// A copy of root's saves in backup_root\<date>[_note]; *made = its folder
bool backup(const std::wstring &root, const std::wstring &backup_root, const std::wstring &note, std::wstring *made,
            std::wstring *error);
// to's saves moved into a backup (note), then from's copied to `to` (restore a backup, import, export)
bool replace(const std::wstring &from, const std::wstring &to, const std::wstring &backup_root, const std::wstring &note,
             std::wstring *made, std::wstring *error);

// Saves signed for this port. Ninja Gaiden Black and Ninja Gaiden 2004 sign system.dat (TDATA) and each slot's
// save000.dat (UDATA) with XAPI's non-roamable signature in their first 20 bytes: HMAC-SHA1(console key,
// HMAC-SHA1(title key, the rest of the file)) (2026-10-31: the game's verify call given file[20:], loader --probe; the
// encryption's seed is in the file, so nothing else depends on the console). The port's keys are 16 zero bytes both
// (loader kernel/ktable.cpp: XboxSignatureKey, and the console key whatever online play does). A save made on another
// Xbox, in xemu or by another player, or with online play on before 1.2.1, has another signature and reads as damaged.
// resign signs every such file again; when any is to change, the title's saves are first copied into a backup
// ("before-resign", *made). *count: files signed again (0: all were right).
bool resign(const std::wstring &root, const std::wstring &backup_root, int *count, std::wstring *made, std::wstring *error);

} // namespace saves
