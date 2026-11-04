// Xbox Live through Insignia (project/docs/09): opt-in (black.ini online = 1). The game uses the port's own console in
// the hard disk folder (online_eeprom.bin, online_accounts.bin), imported from the player's console registered in xemu
// (black.ini online_eeprom = the xemu eeprom.bin, online_hdd = the xemu hard disk image; both only read). Off, nothing
// here runs and the game sees no network, as before.
#pragma once

#include <cstdint>
#include <string>

bool online_enabled();         // set up and running: the NIC, the console identity, the accounts
const std::wstring &online_off_reason(); // black.ini online = 1 but online_init failed: why, for the player ("" otherwise)
bool online_init();            // at start, before the game: false = stay offline (the reason is logged)
// --online-check: checks black.ini's online files and writes what was found (no secrets) to out_path for the launcher
bool online_check(const std::wstring &out_path);
// --online-import: imports the console registered in xemu (black.ini online_eeprom / online_hdd, only read) into the
// port's own (the hard disk folder's online_eeprom.bin and online_accounts.bin): its accounts the port lacks. What was
// imported, skipped or wrong goes to out_path for the launcher. false on an error.
bool online_import(const std::wstring &out_path);
// --online-new-console: a console of the port's own (a new EEPROM, an empty config area) in the hard disk folder when it
// has none; to be registered with Insignia by the Setup Assistant. "created = 1" or "error = why" to out_path.
bool online_new_console(const std::wstring &out_path);
// The console's whole EEPROM (256 bytes) when online play is on, for ExQueryNonVolatileSetting(0xFFFF)
bool online_whole_eeprom(uint8_t out[256]);
// --online-dash-from IMAGE: the Xbox Live dashboard's files copied from that Xbox hard disk image into the hard disk
// folder when not there yet ("dashboard = ok" / "missing" to out_path)
bool online_dash_from(const std::wstring &image, const std::wstring &out_path);

// The console's factory EEPROM values for ExQueryNonVolatileSetting (0x100 serial, 0x101 MAC, 0x102 online key,
// 0x103 AV region, 0x104 game region): false = not online / not one of these
bool online_factory_setting(uint32_t index, uint32_t *type, uint8_t *out, uint32_t *size);

// \Device\Harddisk0\Partition0 bytes [0x1000, 0x80000) (config sectors 8 on: network settings, machine and user
// accounts) come from the registered disk, in memory; writes there stay in memory for the run.
void online_partition0_read(uint64_t offset, uint8_t *buf, uint32_t len);
bool online_partition0_write(uint64_t offset, const uint8_t *buf, uint32_t len); // true = taken (all inside)

bool kcrypto_selftest(); // kernel/k_crypto.cpp
