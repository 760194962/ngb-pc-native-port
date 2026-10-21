[简体中文](README.md) | **English**

# NGB PC Native Port (Ninja Gaiden Black "Hurricane" · native PC loader)

An unofficial native Windows port of **vb-zh-x2.1**, the Chinese-localized "Hurricane" edition of *Ninja Gaiden Black*
(original Xbox).

`black.exe` loads your own `default.xbe` and runs the original x86 game code directly on the PC; the Xbox kernel,
Direct3D 8, DirectSound and the other system parts are reimplemented on the PC by the loader (HLE). The game code is
not interpreted instruction by instruction, and it is never modified on disk.

This project is a fork of [black-pc](https://github.com/Spark198rus/black-pc) (the native port of BLACK); it adds
support for the Ninja Gaiden main program and the programs it launches to that loader. The original BLACK NTSC-U
support is kept.

**You need your own copy of the game.** This repository contains no game code or data and no Microsoft XDK code.

## Supported builds

The loader identifies the build by the XBE's SHA-256 and does not run any other.

| Program | SHA-256 |
|---|---|
| Main program `default.xbe` (vb-zh-x2.1) | `aa3c5bec2923f83ccb37fe6a63ea5516b26d477b53c679a1620d19e4a8e4e8f6` |
| `NinjaTomoe.xbe` (bundled with the original release, Japanese) | `2aff93ad098b3b5762d294d9c17a44c36d04bfa31843e413cf5e2963f14282ff` |
| `NinjaTrilogy.xbe` (bundled with the original release, US) | `31f8efd073abba2ac339ee44f4b13011844afc470b166f826dc12f6b79ff5621` |
| `NinjaFCJ.xbe` (SNES trilogy, Japanese, added by x2.1) | `b5b8754e6008f3b2e773c1eb9afe50ee9decebf0eadb945a9953369b1b9f4f31` |
| `NinjaFCU.xbe` (SNES trilogy, US, added by x2.1) | `9fac4bb5162b8a99c65c0fbb6289ee81db4c42bdbd253807e74261a68e3399b1` |

The launched programs are entered from the main menu's arcade and SNES entries: the game calls `XLaunchNewImage`, and
the loader restarts itself with the same options and loads the target XBE.

## Status

Each item's verdict and evidence are in `project/docs/06_功能验证矩阵.md` (feature verification matrix).

- **Localization**: title, main menu, save / load pages, pause menu / weapons / move lists, chapter title cards,
  opening CG and in-engine cutscene subtitles, results screen, items / shop / map / dialogue: all verified.
- **Hurricane features**: both modes of 交差之法, the Armlet of Tranquility (宁静腕轮), costume selection with the
  left trigger, the Kitetsu blade's health drain (妖刀鬼彻), a second playthrough keeping the golden scarabs, the
  Talisman of Rebirth switch (还魂护符), bow crosshair / view, Mission Mode: all verified in play.
- **Launched programs**: the arcade and SNES versions are playable from the main menu.
- **Saves**: saving, loading and loading back the same slot verified. Saves are in the standard Xbox format; copied
  into xRevive's save folder they load directly (tested).
- **Sound**: a software DirectSound mixer on XAudio2, with reverb.
- **Performance**: in a chapter 1 battle, an average frame time of 10.45 ms on Intel Iris Xe integrated graphics
  (the 60 fps budget is 16.68 ms; r178, `project/docs/05_音频输入存档.md`).
- **Input**: XInput controllers.

### Original-game defects fixed

These also happen on the Xbox; the port fixes them. Each fix can be switched back live for comparison
(`project/docs/07_证据盘点.md`, the evidence ledger):

- Chapter 2 water: black / white jagged edges, and stretched stripes when the camera is at the water line (a
  per-vertex division moved to per pixel).
- Lava rooms: a black bar down the right (the screen copy one column short), a white blob in the heat haze, and a
  blurred band at the right edge.

## Known issues

- Mirror: seen close up in first person, a dark band with horizontal streaks runs down the right of the screen. This
  is an original-game defect (the reflection camera does not cover that part of the mirror); investigated and left
  unfixed (`project/docs/07_证据盘点.md`).
- 3D sound and reverb approximate the Xbox's and have not been checked item by item (`project/docs/07_证据盘点.md`
  #9).
- Leaderboard "new user": the Xbox goes to the dashboard here; the port restarts the main program instead, so the
  window closes and opens again.
- Running `NinjaFCJ.xbe` directly, without launch data, crashes (players entering from the main menu never take this
  path).
- Emulation rules still being verified are listed in the progress section of `project/docs/07_证据盘点.md`.

## Running

1. Extract the x2.1 image to a folder (for example `game\x21\`) and check the hash of `default.xbe`:

   ```powershell
   Get-FileHash game\x21\default.xbe -Algorithm SHA256
   ```

2. Start it (`project/play.bat` uses a command like this):

   ```
   black.exe --windowed --hdd savedata --log black.log game\x21\default.xbe
   ```

   - `--hdd <folder>`: where the Xbox hard disk lives (saved games and the game's settings).
   - `--windowed`: window mode. In the game, **Alt+Enter** switches between window and fullscreen, and the setting is
     saved.
   - `--screenshot <dir>`: timed screenshots, one every `--shot-every N` frames (default 60). When playing with
     screenshots on, set N very large (for example `--shot-every 100000000`); one screenshot a second makes the game
     stutter.

`black.exe --help` lists every option. Some of them (field of view, auto-aim, the PC SETTINGS menus and others) are
BLACK-only and have no effect on Ninja Gaiden.

## Building

The project is built with Visual Studio 2017 (x86 C++ tools) and CMake, as a 32-bit program:

```
cmake -S loader -B build -G "Visual Studio 15 2017" -A Win32
cmake --build build --config RelWithDebInfo
```

This produces `build\RelWithDebInfo\black.exe` and `blackrt.dll`. `loader/CMakePresets.json` also has upstream's
Visual Studio 2022 / 2026 presets.

## Diagnostics

Started with `--press-file <file>`, the game runs each line appended to that text file at once:

| Command | Effect |
|---|---|
| `shot:name` | capture the next whole frame as `name.png` |
| `trace` / `trace:N` | log every draw of the next frame (or the next N): states, textures, vertices, callers, the constants the vertex program reads; dump the render targets |
| `cfg:pixwatch:X,Y` | in a traced frame, read one screen pixel back before every draw and log the draws that change it |
| `cfg:pixwatchrt:address` | the same for a render target (by its data address) |
| `cfg:skipcaller:address` | skip the draws with that return address on the call stack, to find who draws something |
| `cfg:<switch>:0` / `:1` | turn a fix off / on live to compare with the original, e.g. `rhwedge`, `edgemirror`, `vsdivide`, `rhwoffset` |

The start option `--debug-frame N` names a frame to trace.

## Layout

| Path | Contents |
|---|---|
| `loader/` | loader source: `kernel/` kernel HLE; `d3d/` Direct3D 8 → 9, with NV2A vertex programs and register combiners translated to HLSL; `audio/` DirectSound mixing; `input/`; `ngb/` Ninja Gaiden address tables and patches |
| `project/docs/` | project documents (Simplified Chinese): inputs, toolchain, static analysis, loader and kernel, graphics, audio / input / saves, verification matrix, evidence ledger |
| `project/tools/` | analysis and comparison scripts: XBE census, function tables, image comparison, xemu save export and more |
| `notes/` | upstream's analysis results for BLACK |

Decompiler and disassembler output, game files and test-run folders stay local and are not in the repository.

## How it works

`black.exe` is a small stub linked at the XBE's base address, so that it holds that address range before Windows can
put anything there. `blackrt.dll` maps `default.xbe` into the range, provides the Xbox kernel functions the game
imports, and patches the entry points of the Xbox libraries (Direct3D 8, DirectSound, XAPI) to go to PC
implementations.

The Ninja Gaiden main program is an LTCG build (register arguments, inlined functions), so its library function
table was checked address by address; the launched programs use plain D3D8, and their tables are generated from
XbSymbolDatabase's results. Every emulation rule needs a basis: the original program, xemu / documentation, hardware
specifications or a measured comparison. The bases are recorded in `project/docs/07_证据盘点.md`.

## Legal

This project is not affiliated with or endorsed by Koei Tecmo, Team NINJA or Microsoft. Ninja Gaiden is a trademark
of Koei Tecmo; Xbox is a trademark of Microsoft. The repository contains no game code or data and no code from the
Microsoft Xbox Development Kit. Please use your own legally obtained copy of the game.

This project is free software under the GNU General Public License, version 2 or (at your option) any later version.
See `LICENSE`.

## Thanks

- [black-pc](https://github.com/Spark198rus/black-pc) (Spark198rus): the loader this project is built on.
- [ngbrecomp](https://github.com/gamboozino/ngbrecomp): the D3D symbol table of the Ninja Gaiden main program.
- [xemu](https://xemu.app) and [Cxbx-Reloaded](https://github.com/Cxbx-Reloaded/Cxbx-Reloaded): references for how
  the Xbox kernel and the NV2A behave (no code copied).
- [genma-pc](https://github.com/w1284603830/genma-pc): a similar native Xbox port, a reference for CLIPPLANE and other
  behaviour.
- [XbSymbolDatabase](https://github.com/Cxbx-Reloaded/XbSymbolDatabase): identifies the Xbox library functions in the
  game.

Developed by 哈雅布撒 (Bilibili) with Claude Code.
