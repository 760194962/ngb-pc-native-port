# Smoothness Settings Guide

Applies to 2.2.37.3-t. The goal is a smoother, steadier picture with the current architecture.

## Quick choice

| Display | Display mode | Steady game speed | V-Sync | 60 Hz with V-Sync | Low-latency present |
|---|---|---|---|---|---|
| 120 / 240 Hz | Exclusive fullscreen | On | On | Off | Off |
| 144 / 165 Hz | Exclusive fullscreen | On | On | On | Off |
| 60 Hz | Exclusive fullscreen | On | On | Off | Off |
| 60 Hz | Windowed / borderless | Off | On | Off | Off |

120 / 240 Hz divide 60 exactly, so keep the high refresh rate; 144 / 165 Hz switch to 60 Hz so each frame gets one refresh.

## Where the settings are

Launcher Start page:

- Steady game speed

Launcher Graphics page:

- V-Sync
- Low-latency present
- 60 Hz with V-Sync

Press Save after changing the Graphics page. Steady game speed is written as soon as you click it, but the game only reads it on its next start. Restart the game after changing any setting.

## High refresh display: exclusive fullscreen

120 / 240 Hz:

1. Choose exclusive fullscreen.
2. Turn V-Sync on.
3. Leave "60 Hz with V-Sync" off.
4. Turn Low-latency present off.
5. Press Save.
6. Go back to the Start page and turn Steady game speed on.
7. Start the game.

Result: the display stays at 120 / 240 Hz; 60 fps gets 2 / 4 refreshes per frame.

144 / 165 Hz:

1. Choose exclusive fullscreen.
2. Turn V-Sync on.
3. Turn "60 Hz with V-Sync" on.
4. Turn Low-latency present off.
5. Press Save.
6. Go back to the Start page and turn Steady game speed on.
7. Start the game.

Result: entering exclusive fullscreen switches the display from 144 / 165 Hz to 60 Hz, with one refresh per frame.

## 60 Hz display: exclusive fullscreen

1. Choose exclusive fullscreen.
2. Turn Steady game speed on.
3. Turn V-Sync on.
4. Turn Low-latency present off.
5. Leave "60 Hz with V-Sync" off. It does nothing on a 60 Hz display.
6. Save and restart the game.

## 60 Hz display: windowed / borderless

1. Choose windowed or borderless.
2. Turn Steady game speed off.
3. Turn V-Sync on.
4. Turn Low-latency present off.
5. Leave "60 Hz with V-Sync" off.
6. Save and restart the game.

Windowed / borderless is shown through Windows' desktop composition (DWM) and does not switch the display mode.

## What the settings do

Steady game speed: pins the game clock to 59.94 Hz and stops following the display refresh rate. It is not a frame-rate cap and does not limit rendering.

V-Sync: changes the picture on a display refresh to avoid tearing. In exclusive fullscreen it is needed together with the 60 Hz mode.

60 Hz with V-Sync: only applies in exclusive fullscreen with V-Sync on. It switches a 144 / 165 Hz display to 60 Hz; 120 / 240 Hz divide 60 exactly and do not need it. Steady game speed must also be on; otherwise the game beat follows kernel vblank, can fall to 47 and a 30 fps cutscene drops to 24.

Low-latency present: experimental. Keep it off in exclusive fullscreen to avoid Present and the beat waiting twice.

## Common questions

Cutscene slow: check whether "60 Hz with V-Sync" is on while Steady game speed is off. On a high refresh display, turn both on, or leave the 60 Hz switch off.

Game speed wrong: turn Steady game speed on, then restart the game.

Tearing: turn V-Sync on.

Black screen when switching fullscreen: switching the display mode briefly goes black; this is normal.

Stutter after Alt+Tab: switch to windowed / borderless first; exclusive fullscreen mode changes still carry risk.

## One line

High refresh exclusive fullscreen: 60 Hz switch on, Steady game speed on, V-Sync on, Low-latency present off.

60 Hz exclusive fullscreen: Steady game speed on, V-Sync on, Low-latency present off, 60 Hz switch off.

60 Hz windowed / borderless: Steady game speed off, V-Sync on, Low-latency present off, 60 Hz switch off.
