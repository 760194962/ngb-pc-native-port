#include "game_features.h"

#include "common.h"

bool running_game(Game g) { return g_cfg.profile && g_cfg.profile->game == g; }
bool running_program(Program p) { return g_cfg.profile && g_cfg.profile->program == p; }
bool running_unknown() { return !g_cfg.profile; }

// Every program the port knows, and what it gets:
//
//                         BLACK  Ninja Gaiden Black          Ninja Gaiden (2004)                     Dashboard
//                                main  arcade SNES           main  SNES  downloader updater
//  BLACK's own patches      x
//  other screen shapes           x(1)                         x(1)
//  HUD at the sides              x(1)                         x(1)
//  EXIT GAME row                 Ngb                          Ng04
//  5.1 music fix                 x
//  CPU textures kept             x     x      x               x     x     x          x            x
//  saved volumes                 x                            x
//  Live boxes cancel (2)         Ngb                          Ng04        Ng04
//
// (1) where the build's address of it is known (BuildProfile::aspect_const, ::wide2d)
// (2) only when the port's own Xbox Live dashboard cannot run (main.cpp)
Patches patches_for(const BuildProfile *p)
{
    Patches r;
    if (!p) return r;
    r.black = p->game == Game::Black;
    r.aspect = p->aspect_const != 0;
    r.hud_probe = p->wide2d != nullptr;
    r.cpu_textures = p->game != Game::Black;
    const bool main = p->program == Program::Main;
    switch (p->game) {
    case Game::NinjaGaidenBlack:
        if (main) r.exit_menu = ExitMenu::Ngb, r.music51 = true, r.saved_volumes = true, r.live_boxes = LiveBoxes::Ngb;
        break;
    case Game::NinjaGaiden2004:
        if (main) r.exit_menu = ExitMenu::Ng04, r.saved_volumes = true;
        if (main || p->program == Program::Downloader) r.live_boxes = LiveBoxes::Ng04;
        break;
    case Game::Black:
    case Game::XboxDashboard:
        break;
    }
    return r;
}
