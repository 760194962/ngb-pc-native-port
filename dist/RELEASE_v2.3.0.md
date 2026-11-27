《忍者龙剑传 黑之章》（含飓风版中文汉化 vb-zh-x2.1）与《忍者龙剑传》（2004）的非官方 Windows 原生移植。**2.3.0**。

## 2.3.0：新的帧节拍时钟，各种显示模式下画面都均匀

### 先看这里：这次更新对你有什么影响

| 你的情况 | 更新后 |
|---|---|
| 60 Hz 屏幕，真全屏 + 垂直同步 | 以前约每 17 秒一帧多显示一次；现在每帧都只显示一次 |
| 60 Hz 屏幕，窗口 / 无边框 | 和以前一样均匀；无边框开头几秒的一串重复 / 丢帧没有了 |
| 144 / 165 Hz 屏幕，无边框 | 进入无边框时自动切到 60 Hz（和真全屏一样），每帧显示一次；最小化或退出时切回。不想要就关掉启动器「图形」页「全屏时用 60 Hz」 |
| 144 / 165 Hz 屏幕，窗口 | 没有变化（窗口不能单独改屏幕刷新率） |
| 开了 G-SYNC / FreeSync 并保留高刷 | 帧间隔明显更均匀 |
| 真全屏，垂直同步关（默认） | 撕裂线基本只出现在画面最上、最下的边缘，不再满屏滚动 |
| 打开了「低延迟呈现」 | 真全屏下按键延迟约 16.6 ms → 1–5 ms，画面不再多出重复 / 丢帧；窗口和无边框下这个开关不再提前送帧 |
| 游戏速度、画面内容、声音 | 不变（速度和原版 59.94 Hz 最多差 0.4%，常见 60 Hz 屏幕 +0.11%） |

### 改了什么

1. **游戏时钟对准屏幕的刷新**：以前游戏的节拍是固定 59.94 Hz 计时器（只有 60 Hz 窗口跟着桌面合成），而屏幕实际是 60.008 Hz 之类，
   两者差一点，真全屏下约每 14–17 秒必然有一帧多显示一次。现在，屏幕刷新率是 60 / 120 / 240 Hz 一类时，计时器被微调到对准屏幕的
   vblank（每 1 / 2 / 4 次刷新一拍），每帧显示的刷新次数完全相同。计时器从不等待屏幕：显示器的信息只能把节拍的速度在 59.70–60.30 Hz
   之间微调，不会再出现以前跟着显示器事件走而掉到 47 / 24 帧的问题。屏幕的刷新时刻从扫描线读取；读出来不可信时（有些笔记本切过模式后
   扫描线计数会失真）改用 Windows 桌面合成器的时间戳；都读不到时退回固定 59.94 Hz。
2. **144 / 165 Hz 屏幕的无边框也切到 60 Hz**：60 帧在 165 Hz 上每帧只能 3 次 / 2 次刷新交替，有固定的顿挫。以前只有真全屏会切到
   60 Hz，现在无边框也一样（Windows 的临时显示模式：最小化、回到窗口、退出时切回，程序意外退出时由系统恢复）。屏幕本来就是
   60 / 120 / 240 Hz 时不切换（以前真全屏下 120 / 240 Hz 也会被切到 60 Hz）。
3. **G-SYNC / FreeSync 保留高刷**：这时屏幕在每帧送出时刷新，而游戏每帧画完的时间不一样（等到 vblank 才开始画），帧间隔在 13–20 ms
   之间散开。现在每帧都等到节拍后的同一时刻再送出（自动估算，约 5 ms），帧间隔变均匀。代价：多约 5 ms 延迟。
4. **低延迟呈现重做**：只在真全屏、并且这一帧赶得上下一次刷新时才提前送出；赶不上就照常等节拍，之后连续 120 帧都赶得上才再提前。
   窗口和无边框经过桌面合成，提前送会一帧重复一帧丢失，所以那里不再提前。仍默认关闭。
5. **真全屏不开垂直同步时的撕裂线**：节拍对准屏幕后撕裂线不再滚动，于是把节拍放在 vblank 前 1 ms，让翻页落在刷新间隙或画面边缘
   （和 RTSS 的“扫描线同步”是同一思路）。
6. 启动器「图形」页：「垂直同步时用 60 Hz」改名为「全屏时用 60 Hz」；「开始游戏」页「稳定游戏速度」现在对所有模式都有效
   （打开后时钟固定 59.94 Hz、不对准屏幕）；说明文字和流畅度指南按新行为重写。

### 测试（RTX 3060 独显直连，开场演示 150 秒，用桌面复制逐帧读出屏幕上实际显示的帧号）

