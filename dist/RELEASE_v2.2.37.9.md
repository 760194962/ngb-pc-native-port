《忍者龙剑传 黑之章》（含飓风版中文汉化 vb-zh-x2.1）与《忍者龙剑传》（2004）的非官方 Windows 原生移植。**2.2.37.9**。

## 2.2.37.9：2.2.37.8 的收尾 —— 发布包补回 README，说明改准，Alt+Enter 记住真全屏

这一版修的是对 2.2.37.3～2.2.37.8 做代码审查时找到的问题。**游戏运行、画面节拍和性能与 2.2.37.8 相同。**

### 先看这里：这次更新对你有什么影响

| 你的情况 | 更新后 |
|---|---|
| 所有人 | 发布包里重新有 `README.md` / `README.en.md`（2.2.37.4～2.2.37.8 的包里漏了） |
| 按流畅度指南设置过 | 指南重写了：以前有几处说法已经不对（见下）。按新指南的“快速选择”对一下即可 |
| 用真全屏，常按 Alt+Enter 切窗口 | 从真全屏切到窗口、关掉游戏，下次 Alt+Enter 会回到**真全屏**（以前会进无边框） |
| 一次游戏里切换过全屏又切回来 | 关闭时是什么状态，下次就是什么状态（以前可能下次一开就是全屏） |

### 修了什么

1. **发布包缺 README**：2.2.37.4 加入流畅度指南时，打包脚本里复制 README 的那一行被误替换掉，2.2.37.4～2.2.37.8 的包里都没有 README。现在补回，并且打包时会核对包里的 14 个文件，少一个或多一个都会停下，不会再悄悄漏掉。
2. **流畅度指南、README 的“流畅度设置”、启动器说明按实际行为重写**。以前不准确的地方：
   - “开 60 Hz 开关时必须打开稳定游戏速度，否则会跟 kernel vblank 掉到 47、CG 变 24”：2.2.37.6 起已不再使用 kernel vblank，真全屏一律用固定计时器；
   - “120 / 240 Hz 真全屏每帧正好显示 2 / 4 次刷新”：游戏时钟是 59.94 Hz，屏幕是 60 的倍数，约每 8 / 4 秒会有一帧多停一次刷新；
   - 真全屏推荐打开「稳定游戏速度」：它只影响 60 Hz 显示器的窗口 / 无边框，真全屏下没有作用；
   - 窗口 / 无边框推荐“垂直同步开”：垂直同步只在真全屏下起作用；
   - 「稳定游戏速度」写“不锁帧”：打开后游戏仍然每秒 60 帧，只是不跟桌面合成；
   - 「低延迟呈现」写“出现 CG 变速、45 / 24 帧时先关掉”：它只改变画面送出的时刻，不影响速度。
   
   新指南写明：**游戏速度在任何设置下都正确**；真全屏开垂直同步时约每 17 秒（120 Hz 约 8 秒、240 Hz 约 4 秒）有一帧多显示一次；开了 G-SYNC / FreeSync 时真全屏最好；120 Hz 以上的窗口 / 无边框画面不均匀（已知问题，可改用真全屏，或把 Windows 刷新率改成 60 Hz）。
3. **Alt+Enter 记住真全屏**：以前从真全屏按 Alt+Enter 切到窗口再关闭，存档只记下“窗口”，下次 Alt+Enter 就进了无边框。现在 `black.ini` 另外记住“全屏时用哪一种”（新项 `fullscreen_kind`），启动器选无边框 / 真全屏时也会一并写入；选“窗口”时保留上次的全屏类型；“恢复默认”回到无边框。
4. **一次游戏里改过去又改回来的设置会正确保存**：游戏只写回本次改过的设置（不覆盖启动器在游戏运行期间改的设置），但它比较的是启动时读到的值。窗口 → 全屏 → 窗口后，最后的“窗口”和启动时一样，就没有再写，存档停在“全屏”，下次一开就是全屏。现在比较的是上一次写入的值。2.2.37.8 及更早的版本都有这个问题。
5. 小修：恢复窗口样式失败时不再丢掉待办标志；删掉只写不读的“焦点”状态（不影响任何行为）。

### 没有改的

