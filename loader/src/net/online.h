// Xbox Live through Insignia (project/docs/09): opt-in (black.ini online = 1 with the player's own registered console:
// online_eeprom = the xemu eeprom.bin, online_hdd = the xemu hard disk image whose config area holds the Live
// accounts). Both files are only read. Off, nothing here runs and the game sees no network, as before.
#pragma once

#include <cstdint>

bool online_enabled();         // set up and running: the NIC, the console identity, the accounts
bool online_init();            // at start, before the game: false = stay offline (the reason is logged)

// The console's factory EEPROM values for ExQueryNonVolatileSetting (0x100 serial, 0x101 MAC, 0x102 online key,
// 0x103 AV region, 0x104 game region): false = not online / not one of these
bool online_factory_setting(uint32_t index, uint32_t *type, uint8_t *out, uint32_t *size);

// \Device\Harddisk0\Partition0 bytes [0x1000, 0x80000) (config sectors 8 on: network settings, machine and user
// accounts) come from the registered disk, in memory; writes there stay in memory for the run.
void online_partition0_read(uint64_t offset, uint8_t *buf, uint32_t len);
bool online_partition0_write(uint64_t offset, const uint8_t *buf, uint32_t len); // true = taken (all inside)

bool kcrypto_selftest(); // kernel/k_crypto.cpp