| 场景 | 2.2.37.13 | 2.3.0 |
|---|---|---|
| 60 Hz 真全屏 + 垂直同步，重复帧（2 次） | 72 / 51 | 3 / 5 |
| 165 Hz 桌面 → 真全屏（切 60 Hz） | 56 | 6 |
| 165 Hz 桌面 → 无边框 | 每帧 3 / 2 次交替 | 0（切到 60 Hz） |
| 60 Hz 窗口 / 无边框 | 2 / 0 | 1 / 0 |
| G-SYNC 165 Hz 真全屏，帧间隔在 16.7 ± 1 ms 内 | 31% | 93% |
| 真全屏关垂直同步，撕裂在画面中间 | 约 66% | 约 0.4% |
| 低延迟呈现（真全屏），按键到送出 | 16.6 ms | 1–5 ms，重复 / 丢帧 1–2 / 0 |

- 21 种显示器的仿真（60 / 59.94 / 60.1 / 120 / 180 / 240 / 360 Hz、144 / 165 / 75 / 50 Hz、VRR、扫描线失真、读不到扫描线）：
  能对准的刷新率不均匀帧全部少于 2.2，60 Hz 一类为 0；不能对准的和 2.2 一样。
- 性能（待机演示 160 秒，与 2.2.37.13 交错）：游戏速度 93.70% 对 93.72%，平均帧时间 3.72 对 3.82 ms。
- 冒烟测试（发布包本身）：`Ninja Gaiden Black.exe` 跑黑之章 90 秒、`Ninja Gaiden.exe` 跑 2004 更新补丁版 20 秒，8493 个绘制状态
  全部回放、0 遗漏，都正常退出。
- 发布包：Windows Defender 扫描四个程序，无威胁；14 个文件，和 2.2.37.13 相同；种子等数据文件逐字节相同；程序属性显示 2.3.0。
  测试用的选项（`--frame-marker`、`--tear-trace`、`--tick-phase`、`--no-scanline`）不加参数时不起作用。
- **还没测**：集成显卡（Iris Xe），以及 120 / 240 Hz 屏幕（作者手上没有，只做了仿真）。

设计与全部数据：源码仓库 `project/docs/27_帧节拍时钟_2.3.md`。

## 发布包

- `ngb-hurricane-pc-2.3.0-win32.zip`
- 大小：3,525,719 字节
- SHA-256：`a9edc408426a2843fae45f5387994cf465f58e95b6eb86637e9adeb92be81a31`
- 符号包 `ngb-hurricane-pc-2.3.0-win32-symbols.zip`：19,876,840 字节
- SHA-256：`b034f6cd4bf395ab450a7a8393284cc09f6fd6cf87bd2ae415322476ab040f1e`

作者：b站 哈雅布撒（QQ：2682557218）

---

## English

An unofficial native Windows port of Ninja Gaiden Black (including the Hurricane Chinese translation vb-zh-x2.1) and Ninja Gaiden (2004). **2.3.0**.

## 2.3.0: a new frame clock, even frames in every display mode

### Start here: what this update means for you

| Your setup | After the update |
|---|---|
| 60 Hz screen, exclusive fullscreen with V-Sync | One frame shown a refresh longer about every 17 s before; now every frame shows exactly once |
| 60 Hz screen, window / borderless | As even as before; borderless's burst of repeats / drops in the first seconds is gone |
| 144 / 165 Hz screen, borderless | Entering borderless switches the screen to 60 Hz (as exclusive fullscreen does), each frame shown once; back when minimised or on exit. Turn off the launcher's Graphics page "60 Hz in fullscreen" if you prefer |
| 144 / 165 Hz screen, window | No change (a window cannot change the screen's refresh rate) |
| G-SYNC / FreeSync with the high refresh kept | Clearly more even frame gaps |
| Exclusive fullscreen, V-Sync off (the default) | The tear is kept at the very top or bottom of the picture instead of rolling over it |
| "Low-latency present" on | In exclusive fullscreen pad-to-screen lag about 16.6 ms -> 1-5 ms with no pictures repeated or lost; in a window / borderless it no longer sends frames early |
| Game speed, picture, sound | Unchanged (the speed is at most 0.4% from the console's 59.94 Hz; a common 60 Hz screen +0.11%) |

### What changed

1. **The game's clock lines up with the screen's refreshes**: the game's steps came from a fixed 59.94 Hz timer (only a 60 Hz window
   followed the desktop's composition) while the screen runs at, say, 60.008 Hz, so in exclusive fullscreen a frame was bound to show
   twice about every 14-17 s. Now, on a 60 / 120 / 240 Hz-class screen, the timer is steered onto the screen's vblanks (a step every
   1 / 2 / 4 refreshes) and every frame stays for the same number of refreshes. The timer never waits for the screen: what the display
   says can only steer its rate within 59.70-60.30 Hz, so the old drops to 47 / 24 fps from following display events cannot come back.
   When the screen refreshes is read from its scanline, or, when that reading is not to be trusted (some laptops' scanline counters go
   wrong after a mode change), from the Windows desktop compositor's timestamps; with neither, a fixed 59.94 Hz.
2. **Borderless on a 144 / 165 Hz screen switches to 60 Hz too**: 60 frames on 165 Hz stay for 3 and then 2 refreshes, a steady judder.
   Only exclusive fullscreen used to switch to 60 Hz; borderless now does as well (a temporary Windows display mode: back when
   minimised, back in a window or on exit, and restored by Windows if the program ends unexpectedly). A 60 / 120 / 240 Hz screen is no
   longer switched (exclusive fullscreen used to switch 120 / 240 Hz to 60 too).
3. **G-SYNC / FreeSync with the high refresh kept**: the screen then refreshes as each frame is sent, and the game takes a different
   time to draw each (it starts after the vblank), so frame gaps spread over 13-20 ms. Every frame now waits for the same time after
   its step before going out (estimated as it runs, about 5 ms): even gaps, about 5 ms more lag.
4. **Low-latency present redone**: a frame goes out early only in exclusive fullscreen and only when it makes the next refresh; one that
   would not waits for its step as usual, and frames go early again after 120 in a row would have made it. Through the desktop
   compositor (window, borderless) going early shows one picture twice and loses one, so it no longer does there. Still off by default.
5. **The tear in exclusive fullscreen without V-Sync**: with the clock on the screen's refreshes the tear stops rolling, so the step is
   put 1 ms before the vblank and the flip falls between refreshes or at the picture's edge (the idea of RTSS's "scanline sync").
