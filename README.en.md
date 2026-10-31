[简体中文](README.md) | **English**

# NGB PC Native Port (Ninja Gaiden Black · native PC loader)

An unofficial native Windows port of *Ninja Gaiden Black* (original Xbox): the original US release, and the basic
edition va-zh-x2 and the Hurricane editions vb-zh-x2.1 / vb-jp-x2.1 made from it.

`Ninja Gaiden Black Any.exe` loads your own `default.xbe` and runs the original x86 game code directly on the PC; the Xbox kernel,
Direct3D 8, DirectSound and the other system parts are reimplemented on the PC by the loader (HLE). The game code is
not interpreted instruction by instruction, and it is never modified on disk.

This project is a fork of [black-pc](https://github.com/Spark198rus/black-pc) (the native port of BLACK); it adds
support for the Ninja Gaiden main program and the programs it launches to that loader. The original BLACK NTSC-U
support is kept.

**You need your own copy of the game.** This repository contains no game code or data and no Microsoft XDK code.

## Supported builds

The loader identifies the build by the XBE's SHA-256 and does not run any other. Four disc images are supported: the
original US release and three editions made from it by 哈雅布撒 (Hayabusa).

| Main program `default.xbe` | Language | Bundled programs | SHA-256 |
|---|---|---|---|
| Original US release NINJA GAIDEN BLACK (NTSC-U) | English | 2 arcade | `df1d2af0154d771f42b77ec4df03a862417ac457ff0d39fec846c40cb55ffbda` |
| Basic edition va-zh-x2 | Chinese / English | 2 arcade | `00cef52546618eb0149d7895f9042403e95f361b5483aac38933e799c67a50e1` |
| Hurricane edition vb-zh-x2.1 | Chinese / English | 2 arcade + 2 SNES | `aa3c5bec2923f83ccb37fe6a63ea5516b26d477b53c679a1620d19e4a8e4e8f6` |
| Hurricane edition vb-jp-x2.1 | Japanese / English | 2 arcade + 2 SNES | `535778c8a4623195c8f320b85d2d139f20ff13c020d40150adaed54cfdeffc38` |

The three editions are the US `default.xbe` patched: the same sections at the same addresses, the Direct3D, DirectSound
and other library sections byte for byte identical, and no change at an address the loader uses, so all four share
the same tables and fixes. The bundled programs are identical in every build:

| Bundled program | SHA-256 |
|---|---|
| `NinjaTomoe.xbe` (bundled with the original release, Japanese) | `2aff93ad098b3b5762d294d9c17a44c36d04bfa31843e413cf5e2963f14282ff` |
| `NinjaTrilogy.xbe` (bundled with the original release, US) | `31f8efd073abba2ac339ee44f4b13011844afc470b166f826dc12f6b79ff5621` |
| `NinjaFCJ.xbe` (SNES trilogy, Japanese, added by the Hurricane editions) | `b5b8754e6008f3b2e773c1eb9afe50ee9decebf0eadb945a9953369b1b9f4f31` |
| `NinjaFCU.xbe` (SNES trilogy, US, added by the Hurricane editions) | `9fac4bb5162b8a99c65c0fbb6289ee81db4c42bdbd253807e74261a68e3399b1` |

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
- **Exit game**: the main menu has a last row, 退出游戏 (EXIT GAME on the English menus), that asks "Exit the game?" in
  the game's own confirmation box: Ⓐ yes returns to the desktop, Ⓑ no goes back to the menu.
- **Online leaderboards (optional)**: with your own console and account registered with Insignia in xemu, the game's
  Ranking menu signs in and shows the real leaderboards (tested). Off by default; see "Online" below.
- **Saves**: saving, loading and loading back the same slot verified. Saves are in the standard Xbox format; copied
  into xRevive's save folder they load directly (tested).
- **Sound**: a software DirectSound mixer on XAudio2, with reverb.
- **Performance**: in a chapter 1 battle, an average frame time of 10.45 ms on Intel Iris Xe integrated graphics
  (the 60 fps budget is 16.68 ms; r178, `project/docs/05_音频输入存档.md`).
- **Input**: Xbox and other XInput controllers, plus PS3 / PS4 / PS5 controllers, the Switch Pro Controller, Joy-Cons and DirectInput pads (through SDL3); all with rumble.

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

**Requirements**: Windows 10 or 11 (64-bit is fine; the program itself is 32-bit), a Direct3D 9 graphics card
(integrated graphics work too), and your own game image (one of the four above). Nothing else to install: Direct3D, XAudio2 and
XInput come with Windows, and the C++ runtime is built into the programs.

**With the launcher (recommended)**:

1. Extract the release zip. Its `NGB-Hurricane\` folder can go anywhere:

   ```
   NGB-Hurricane\
     Ninja Gaiden Black Any Launcher.exe   the launcher: start here
     Ninja Gaiden Black Any.exe            the game program, with blackrt.dll
     game\                                 where the game is installed (by the launcher)
     savedata\                             the Xbox hard disk: saves and the game's settings (made on the first run)
   ```

2. Open the launcher; on the Install page choose your game image (`.iso`) and install it to the game folder (by default
   `game` beside the launcher). The install checks the game programs and can move the image to the Recycle Bin. A
   game already extracted: choose its folder on the same page.
   Once installed, double-clicking `Ninja Gaiden Black Any.exe` starts the game too (it finds what the launcher
   installed).
   Upgrading from 1.1.12 or earlier: the programs were renamed (they were `Ninja Gaiden Black Hurricane.exe` / `…Hurricane
   Launcher.exe`); after extracting over the old folder you can delete those two, and make desktop shortcuts again. Saves
   and settings are kept.
3. Adjust the Display, Graphics and Controls pages if you like (changes apply once saved; each "?" explains its
   setting), then start from the Play page.

The Saves page backs up and restores saves and imports from / exports to xRevive; About has the version and the legal note; Support has the author's WeChat Pay and Alipay codes (giving is entirely optional).

**With run.bat (for development)**: `run.bat` in the repository root starts the game directly, without the launcher,
from this layout:

```
ngb-pc-native-port\
  run.bat
  build\RelWithDebInfo\Ninja Gaiden Black Any.exe   the build, with blackrt.dll (see "Building")
  game\x21\default.xbe                                       your own extracted x2.1 image (all of its files)
  savedata\                                                  the Xbox hard disk
  logs\                                                      the log black.log, screenshots shots\, the command file presses.txt
```

Check that the hash of `default.xbe` matches the table above: `Get-FileHash game\x21\default.xbe -Algorithm SHA256`.
If the program or the game is missing, `run.bat`'s window says what is missing.

In the game, **Alt+Enter** switches between window and fullscreen, and the setting is saved. Fullscreen is either
exclusive (`fullscreen = 2` in `black.ini`) or borderless (`fullscreen = 1`); the launcher's Display page chooses.

On a slow machine, set **Fullscreen size** on the Display page to "Window size" (`fullscreen_res = window`, default
`screen`): fullscreen and borderless then show the picture at the window size (the screen's shape, shrunk to fit it)
and scale it up over the screen with linear filtering, so the last full-screen passes of each frame have fewer pixels;
with the render resolution at "Window size" the 3D picture is drawn at that size too. The display's resolution stays:
borderless, Windows scales it; exclusive, the graphics card does before showing it. Measured on integrated graphics
(1920x1200 screen, 3x render, 1280x720 window): borderless 9.37 -> 8.47 ms a frame on average.

**Graphics card**: the dedicated one by default, the integrated one where there is none. On its first run, if Windows'
graphics settings have no preference for the program yet, it registers "high performance" for itself; to change it
later, use the launcher's Graphics page or Windows Settings > System > Display > Graphics, and the program never
overrides that choice. The log line
`host: Direct3D 9 device ... on '<card>'` names the card in use.

**Other locations**: drop `default.xbe` onto `run.bat`, or set the paths in a command prompt first:

```bat
set EXE=D:\ngb\Ninja Gaiden Black Any.exe
set GAME=D:\ngb\x21\default.xbe
set HDD=D:\ngb\savedata
set LOGDIR=D:\ngb\logs
run.bat
```

**Saved games** are in `savedata\Partition1\`: `UDATA\5443000d\` holds the save slots, `TDATA\5443000d\system.dat` the
system save. To back them up, copy the whole `savedata` folder.

**Screenshots**: append a line `shot:name` to `logs\presses.txt`, and the next frame is saved as `logs\shots\name.png`.
More commands under "Diagnostics".

**Graphics settings** (the launcher's Graphics page, or `black.ini`): `shadow` scales the shadow map (1 = the Xbox's
128x128, default 4 = 512x512, up to 16); `vsync = 1` waits for the display's refresh in exclusive fullscreen (no
tearing; a window or borderless fullscreen never tears anyway; measured at almost no cost; off by default); `perfhud = 1` shows frame rate, GPU, CPU, memory and disk use in the picture's top-right
corner (off by default); `aniso` anisotropic texture filtering (1 = off, 2-16). On the command line: `--vsync`,
`--perfhud`, `--aniso N`; command-line values hold for that run only and are not written back to `black.ini`.

**Picture shape**: with `widescreen = 1`, `aspect = 16:9` (default), `16:10` or `21:9` (experimental), on the command
line `--aspect W:H`; `widescreen = 0` is 4:3. At 21:9 the 3D view widens (the height stays), at 16:10 it is a little
narrower; the HUD, text, menus, the title screen and the intro movie keep a 16:9 shape (black at the sides at 21:9, above
and below at 16:10); at 21:9 screens such as the shop and game over have not all been checked yet. Ninja Gaiden Black
itself only: the bundled SNES / arcade programs stay 16:9. Most ultrawides are not exactly 21:9: the launcher's
aspect list has `64:27` (5120×2160, 2560×1080), `43:18` (3440×1440) and `12:5` (3840×1600), and with the matching one
the 3D picture fills the screen (the HUD and menus stay a 16:9 part in the middle); "Match the monitor" writes this PC's
main monitor's shape to `aspect`. Shapes wider than 16:9 are experimental; 1.5:1 to 2.5:1 are supported, wider ones
such as 32:9 are not.

**Keyboard and controller**: the game itself reads only its controller; each key presses one controller input (by
default WASD to move, arrows for the camera, J / K to attack, Space to jump, Esc for Start...). The launcher's Controls
page changes them and can remap the controller's buttons. In `black.ini`: `kb_<input> = Key[,Key]` and
`padmap_<input> = <input>`. Keys do not reach the game while Alt is held.

**Supported controllers**: Xbox and other XInput controllers are read through Windows' XInput; the rest through the
built-in SDL3 (zlib licence, linked into `blackrt.dll`), with no DS4Windows or similar needed: PS4 / PS5 controllers
(USB or Bluetooth), the Switch Pro Controller, Joy-Cons (a pair as one controller, or one held sideways), DirectInput pads
and USB adapters for PS1 / PS2 pads (whether an adapter rumbles depends on the adapter). A PS3 controller needs its
driver first (DsHidMini, for example). Buttons follow the Xbox layout by position: bottom A, right B, left X, top Y, so
the button marked B on a Nintendo pad is A; a PlayStation pad's touchpad click is Back. Several controllers can be
plugged in at once and all drive the same character. `--no-sdl-pads` keeps to XInput.

**Shader cache**: the first time the program meets a shader it compiles it on the spot, which can stutter; the result
is kept in `cache\shaders\` next to the program and read into memory at the next start, so it stutters no more.
Deleting the folder does no harm; the shaders are just compiled again.

The command `run.bat` runs (you can also type it yourself):

```
"Ninja Gaiden Black Any.exe" --windowed --hdd savedata --log logs\black.log --screenshot logs\shots --shot-every 100000000 --press-file logs\presses.txt game\x21\default.xbe
```

- `--shot-every 100000000` keeps timed screenshots from ever happening; screenshots are taken only on a `shot:`
  request. The default of 60 is one a second, which makes the game stutter.
- `"Ninja Gaiden Black Any.exe" --help` lists every option. Some of them (field of view, auto-aim, the PC SETTINGS menus and others) are
  BLACK-only and have no effect on Ninja Gaiden.

**Online (Insignia leaderboards, optional)**

The port has no account of its own: it borrows the "console" you already registered in xemu, and the game's own online
code runs unchanged, as it does in xemu.

1. **Register with Insignia in xemu first** (once): follow Insignia's own instructions to register your console and
   account with xemu, and check that you can sign in to Xbox Live there. Your EEPROM (`eeprom.bin`) and hard disk image
   (`xbox_hdd.qcow2`) then hold the console's identity and the account. "How to register" on the launcher's Online
   page (Signing in card) lists the steps, taken from Insignia's connection guide.
2. **Close xemu**: one console cannot sign in twice, and the port stays offline while xemu runs.
3. **The launcher's Online page**:
   - Select On, then choose the two files with "Choose file…". Both files are only read.
   - The Console row shows the check: whether the identity is valid, whether the console is registered, and which accounts it holds.
   - Press Save.

   The page writes `black.ini` next to the game program (`online = 1`, `online_eeprom = …`, `online_hdd = …`), which you
   can also edit by hand.
4. **In the game**: main menu → Ranking, pick your gamertag to sign in, and view the leaderboards. When it connects, the
   picture's bottom-right corner says so.

**Do you need InsigniaDNS.exe?** Not by default. First, two things with nearly the same name:

- **Insignia's public DNS**: Insignia's DNS server on the internet (`46.101.64.175`); nothing to install.
- **Local InsigniaDNS.exe**: a small program from Insignia running on your PC; it answers on that PC's LAN address.

The Online page's DNS "Lookups" choose between them:

- **Insignia public DNS** (the default, recommended): the port sends every DNS lookup of the game to it (change the
  address with `online_dns = …`), whatever xemu was set to, with no InsigniaDNS.exe. Only inside the port; the
  console's files are not changed.
- **Local InsigniaDNS.exe**: every lookup goes to a PC running InsigniaDNS.exe, at the InsigniaDNS.exe address entered.
  "This PC" finds InsigniaDNS.exe running on this PC and fills in its address. It listens on the LAN address
  (`192.168.x.x`) only; `127.0.0.1` does not work. The address is kept in the port's settings, not taken from the DNS
  entered in xemu, so on another PC or network change it here. Use it with a proxy's TUN on (see below).

The DNS in use row shows the DNS the game will actually use: "Automatic 46.101.64.175 (Insignia's public DNS)" or
"Manual 192.168.x.x (local InsigniaDNS.exe)", following the choice above. The DNS entered in the console's setting in xemu is not used and
no longer shown: the port sends every DNS lookup of the game to the address shown here.

**If it cannot connect:**

- Press Check network on the Online page first. It asks for `AS.XBOXLIVE.COM` the way the lookups are set:
  - an address: DNS works;
  - `198.18.x.x`: a proxy's (Clash etc.) TUN / fake-ip mode captures DNS;
  - no answer: check the network, or the InsigniaDNS.exe on that PC.

  To keep TUN on: **turn TUN off, start InsigniaDNS.exe, turn TUN back on**, then choose "Local InsigniaDNS.exe".
  InsigniaDNS.exe on a LAN address (`192.168.x.x`) is not captured by TUN. Or just turn TUN off and use Insignia's
  public DNS.
- The game also shows why it cannot connect in the bottom-right corner: DNS captured, DNS not answering, xemu running,
  files not valid.
- In the game's "cannot connect" / "repair gamertag" boxes, Ⓐ now cancels like Ⓑ. Their Ⓐ used to open the Xbox
  dashboard, which the port does not have.
- In `black.log`, search for `online:` and `[net]` to see whether the identity was read and what DNS answered.

**How online play works (in plain words)**

The original Xbox Live closed long ago. Insignia is a replacement run by the community: its servers act as the old Xbox
Live so that old games can go online again. To reach it, the port does four things:

1. **The game's own online code.** The game already has everything for going online (signing in, leaderboards,
   encrypted traffic). The port rewrites none of it and lets it run as it is, so to the game this is Xbox Live as it
   always was.
2. **A pretend network card.** A real Xbox has a network card; a PC port has none. The port imitates the Xbox's card in
   software: what the game sends to it, the port hands to Windows to send on the internet, and the servers' replies go
   back into the card for the game. The game posts letters in its own mailbox; the port is the postman.
3. **The identity of a registered console.** Insignia only accepts registered Xboxes. The virtual Xbox you registered in
   xemu keeps its identity card (console serial and keys) in the EEPROM file and your account on the hard disk image.
   The port only reads the two files to sign in with that identity; it changes and uploads nothing. That is also why one
   console cannot sign in from xemu and the port at the same time, as one ID card cannot be shown in two places at once.
4. **Another phone book (DNS).** The first step online is looking up where the Xbox Live servers are: DNS, a phone book.
   In an ordinary phone book, Xbox Live's numbers were disconnected long ago; Insignia's phone book points those names at
   Insignia's own servers. Whichever book the game asks, the port asks Insignia's instead: "Insignia public DNS" uses
   the one Insignia keeps on the internet; "Local InsigniaDNS.exe" uses the one the small program on your PC keeps, for
   when a proxy captures lookups.

So the game believes it reached the old Xbox Live, and Insignia believes a real Xbox (the one you registered) is calling;
the two match, and you can sign in and see the leaderboards.

In one sentence: the game stays as it is; the port gives it a network card, borrows your registered console's identity,
and swaps the phone book for Insignia's.

Details and evidence: `project/docs/09_联网_Insignia.md`.

## Building

The project is built with Visual Studio 2017 (x86 C++ tools) and CMake, as a 32-bit program:

```
cmake -S loader -B build -G "Visual Studio 15 2017" -A Win32
cmake --build build --config RelWithDebInfo
```

This produces `build\RelWithDebInfo\Ninja Gaiden Black Any.exe` and `blackrt.dll`. `loader/CMakePresets.json` also has upstream's
Visual Studio 2022 / 2026 presets.

**SDL3**: at configure time CMake downloads the SDL3 3.4.18 source release (SHA256 checked) and builds it with the
program; `-DNGB_SDL3_DIR=<path>` builds from an unpacked copy instead.

**Icon**: both programs carry `launcher/art/icon.ico` (the Dragon Sword, see "Legal") by default. For another icon or
none, configure with `-DPORT_ICON=<path to .ico>` or `-DPORT_ICON=`.

**Release zip**: run `tools\make_release.ps1 -Bin <build folder>\RelWithDebInfo` for
`dist\ngb-hurricane-pc-<version>-win32.zip` (the launcher, the game program, `blackrt.dll`, both READMEs, LICENSE) and
a symbols zip. The script checks that the game program carries its icon.

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
| `launcher/` | the launcher (in progress, see `project/docs/08_PC启动器计划.md`): install from a disc image, choose the game folder, start the game; `art/` holds its background art |
| `notes/` | upstream's analysis results for BLACK |

Decompiler and disassembler output, game files and test-run folders stay local and are not in the repository.

## How it works

`Ninja Gaiden Black Any.exe` is a small stub linked at the XBE's base address, so that it holds that address range before Windows can
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

The exceptions are the pictures in `launcher/art/`: the launcher's background art, taken from the author's own
website (its Ryu Hayabusa key art is Koei Tecmo's), and the programs' icon, the game's Dragon Sword texture. Both are
used only as the interface of this non-commercial fan port; the sources are in `launcher/art/SOURCES.md`. The author's own
two payment codes are there too.

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
- [SDL3](https://libsdl.org) (zlib licence, linked into `blackrt.dll`): reads the controllers beyond XInput and their
  rumble.

Developed by 哈雅布撒 (Bilibili) with Claude Code.
