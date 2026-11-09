cmake_minimum_required(VERSION 3.24)
if(POLICY CMP0184)
  cmake_policy(SET CMP0184 NEW) # MSVC_RUNTIME_CHECKS property
endif()
project(black_loader VERSION 2.2.12 LANGUAGES C CXX) # the release number (About box, first log line)
set(NGB_VERSION_STRING "${PROJECT_VERSION}") # test packages carry -tN (1.1.5-t..1.1.8-t, 2.2.0-t1, -t2); releases none

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "The loader must be 32-bit: configure with -A Win32 (or use the 'win32' preset).")
endif()
if(NOT MSVC)
  message(FATAL_ERROR "MSVC (cl or clang-cl) is required: the loader uses MSVC x86 inline asm and linker options.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# The version block in each program's Properties > Details (version.rc.in): product, version, description.
function(ngb_version_info target type desc file)
  set(NGB_RC_TYPE ${type})
  set(NGB_RC_DESC "${desc}")
  set(NGB_RC_FILE "${file}")
  set(NGB_RC_INTERNAL ${target})
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/version.rc.in" "${CMAKE_CURRENT_BINARY_DIR}/version_${target}.rc" @ONLY)
  target_sources(${target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/version_${target}.rc")
endfunction()

# blackrt.dll: XBE loader, HLE kernel, hooks. Everything that is real code lives here.
add_library(blackrt SHARED
  src/main.cpp
  src/log.cpp
  src/xbe.cpp
  src/image.cpp
  src/xthread.cpp
  src/hooks.cpp
  src/timing.cpp
  src/game_load.cpp
  src/fov.cpp
  src/ngb_aspect.cpp
  src/ngb_exitmenu.cpp
  src/ng04_exitmenu.cpp
  src/ngb_livedash.cpp
  src/fe/calls.cpp
  src/fe/vars.cpp
  src/fe/pc_settings.cpp
  src/fe/apt.cpp
  src/fe/pages.cpp
  src/fe/bind_list.cpp
  src/fe/pause_ui.cpp
  src/input/mouselook.cpp
  src/input/autoaim.cpp
  src/input/bindings.cpp
  src/input/padkeys.cpp
  src/input/movies.cpp
  src/audio/reverb.cpp
  src/settings.cpp
  src/perf.cpp
  src/sha1.cpp
  src/kernel/ktable.cpp
  src/kernel/k_memory.cpp
  src/kernel/k_thread.cpp
  src/kernel/k_sync.cpp
  src/kernel/k_dpc.cpp
  src/kernel/k_file.cpp
  src/kernel/k_rtl.cpp
  src/kernel/k_misc.cpp
  src/kernel/k_ngb.cpp
  src/kernel/k_crypto.cpp
  src/kernel/k_nxdk.cpp
  src/mmio_decode.cpp
  src/nv2a_display.cpp
  src/usb_ohci.cpp
  src/dashboard.cpp
  src/net/online.cpp
  src/net/nvnet.cpp
  src/net/vnet.cpp
  src/net/dashfonts.cpp
  src/fiber.cpp
  src/kernel/k_content.cpp
  src/ngb/d3d_ngb.cpp
  src/d3d/dispatch.cpp
  src/d3d/device.cpp
  src/d3d/host.cpp
  src/d3d/apicount.cpp
  src/d3d/pushbuf.cpp
  src/d3d/draw.cpp
  src/d3d/formats.cpp
  src/d3d/resources.cpp
  src/d3d/texture.cpp
  src/d3d/vshader.cpp
  src/d3d/nv2a_vsh.cpp
  src/d3d/nv2a_psh.cpp
  src/d3d/pshader.cpp
  src/d3d/ffp_ps.cpp
  src/d3d/shader_cache.cpp
  src/d3d/perfhud.cpp
  src/audio/dsound.cpp
  src/audio/output.cpp
  src/input/xinput.cpp
)
target_include_directories(blackrt PRIVATE src)
target_compile_definitions(blackrt PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE
  BLACK_VERSION="${NGB_VERSION_STRING}")
target_compile_options(blackrt PRIVATE /W3 /utf-8 /EHsc)
target_link_libraries(blackrt PRIVATE ntdll bcrypt d3d9 d3dcompiler windowscodecs ole32 user32 gdi32 winmm dbghelp advapi32 shell32 pdh dxgi psapi ws2_32 iphlpapi)

# SDL3 (zlib licence), linked statically, only its gamepad side: the controllers XInput does not see (DualShock 3/4,
# DualSense, Switch Pro and Joy-Con, DirectInput pads and adapters; input/sdlpad.cpp). NGB_SDL3_DIR: an unpacked
# SDL3-3.4.18 to build from instead of downloading the release.
set(NGB_SDL3_DIR "" CACHE PATH "SDL3 source tree (empty: download SDL3 3.4.18)")
if(NGB_SDL3_DIR)
  set(FETCHCONTENT_SOURCE_DIR_SDL3 "${NGB_SDL3_DIR}")
endif()
foreach(_off SDL_SHARED SDL_TEST_LIBRARY SDL_TESTS SDL_EXAMPLES SDL_INSTALL SDL_AUDIO SDL_VIDEO SDL_GPU SDL_RENDER
             SDL_CAMERA SDL_DIALOG SDL_TRAY SDL_POWER)
  set(${_off} OFF CACHE BOOL "" FORCE)
endforeach()
set(SDL_STATIC ON CACHE BOOL "" FORCE)
include(FetchContent)
FetchContent_Declare(SDL3
  URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz
  URL_HASH SHA256=9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3
  EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(SDL3)
target_sources(blackrt PRIVATE src/input/sdlpad.cpp)
target_link_libraries(blackrt PRIVATE SDL3::SDL3-static)
# Xbox SEH handlers run through the same fs:[0] chain; no SafeSEH tables anywhere in the process.
ngb_version_info(blackrt VFT_DLL "Ninja Gaiden Black runtime" "blackrt.dll")
target_link_options(blackrt PRIVATE /SAFESEH:NO /MAP) # blackrt.map: port addresses in crash logs -> functions

# black.exe: tiny stub with no CRT. Linked at 0x10000 with a large BSS so its own image owns the
# XBE's address range (0x10000-0x587a40) before Windows can place heaps or stacks there.
# blackrt.dll then maps the XBE over this image (headers included) and never returns.
# A Windows-subsystem program: no console window; started from a command prompt it attaches to that one (main.cpp).
add_executable(black stub/stub.c)
# The program is named after the game it runs (the user's names: 2026-10-22 no "X21"; 2026-10-31 "Any", for every
# supported build, was "Hurricane").
set_target_properties(black PROPERTIES OUTPUT_NAME "Ninja Gaiden Black Any")
# PORT_ICON: the programs' icon (icon group 1, also the windows'): the Dragon Sword from the game's weapons page,
# launcher/art/icon.ico, in the repository and the release at the user's decision (2026-10-22; CLAUDE.md, README
# Legal). Another .ico can be given; empty builds without an icon.
set(PORT_ICON "${CMAKE_CURRENT_SOURCE_DIR}/../launcher/art/icon.ico" CACHE FILEPATH "the programs' .ico; empty: no icon")
if(PORT_ICON AND EXISTS "${PORT_ICON}")
  file(TO_CMAKE_PATH "${PORT_ICON}" _port_icon)
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/port_icon.rc" "1 ICON \"${_port_icon}\"\n")
  target_sources(black PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/port_icon.rc")
endif()
ngb_version_info(black VFT_APP "Ninja Gaiden Black" "Ninja Gaiden Black Any.exe")
target_link_libraries(black PRIVATE blackrt kernel32)
set_property(TARGET black PROPERTY MSVC_RUNTIME_CHECKS "")
target_compile_options(black PRIVATE /GS- /Gs999999)
target_link_options(black PRIVATE
  /BASE:0x10000 /FIXED /DYNAMICBASE:NO /NXCOMPAT:NO /SAFESEH:NO /LARGEADDRESSAWARE
  /NODEFAULTLIB /ENTRY:StubEntry /SUBSYSTEM:WINDOWS /MANIFEST:NO /INCREMENTAL:NO)

# The launcher (project/docs/08): installs and starts the game, display / graphics card / controls / saves.
add_executable(launcher WIN32 ../launcher/src/main.cpp ../launcher/src/strings.cpp ../launcher/src/install.cpp
  ../launcher/src/saves.cpp ../launcher/src/keys.cpp
  ../launcher/src/launcher.rc)
set_target_properties(launcher PROPERTIES OUTPUT_NAME "Ninja Gaiden Black Any Launcher")
ngb_version_info(launcher VFT_APP "Ninja Gaiden Black Launcher" "Ninja Gaiden Black Any Launcher.exe")
target_compile_definitions(launcher PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS _WIN32_WINNT=0x0A00 WINVER=0x0A00
  GAME_VERSION="${NGB_VERSION_STRING}") # the game program's release number, for the About page
target_compile_options(launcher PRIVATE /W3 /utf-8 /EHsc)
target_link_libraries(launcher PRIVATE d2d1 dwrite windowscodecs ole32 shell32 user32 advapi32 bcrypt dxgi ws2_32 iphlpapi)
if(PORT_ICON AND EXISTS "${PORT_ICON}")
  target_sources(launcher PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/port_icon.rc") # the same icon as the game
endif()
