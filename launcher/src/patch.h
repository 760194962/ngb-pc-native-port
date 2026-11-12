// The Hurricane patches (the translator's xdelta files: basic va-zh-x2, Hurricane vb-zh-x2.1 / vb-jp-x2.1) applied by
// the launcher. They are made against one image, "NINJA GAIDEN BLACK.iso" (SHA-1 d35859d1...): extract-xiso 2.7.1's
// rewrite of the US release's Redump disc. The launcher takes either that image or the Redump disc itself, which it
// reads as that image (ngb_base_layout.inc, tools/gen_ngb_base_layout.py).
#pragma once

#include "install.h"

#include <cstdint>
#include <string>

namespace patch {

enum class Source { None, Base, Redump };

// What the image is for the patches: the base image, the US Redump disc, or neither (*why: what was found).
Source identify(const std::wstring &image, std::wstring *why);

// Writes out_iso: the xdelta file applied to the image (the base image, or the Redump disc read as it). Progress in
// p (done / total: bytes written of the base image's size; message: the patch's name). *error: why not. Returns false
// on cancel too (p.cancel), with out_iso removed.
bool apply(const std::wstring &image, const std::wstring &xdelta, const std::wstring &out_iso, install::Progress &p,
           std::wstring *error);

// The base image's size: the room a patched image needs before it is unpacked
uint64_t image_bytes();

// One file patched (a few MB, in memory): out written from src and the xdelta file (Ninja Gaiden 2004: an xdelta
// made against the 1.06 update's default.xbe). *error: why not; out is not left behind on failure.
bool apply_file(const std::wstring &src, const std::wstring &xdelta, const std::wstring &out, std::wstring *error);

} // namespace patch
