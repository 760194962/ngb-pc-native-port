// What Ninja Gaiden 2004's title updater (update.xbe) needs from outside the game:
// - the Xbox dashboard's fonts (C:\fonts\*.xtf), which it draws its text with: copied once from the player's
//   registered xemu hard disk image (qcow2 or raw, read only) into <hdd>\Partition2\fonts;
// - the disc's default.xbe and downloader.xbe as they were signed: the disc carries each with one byte of .text
//   changed after signing (0x7D jge -> 0xEB jmp; the section digests in their headers are of 0x7D), and the title
//   update's patches (PA19 deltas) are made from the signed bytes. Copies with that byte back go into a cache folder
//   that replaces the disc's files for the updater only (g_cfg.game_override).
#pragma once

#include <string>

// true when <partition2>\fonts holds them (already, or copied now from image)
bool dash_fonts_ensure(const std::wstring &image, const std::wstring &partition2);

// The dashboard folder dir of C: (e.g. "xodash", the Xbox Live dashboard) copied whole from the registered image into
// <partition2>\<dir> when <partition2>\<dir>\<needed> is not there yet. true when it is there (already, or now). The copy
// goes into <dir>.part first and is renamed when complete, so an interrupted copy is never taken for one.
bool dash_dir_ensure(const std::wstring &image, const std::wstring &partition2, const char *dir, const wchar_t *needed);

// The file name of C:'s top folder (e.g. "xboxdash.xbe") copied from the registered image into <partition2>\<name> when
// it is not there yet (through <name>.part). true when it is there.
bool dash_file_ensure(const std::wstring &image, const std::wstring &partition2, const char *name);

// The same three (C:\fonts, C:\xodash, C:\xboxdash.xbe) from a folder holding a copy of an Xbox's C: (taken off a real
// Xbox by FTP, say) instead of an image. true when all three are in <partition2>.
bool dash_from_folder(const std::wstring &root, const std::wstring &partition2);

// The signed copies of the disc's two programs in <cache>, and g_cfg.game_override pointed at it (when it is not set
// already). false: the disc's files are not the known ones (nothing changed).
bool updater_signed_sources(const std::wstring &game_dir, const std::wstring &cache);
