#pragma once
// The game's saves (project/docs/08 phase 3): Ninja Gaiden Black keeps them in the Xbox hard disk's UDATA\5443000d
// (one folder per slot, SaveMeta.xbx names it) and TDATA\5443000d (system.dat). Our hard disk has them under
// <hdd>\Partition1, xRevive under <its folder>\SaveData\Ninja Gaiden_ Black\HddData: both standard Xbox layout.
// Nothing here deletes a save: what a restore, import or export replaces is first moved into a dated backup.
#include <string>
#include <vector>

namespace saves {

struct Slot {
    std::wstring name; // SaveMeta.xbx "Name=": "18. CHAPTER 15 009:56" (slot, chapter, play time)
    long long time;    // last written, seconds since 1970
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
