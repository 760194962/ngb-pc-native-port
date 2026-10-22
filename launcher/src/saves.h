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

} // namespace saves
