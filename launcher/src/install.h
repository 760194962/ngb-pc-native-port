// Installing the game from an Xbox disc image (XDVDFS), and checking an installed copy.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

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
// Japanese / English, the basic edition x2 (Chinese / English) and the original US release they are all made from.
enum class Edition { None, X21, X21Jp, BasicX2, Us };

// Whether dir holds a supported build: default.xbe and the programs it launches, with their known SHA-256 (the
// Hurricane editions: four; the basic edition and the US release: the two NES trilogies). *detail: what was found (or
// what is missing / different); *edition: which.
bool check_game(const std::wstring &dir, std::wstring *detail, Edition *edition = nullptr);

} // namespace install
