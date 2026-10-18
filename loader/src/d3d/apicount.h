#pragma once
// --api-count (diagnosis): Direct3D 9 calls by kind, per-frame averages logged every 600 frames (d3d/apicount.cpp).
#include <cstdint>

struct IDirect3DDevice9;

namespace apicount {
void install(IDirect3DDevice9 *dev);
void frame(uint32_t frame_no);
} // namespace apicount