6. Launcher Graphics page: "60 Hz with V-Sync" is now "60 Hz in fullscreen"; the Play page's "Steady game speed" now applies in every
   mode (on: a fixed 59.94 Hz clock that does not line up with the screen); notes and smoothness guides rewritten to the new behaviour.

### Tests (RTX 3060 via MUX, attract demo 150 s, the frame number on screen read back with Desktop Duplication)

| Case | 2.2.37.13 | 2.3.0 |
|---|---|---|
| 60 Hz exclusive with V-Sync, repeated frames (2 runs) | 72 / 51 | 3 / 5 |
| 165 Hz desktop -> exclusive (switched to 60 Hz) | 56 | 6 |
| 165 Hz desktop -> borderless | 3 / 2 refreshes in turn | 0 (switched to 60 Hz) |
| 60 Hz window / borderless | 2 / 0 | 1 / 0 |
| G-SYNC 165 Hz exclusive, gaps within 16.7 +- 1 ms | 31% | 93% |
| Exclusive without V-Sync, tears mid-picture | about 66% | about 0.4% |
| Low-latency present (exclusive), pad to picture going out | 16.6 ms | 1-5 ms, repeats / drops 1-2 / 0 |

- A simulation of 21 displays (60 / 59.94 / 60.1 / 120 / 180 / 240 / 360 Hz, 144 / 165 / 75 / 50 Hz, VRR, a scanline that jumps, no
  scanline): fewer uneven frames than 2.2 at every rate the clock can follow, none at 60 Hz; the others as in 2.2.
- Performance (attract demo 160 s, alternated with 2.2.37.13): game speed 93.70% vs 93.72%, average frame 3.72 vs 3.82 ms.
- Smoke tests (the package itself): `Ninja Gaiden Black.exe` ran Black 90 s and `Ninja Gaiden.exe` the NG2004 title update 20 s; all
  8,493 draw states replayed, none left out; both exited normally.
- The package: Windows Defender scan of the four programs, no threats; 14 files as 2.2.37.13; seed data byte for byte the same; the
  programs' properties say 2.3.0. The test options (`--frame-marker`, `--tear-trace`, `--tick-phase`, `--no-scanline`) do nothing
  unless given.
- **Not tested yet**: integrated graphics (Iris Xe), and 120 / 240 Hz screens (none at hand; simulated only).

Design and all the measurements: `project/docs/27_帧节拍时钟_2.3.md` in the source repository.

## Package

- `ngb-hurricane-pc-2.3.0-win32.zip`
- Size: 3,525,719 bytes
- SHA-256: `a9edc408426a2843fae45f5387994cf465f58e95b6eb86637e9adeb92be81a31`
- Symbols `ngb-hurricane-pc-2.3.0-win32-symbols.zip`: 19,876,840 bytes
- SHA-256: `b034f6cd4bf395ab450a7a8393284cc09f6fd6cf87bd2ae415322476ab040f1e`

Author: 哈雅布撒 on Bilibili (QQ: 2682557218)