画面节拍的代码完全没动。真全屏开垂直同步时“约每 17 秒一帧多显示一次”是固定 59.94 Hz 游戏时钟的代价，这一版只把它写清楚；改进放到后续版本。

### 测试

- **Alt+Enter**（前台，向游戏窗口发送 Alt+Enter，再正常关闭）：

  | 过程 | 关闭后存档 | 下次 Alt+Enter |
  |---|---|---|
  | 真全屏 → 窗口 | fullscreen 0，记住真全屏 | 进入**真全屏** |
  | 窗口 → 真全屏 → 窗口 | fullscreen 0（修正前错写成 2） | — |
  | 无边框 → 窗口 | fullscreen 0，记住无边框 | 进入无边框 |
  | 窗口 → 无边框 → 窗口 | fullscreen 0（修正前错写成 1） | — |
- **设置读取**：`fullscreen` / `fullscreen_kind` 不论在文件里的先后顺序，5 种组合都读对；旧存档（没有 `fullscreen_kind`）照旧默认无边框。
- **启动器**：开始页、图形页中英文截图核对，改过的文字都放得下。
- **性能**（待机演示 160 秒，与 2.2.37.8 各跑 1 次，独显）：游戏速度都是 93.7%；平均帧时间 3.73 对 3.75 ms，99% 帧 16.95 对 17.06 ms；都没有卡顿。
- **冒烟测试**（发布包本身）：`Ninja Gaiden Black.exe` 跑黑之章 150 秒、`Ninja Gaiden.exe` 跑 2004 更新补丁版 20 秒，绘制状态全部回放、0 条遗漏，0 次卡顿。
- **发布包**：Windows Defender 扫描四个程序，无威胁；14 个文件（比 2.2.37.8 多回 README 两个），种子等数据文件与 2.2.37.8 逐字节相同；程序文件属性显示 2.2.37.9。没有测试用代码。

## 发布包

- `ngb-hurricane-pc-2.2.37.9-win32.zip`
- 大小：3,505,242 字节
- SHA-256：`d357a99c5b6e01cb4c9c0a12aa9a02f13aedd4bbc5e398849c73f17cdfd6fe7e`
- 符号包 `ngb-hurricane-pc-2.2.37.9-win32-symbols.zip`：41,514,994 字节
- SHA-256：`d60d42c47d16e65b4edab726958b2d0c0e2e3e9a41c1551dc491c0bb74283a17`

作者：b站 哈雅布撒（QQ：2682557218）

---

## English

An unofficial native Windows port of Ninja Gaiden Black (including the Hurricane Chinese translation vb-zh-x2.1) and Ninja Gaiden (2004). **2.2.37.9**.

## 2.2.37.9: wrapping up 2.2.37.8 - the READMEs back in the package, accurate notes, Alt+Enter remembers exclusive fullscreen

This release fixes what a code review of 2.2.37.3-2.2.37.8 found. **The game, its frame pacing and its performance are as in 2.2.37.8.**

### Start here: what this update means for you

| Your setup | After the update |
|---|---|
| Everyone | `README.md` / `README.en.md` are in the package again (missing from 2.2.37.4-2.2.37.8) |
| You followed the smoothness guide | The guide is rewritten: some of its advice was no longer true (below). Check the new "Quick choice" table |
| You use exclusive fullscreen and Alt+Enter to a window | From exclusive fullscreen to a window and closing the game, the next Alt+Enter goes back to **exclusive** fullscreen (it went borderless) |
| You switched to fullscreen and back within one session | The game starts the next time as it was when you closed it (it could start fullscreen) |

### What was fixed

