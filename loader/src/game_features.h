// What the port does for each program it runs, decided in one place from the program's game and role
// (build_profile.h: BuildProfile::game, ::program), rather than at each use from combinations of fields.
//
// The running program is g_cfg.profile, set once from default.xbe's hash before anything is installed (main.cpp).
// A program with no profile is one the port does not know (the Insignia Setup Assistant, homebrew): it drives the
// hardware itself (nv2a_display.cpp, usb_ohci.cpp) and gets none of the games' patches.
#pragma once

#include "build_profile.h"

// The running program's game and role
bool running_game(Game g);
bool running_program(Program p);
bool running_unknown(); // no profile: a program the port does not know

// The title menu's EXIT GAME row: each game's own way (ngb_exitmenu.cpp, ng04_exitmenu.cpp)
enum class ExitMenu : uint8_t { None, Ngb, Ng04 };
// Ⓐ on the Live boxes' dashboard choices when the port's dashboard cannot run: cancel as Ⓑ (ngb_livedash.cpp)
enum class LiveBoxes : uint8_t { None, Ngb, Ng04 };

// What the loader installs into a program's code before it starts (main.cpp)
struct Patches {
    bool black = false;          // BLACK's own: timing, mouse look, auto-aim, FOV, front end, 60 fps, PC SETTINGS, vertex snap
    bool aspect = false;         // widescreen shapes other than 16:9 (the build's 16/9 constant is known)
    bool hud_probe = false;      // the HUD's sides wider than 16:9: in play as the game's HUD routine says (its 2D is known)
    ExitMenu exit_menu = ExitMenu::None;
    bool music51 = false;        // the music player's six channels at -96 dB (Ninja Gaiden Black's 5.1 music)
    bool cpu_textures = false;   // the ripple textures the game makes on the CPU, kept
    bool saved_volumes = false;  // the saved sound volumes at a cold start
    LiveBoxes live_boxes = LiveBoxes::None;
};
Patches patches_for(const BuildProfile *p);
