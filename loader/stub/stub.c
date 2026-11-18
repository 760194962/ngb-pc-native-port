/*
 * The game programs' stub: address-space owner for the Xbox image. Built twice (loader/CMakeLists.txt):
 * Ninja Gaiden Black.exe with NGB_GAME=1 and Ninja Gaiden.exe (2004) with NGB_GAME=2; started without a game on the
 * command line, each looks for its own game (blackrt.dll, BlackMainFor). Without NGB_GAME it calls BlackMain, as the
 * old Ninja Gaiden Black Any.exe did.
 *
 * Linked at base 0x10000 (/FIXED, no CRT). The uninitialized array below makes this image span
 * 0x10000 up to about 0x800000, which covers the XBE (0x10000-0x587a40). Windows maps the exe before
 * it creates the process heap, the main stack or anything else, so the range is guaranteed free.
 * Ninja Gaiden Black: image 0x10000-0xE17000 (size_of_image 0xE07000); Ninja Gaiden (2004) title update 1.06.04:
 * 0x10000-0xE9D000 (size_of_image 0xE8C020), so the reserve is 0xEA0000 (only address space: the pages are
 * committed when touched).
 * blackrt.dll (loaded through the import table) copies the XBE over this whole image, including these
 * PE headers and this code, so nothing here may run after BlackMain is entered. BlackMain never returns.
 */
#include <windows.h>

#define XBOX_RESERVE_SIZE 0x00EA0000u

static unsigned char g_xbox_image_reserve[XBOX_RESERVE_SIZE];

#ifdef NGB_GAME
__declspec(dllimport) int __cdecl BlackMainFor(void *reserve, unsigned reserve_size, int game);

void __cdecl StubEntry(void)
{
    ExitProcess((UINT)BlackMainFor(g_xbox_image_reserve, XBOX_RESERVE_SIZE, NGB_GAME));
}
#else
__declspec(dllimport) int __cdecl BlackMain(void *reserve, unsigned reserve_size);

void __cdecl StubEntry(void)
{
    ExitProcess((UINT)BlackMain(g_xbox_image_reserve, XBOX_RESERVE_SIZE));
}
#endif
