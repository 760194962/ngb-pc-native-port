# Smoothness Settings Guide

Applies from 2.2.37.9. What the picture looks like on each kind of monitor and display mode, and how to set things up.

## First: the game's speed is right with any setting

The game's clock is a fixed 59.94 beats a second, as on the original Xbox, whatever the monitor's refresh rate and whether
G-SYNC / FreeSync is on. The one exception is **a 60 Hz monitor in a window / borderless**: there the clock follows Windows'
desktop composition, which gives the most even picture, less than 0.1% from 59.94.

So "cutscenes slow down" or "stuck at 24 / 40 fps" cannot happen. The settings below only change **how evenly the frames are
shown and whether the picture tears**.

## Quick choice

| Monitor | Display mode | V-Sync | 60 Hz with V-Sync | Steady game speed | Low-latency present |
|---|---|---|---|---|---|
| G-SYNC / FreeSync on | Exclusive fullscreen | On or off | Off | Off | Off |
| 60 Hz | Windowed / borderless | No effect | No effect | Off | Off |
| 120 / 240 Hz | Exclusive fullscreen | On | Off | No effect | Off |
| 144 / 165 Hz | Exclusive fullscreen | On | On | No effect | Off |

"No effect" means the setting does nothing in that mode; on or off is the same.

## What you get in each case

### G-SYNC / FreeSync on

- **Exclusive fullscreen**: the graphics card takes over the refresh and the monitor refreshes with the game's frames: even,
  no tearing. The best combination.
- **Windowed / borderless**: on some PCs G-SYNC / FreeSync does not take over there (Windows' desktop composes the picture);
  it is then as with it off, see your refresh rate below.

### 60 Hz monitor

- **Windowed / borderless**: the clock follows the desktop's composition; each frame is shown exactly once, the most even.
- **Exclusive fullscreen with V-Sync**: the game's clock is 59.94 Hz and the monitor 60 Hz, a little apart, so **about every
  17 s one frame is shown twice**.
- **Exclusive fullscreen without V-Sync**: the picture tears.

### 120 / 240 Hz monitor

- **Exclusive fullscreen with V-Sync**: each frame stays 2 / 4 refreshes; for the same 59.94-vs-60 reason, **about every 8 s at
  120 Hz and every 4 s at 240 Hz** one frame stays one refresh longer.
- **Windowed / borderless**: clearly uneven (many frames shown for 1 or 3 refreshes). A known issue, being worked on.
  For an even window: set the refresh rate to 60 Hz in Windows' display settings.

### 144 / 165 Hz monitor

60 does not divide 144 / 165, so without a mode change frames alternate between 3 and 2 refreshes, a steady judder.

- **Exclusive fullscreen with V-Sync and "60 Hz with V-Sync"**: entering exclusive fullscreen switches the monitor to 60 Hz;
  each frame is shown once, as at 60 Hz exclusive (one shown twice about every 17 s). Cost: entering or leaving exclusive
  fullscreen (Alt+Tab too) changes the display mode, **about 1.5 s more of black screen each time**.
- **Windowed / borderless**: the 3/2-refresh judder. For an even window: set Windows' refresh rate to 60 Hz.

## What each setting does

- **Display mode**: windowed, borderless fullscreen, exclusive fullscreen.
- **V-Sync**: exclusive fullscreen only; stops tearing. Windows and borderless never tear.
- **60 Hz with V-Sync**: exclusive fullscreen with V-Sync only; switches the monitor to 60 Hz. For 144 / 165 Hz; at 120 /
  240 Hz it makes little difference. It does not change the game's speed.
- **Steady game speed** (Start page): only for **a 60 Hz monitor in a window / borderless**: the game's clock becomes a fixed
  59.94 Hz instead of following the desktop's composition. Keep it off (off is more even); turn it on only if the speed feels
  unsteady in that case. Anywhere else it does nothing.
- **Low-latency present** (experimental): a frame goes out as soon as it is drawn, less delay from a key press to the
  picture; windowed it may look less even than off. It does not change the game's speed.
- **Alt+Enter**: goes round window -> borderless -> window -> exclusive fullscreen -> window ..., showing the mode at the
  picture's bottom right. Quitting keeps the last mode and where the round is.

## Where the settings are

- Launcher Start page: Steady game speed (written at once).
- Launcher Display page: display mode.
- Launcher Graphics page: V-Sync, 60 Hz with V-Sync, Low-latency present (press Save after changing).

All settings apply from the next start of the game.

## Common questions

**The picture judders**: see your monitor above. It is worst at 120 Hz and above in a window / borderless: use exclusive
fullscreen, or set Windows' refresh rate to 60 Hz.

**A small hitch every dozen seconds or so**: known with exclusive fullscreen and V-Sync (the game's 59.94 Hz against the
monitor's 60 Hz). Not with G-SYNC / FreeSync.

**The picture tears**: turn V-Sync on in exclusive fullscreen.

**A black moment entering or leaving fullscreen**: "60 Hz with V-Sync" changing the display mode, about 1.5 s, normal. Turn it
off if you do not want it.

**The game's speed looks wrong**: check the version is 2.2.37.9 or newer. If it still is, send `logs\black.log` to the author.
