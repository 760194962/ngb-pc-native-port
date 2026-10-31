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

// The signed copies of the disc's two programs in <cache>, and g_cfg.game_override pointed at it (when it is not set
// already). false: the disc's files are not the known ones (nothing changed).
bool updater_signed_sources(const std::wstring &game_dir, const std::wstring &cache);
