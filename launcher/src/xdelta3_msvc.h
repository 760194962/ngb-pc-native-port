// Forced into every file that includes xdelta3.h (loader/CMakeLists.txt): what its GCC-only parts need under MSVC.
#pragma once
#define PRINTF_ATTRIBUTE(x, y)
