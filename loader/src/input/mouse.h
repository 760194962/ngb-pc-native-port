// The mouse: one owner for whether the game holds it (cursor hidden and kept in the window, movement and buttons going
// to the game) and for the raw counts it reads.
//
// The game takes the mouse when it starts (the window's first time in front; the SNES and arcade games and the Xbox
// dashboard are new processes, so they start holding it too) and when the player left-clicks the picture (that click
// is not passed on). It lets go when the window leaves the front (Alt+Tab, minimized) and when the player presses Esc
// (in Ninja Gaiden Black the pause key in play, Esc in a menu); play starting again after that takes it back.
// A button already down when the game takes the mouse reaches the game only once released and pressed again.
//
// Threads: the window's thread runs the state (on_message); the game's input poll reads counts and asks for changes
// (let_go / play_resumed), which are posted to the window's thread.
#pragma once

#include <windows.h>

namespace mouse {

// Window thread
void attach(HWND hwnd); // raw mouse input for this window; call once it exists, before it is shown
// The window's messages the mouse needs; true when handled, with *result to return
bool on_message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT *result);

// Any thread
bool held();                             // the game holds the mouse now
void take(int *dx, int *dy, int *wheel); // counts since the last call: movement while held; wheel notches
                                         // (+ = away from the user) while the window is in front
int buttons();                           // held: bit 0-4 = left, right, middle, X1, X2; 0 otherwise
void let_go();                           // the player pressed Esc (or Black's pause key)
void play_resumed();                     // play started: take the mouse back if Esc let it go

} // namespace mouse
