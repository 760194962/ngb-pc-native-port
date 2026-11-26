# Smoothness Settings Guide

Applies from 2.3.0. What the picture looks like on each kind of monitor and display mode, and how to set things up.

## In short

- **The game's speed is right with any setting.** On a screen refreshing at 60, 120 or 240 Hz (59.94, 60 and 60.1 Hz all count) the
  game's clock lines up with the screen's refreshes, at most 0.4% from the console's 59.94 Hz (a common 60.008 Hz screen: +0.11%); at
  other refresh rates the clock is a fixed 59.94 Hz. The clock never waits for the screen: no "slow movies" or "stuck at 24 / 40 fps".
- **60 Hz-class screens: a window, borderless and exclusive fullscreen are all even**, each frame shown exactly once. The old extra
  refresh about every 17 s in exclusive fullscreen is gone.
- **144 / 165 Hz screens: borderless and exclusive fullscreen switch the screen to 60 Hz by default**, each frame shown once as well.

## Quick choice

| Monitor | Display mode | V-Sync | 60 Hz in fullscreen | Steady game speed | Low-latency present |
|---|---|---|---|---|---|
| 60 Hz | Any | Exclusive: on or off (see below) | No effect | Off | For less lag: on, in exclusive fullscreen |
| 120 / 240 Hz | Any | Exclusive: on | No effect | Off | No effect |
| 144 / 165 Hz | Borderless or exclusive | Exclusive: on | On (default) | Off | Off |
| G-SYNC / FreeSync on | Borderless or exclusive, defaults | On or off | On (default, the most even); off to keep the high refresh | Off | Off |

"No effect" means the setting does nothing in that case.

## What you see

### 60 Hz monitors (59.94 and 60.1 Hz too)

- **Window / borderless / exclusive fullscreen with V-Sync**: the game's clock lines up with the screen; each frame is shown exactly once.
- **Exclusive fullscreen without V-Sync**: it still tears, but the tear is put between refreshes or at the very top or bottom of the
  picture and is hardly ever in the middle (measured: about 66% -> 0.4% of frames). The lowest lag.

### 120 / 240 Hz monitors

- The game's clock takes one step every 2 / 4 refreshes; every frame stays for the same number of refreshes. (Worked out from the design
  and a simulation; not measured on such a screen.)

### 144 / 165 Hz monitors

60 does not divide 144 / 165, so at a fixed refresh rate each frame stays for 3 and then 2 refreshes in turn: a steady judder.

- **Borderless / exclusive fullscreen with "60 Hz in fullscreen" (on by default)**: entering fullscreen switches that monitor to 60 Hz
  at the same size; each frame is shown once. Borderless switches back when minimised, back in a window or when the game exits, and to
  60 Hz again when shown. Costs: about 1.5 s more of black screen at each switch; Alt+Tab away from borderless leaves the desktop at
  60 Hz. Exclusive fullscreen switches only with V-Sync on.
- **Window**: a window cannot change the screen's refresh rate by itself; still 3 / 2. For an even window, set Windows' refresh rate to 60 Hz.

### G-SYNC / FreeSync on

- **Defaults (fullscreen switched to 60 Hz)**: as even as a 60 Hz screen, the most even choice.
- **"60 Hz in fullscreen" off, the high refresh kept**: the screen refreshes as frames come. The game takes a different time to draw
  each frame, so the program sends every frame out at the same time after its step (about 5 ms more lag) and the refreshes come evenly:
  measured, 93% of frame gaps within 16.7 +- 1 ms in exclusive fullscreen, 86% borderless, 80% in a window.

## What each setting does

- **Display mode**: window, borderless fullscreen, exclusive fullscreen.
- **V-Sync**: exclusive fullscreen only. On: no tearing. Off: on a 60 Hz-class screen the tear is put at the picture's edges; the
  lowest lag. A window and borderless never tear.
- **60 Hz in fullscreen** (was "60 Hz with V-Sync"; still `exclusive_60hz` in `black.ini`): on a 144 / 165 Hz-class screen, borderless
  and exclusive fullscreen (with V-Sync on) switch to 60 Hz. A 60 / 120 / 240 Hz screen is not switched. It never changes the game's speed.
- **Steady game speed** (Play page): a fixed 59.94 Hz game clock that never lines up with the screen, in any mode. Keep it off unless
  the game's speed seems unsteady.
- **Low-latency present** (experimental): in exclusive fullscreen a finished frame goes out at once when it makes the next refresh,
  pad to picture going out about 16.6 ms -> 1-5 ms, with no picture shown twice or lost. A window and borderless do not go early
  (through the desktop compositor one picture would show twice and one never). It never changes the game's speed.
- **Alt+Enter**: window -> borderless -> window -> exclusive fullscreen -> window ..., the mode shown at the picture's bottom right;
  the last mode is kept on exit.

## Where the settings are

- Launcher Play page: Steady game speed (saved at once).
- Launcher Display page: display mode.
- Launcher Graphics page: V-Sync, 60 Hz in fullscreen, Low-latency present (press Save).

All settings apply from the next start of the game.

## Common questions

**The picture judders**: most visible on a 144 / 165 Hz screen in a window. Use borderless or exclusive fullscreen (keep "60 Hz in
fullscreen" on), or set Windows' refresh rate to 60 Hz.

**The screen goes black when entering or leaving fullscreen**: "60 Hz in fullscreen" switching the display mode, about 1.5 s. Turn it
off if you prefer (a 144 / 165 Hz screen goes back to 3 / 2).

**Exclusive fullscreen without V-Sync tears at the edges when turning the camera**: the tear is kept at the very top or bottom; turn
V-Sync on to never see it.

**The game's speed looks wrong**: send `logs\black.log` to the author; its `[clock]` lines tell what the clock was doing.