1. **The READMEs missing from the package**: when the smoothness guides were added in 2.2.37.4, the packaging script's line that copied the READMEs was replaced by mistake, so 2.2.37.4-2.2.37.8 had none. They are back, and packaging now checks the package's 14 files and stops on any missing or extra one.
2. **The smoothness guides, the READMEs' "Smoothness settings" and the launcher's notes rewritten to match the code.** What was wrong:
   - "with the 60 Hz switch, Steady game speed must be on, or the clock follows kernel vblank and drops to 47, cutscenes to 24": kernel vblank is not used since 2.2.37.6; exclusive fullscreen always uses the fixed timer;
   - "120 / 240 Hz exclusive shows each frame exactly 2 / 4 refreshes": the clock is 59.94 Hz and the monitor a multiple of 60, so about every 8 / 4 s one frame stays a refresh longer;
   - Steady game speed recommended for exclusive fullscreen: it only affects a 60 Hz monitor in a window / borderless;
   - V-Sync recommended for a window / borderless: V-Sync only works in exclusive fullscreen;
   - Steady game speed said "no frame cap": with it on the game still runs 60 frames a second, it just does not follow the desktop;
   - Low-latency present said to turn it off for cutscene speed changes or 45 / 24 fps: it only changes when a picture goes out, never the speed.

   The new guide says: **the game's speed is right with any setting**; exclusive fullscreen with V-Sync shows one frame a refresh longer about every 17 s (8 s at 120 Hz, 4 s at 240 Hz); with G-SYNC / FreeSync exclusive fullscreen is best; a window / borderless at 120 Hz and above is uneven (known; use exclusive fullscreen or set Windows to 60 Hz).
3. **Alt+Enter remembers exclusive fullscreen**: from exclusive fullscreen to a window and closing, the save said only "window" and the next Alt+Enter went borderless. `black.ini` now also keeps which fullscreen to use (new `fullscreen_kind`); the launcher writes it with a borderless / exclusive choice, keeps it with Window, and Restore defaults sets borderless.
4. **A setting changed and changed back within one session is saved right**: the game writes back only what it changed (so launcher changes made while it runs are kept), but it compared with the values read at the start. Window -> fullscreen -> window left the last "window" equal to the start value, so it was not written and the save stayed at fullscreen: the next start was fullscreen. It now compares with what it last wrote. 2.2.37.8 and earlier had this too.
5. Small fixes: a failed window restyle no longer loses its pending flag; the "focus" state that was written but never read is gone (no change in behaviour).

### Not changed

The frame pacing code is untouched. "One frame a refresh longer about every 17 s" in exclusive fullscreen with V-Sync is the cost of the fixed 59.94 Hz game clock; this release only explains it, an improvement comes later.

### Tests

- **Alt+Enter** (foreground, Alt+Enter sent to the game window, then a normal close):

  | Sequence | Save after closing | Next Alt+Enter |
  |---|---|---|
  | exclusive -> window | fullscreen 0, exclusive remembered | **exclusive** fullscreen |
  | window -> exclusive -> window | fullscreen 0 (was wrongly 2) | - |
  | borderless -> window | fullscreen 0, borderless remembered | borderless |
  | window -> borderless -> window | fullscreen 0 (was wrongly 1) | - |
- **Reading the settings**: `fullscreen` / `fullscreen_kind` read right in either order, 5 combinations; an old save (no `fullscreen_kind`) defaults to borderless as before.
- **Launcher**: Start and Graphics pages checked in Chinese and English snapshots; the changed texts fit.
- **Performance** (attract demo 160 s, one run each against 2.2.37.8, dedicated GPU): game speed 93.7% in both; average frame 3.73 vs 3.75 ms, 99th percentile 16.95 vs 17.06 ms; no hitch in either.
- **Smoke tests** (the package itself): `Ninja Gaiden Black.exe` ran Black 150 s and `Ninja Gaiden.exe` the NG2004 title update 20 s, all draw states replayed, none left out, no hitch.
- **The package**: Windows Defender scan of the four programs, no threats; 14 files (the two READMEs back since 2.2.37.8), its seed data files byte for byte as 2.2.37.8; the programs' properties say 2.2.37.9. No test code.

## Package

- `ngb-hurricane-pc-2.2.37.9-win32.zip`
- Size: 3,505,242 bytes
- SHA-256: `d357a99c5b6e01cb4c9c0a12aa9a02f13aedd4bbc5e398849c73f17cdfd6fe7e`
- Symbols `ngb-hurricane-pc-2.2.37.9-win32-symbols.zip`: 41,514,994 bytes
- SHA-256: `d60d42c47d16e65b4edab726958b2d0c0e2e3e9a41c1551dc491c0bb74283a17`

Author: 哈雅布撒 on Bilibili (QQ: 2682557218)
