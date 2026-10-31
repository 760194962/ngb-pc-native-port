// Installing the game from an Xbox disc image (XDVDFS), and checking an installed copy.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace install {

// A worker's state, read by the window while the worker writes it.
struct Progress {
    std::atomic<uint64_t> done{0}, total{0}; // bytes
    std::atomic<int> files{0}, file_count{0};
    std::atomic<int> state{0}; // 0 idle, 1 running, 2 finished, 3 failed, 4 cancelled
    std::atomic<bool> cancel{false};
    std::mutex lock;
    std::wstring message; // the current file, or why it failed
};

// Extracts every file of the image into dest (made if missing), then checks the program files (check_game).
// Runs on the calling thread; progress goes to p.
void extract(const std::wstring &iso, const std::wstring &dest, Progress &p);

// The space the image's files need, or 0 with *error set when it is not an Xbox disc image.
uint64_t image_size(const std::wstring &iso, std::wstring *error);

// The supported builds (loader/src/build_profile.h): the user's Hurricane edition x2.1 in Chinese / English and in
// Japanese / English, the basic edition x2 (Chinese / English) and the original US release they are all made from;
// and Ninja Gaiden (2004)'s disc (Ng04), a game of its own in the launcher.
enum class Edition { None, X21, X21Jp, BasicX2, Us, Ng04 };

// Whether dir holds a supported build: default.xbe and the programs it launches, with their known SHA-256 (the
// Hurricane editions: four; the basic edition and the US release: the two NES trilogies). *detail: what was found (or
// what is missing / different); *edition: which.
bool check_game(const std::wstring &dir, std::wstring *detail, Edition *edition = nullptr);

// Ninja Gaiden (2004)'s downloadable content on the Xbox hard disk (<hdd>\Partition1\TDATA\54430003): the title update
// 1.06.04 ($u\default.xbe, the program the game program runs when it is there) and Hurricane Packs 1 and 2
// ($c\5443000300030002 and ...0003).
struct DlcStatus {
    bool update = false;          // the title update, the known build
    bool pack1 = false, pack2 = false;
};
DlcStatus dlc_status(const std::wstring &hdd);

// Copies the Hurricane Packs' installer content (its folder "Ninja Gaiden DLC Installer", its "content" folder, or a
// folder holding $u and $c) into <hdd>\Partition1\TDATA\54430003. The game program signs the packs for the console at
// start (loader kernel/k_content.cpp). *error: why not.
bool install_dlc(const std::wstring &src, const std::wstring &hdd, std::wstring *error);

// What is installed there, to uninstall: the title update (id "$u", no name: the launcher names it) first, then each
// content folder of $c (its id, the name its ContentMeta.xbx gives, e.g. "Hurricane 1" or a notice downloaded in the
// game), with its size on disk.
struct ContentItem {
    std::wstring id, name;
    uint64_t bytes = 0;
};
std::vector<ContentItem> installed_content(const std::wstring &hdd);
// Deletes one of them (its whole folder). *error: why not.
bool remove_content(const std::wstring &hdd, const std::wstring &id, std::wstring *error);

} // namespace install
