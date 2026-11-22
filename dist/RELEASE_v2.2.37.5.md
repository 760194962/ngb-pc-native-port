# 《忍者龙剑传 黑之章》PC 原生移植 **2.2.37.5**

> **已被 2.2.37.6 取代。** 2.2.37.5 只修了 kernel vblank 初始化失败；kernel 初始化成功但事件本身变慢（16.7 → 19–21ms）的情况仍会掉到 52。请使用 2.2.37.6。

**2.2.37.4 的修复版。** 修正 60Hz 真全屏下 kernel vblank 不可用时错误使用 DwmFlush 的问题。

## 这个版本是什么

```text
基线：2.2.37.4
修正：60Hz 真全屏 + kernel vblank 不可用时，不再落到 DwmFlush
修正：kernel 不可用时使用固定 59.94 Hz timer
新增：kernel 每 300 tick 重试，切模式后可重新初始化
新增：kernel 初始化失败原因、vblank 来源切换写日志
```

## 修复范围

旧版在 60Hz 真全屏、kernel vblank 初始化失败时，会落到 DwmFlush。独佔全屏下 DWM 不合成游戏画面，DwmFlush 会晚返回，造成：

- 游戏内约 52；
- CG 约 24。

2.2.37.5 改成：

- 真全屏永远不使用 DwmFlush；
- kernel 不可用就直接用固定 timer；
- kernel 失败后每 300 tick 重试；
- kernel wait 失败时退回 timer。

这不包含 Presenter / frame pacing，所以 CG / 主选单的稀疏呈现仍可能偏慢。

## 「稳定游戏速度」做什么

它把**游戏时钟**固定为 59.94 Hz QPC timer，不再跟随显示器刷新 / VRR / 独占全屏的 kernel vblank。

它不是帧率上限：

- 不锁渲染帧率；
- 不保证镜头一定 60 fps；
- 只保证游戏逻辑 / tick 的基础时钟不跟屏幕走。

什么时候用：

> CG、过场或战斗明显变快 / 变慢时打开；固定游戏时钟 59.94 Hz，不跟屏幕，不锁帧。下次启动生效。

## 开关行为

- 在启动器里切换会**立即写入** `black.ini` 的 `timer_vblank = 1/0`；
- 关闭启动器、不开始游戏，也会保留这个选择；
- 游戏只在启动时读取 `black.ini`，运行中不会实时切换；
- 关闭这个开关时，游戏运行路径与 2.2.37.4 基本相同，但 2.2.37.5 的 vblank 来源修复仍生效。

## 明确不包含

- Presenter 线程；
- Present / Reset / 焦点 / occluded 的完整状态机；
- 非阻塞 Present；
- 帧率上限；
- CG / 主选单的稀疏呈现 / frame pacing；
- 对 60 Hz 模式切换风险的修复。

因此：

- `timer_vblank` 不能阻止 Present 卡住、Alt+Tab 的 device reset、全屏切模式黑屏；
- CG / 主选单仍可能 24 / 偏慢，需要 Presenter / frame pacing；
- 「垂直同步时用 60 Hz」仍然会切显示模式，仍有黑屏 / Alt+Tab / 过场变慢的风险；
- 120 / 240 Hz 本来就能整除 60，不需要切 60 Hz；
- 144 / 165 Hz 才有视觉均匀度收益，但仍有模式切换代价。

## 已测试

- 窗口 60Hz：`[d3d] vblank: source DwmFlush (window)`；
- 独占 60Hz、kernel 可用：`[d3d] vblank: source the display's vblank (exclusive)`；
- 独占 60Hz、强制 kernel 不可用：`[d3d] vblank: source 59.94 Hz timer`；
- 独占 60Hz、固定游戏速度开：`[d3d] vblank: source 59.94 Hz timer`；
- Alt+Enter 切回窗口：来源从 kernel 切到 `DwmFlush`；
- 启动器 On/Off 立即写入 `black.ini`；
- 启动器在 125% DPI 下 On/Off 完整显示。

## 说明文字修正

- 「接缝填补」原本写「保持开启即可」，但 2.2.32 起默认是关；已改成「默认关闭，只有接缝明显才开；MSAA 开启时无效」。
- 「垂直同步时用 60 Hz」补上 120/180/240 不需要、144/165 才有用、CG 变慢或 Alt+Tab 卡时先关。
- 「低延迟呈现」补上真全屏 / 高刷下出现 CG 变速、45/24 帧、Alt+Tab 卡顿时先关。

## 发布包

- `ngb-hurricane-pc-2.2.37.5-win32.zip`
  - 3,445,744 字节
  - SHA-256：`5882466d489d9456cef3ee61e5008c37c6dcfa8d3441a23a0b04beac04db229d`
  - 压缩包内附：`Smoothness-Settings-CN.md`、`Smoothness-Settings-EN.md`（中文 / 英文流畅度设置指南）。
- `ngb-hurricane-pc-2.2.37.5-win32-symbols.zip`
  - 39,671,903 字节
  - SHA-256：`3c816baa53d2827ea7826ab20b4db52240425ead4ffe9518c9a1d8061712c1fc`

作者：b站 哈雅布撒（QQ：2682557218）

---

# Ninja Gaiden Black PC native port **2.2.37.5**

> **Superseded by 2.2.37.6.** 2.2.37.5 fixed only kernel vblank initialization failure; when initialization succeeds but the event itself slows down (16.7 → 19–21 ms), it can still fall to 52. Use 2.2.37.6.

**A fix release for 2.2.37.4.** It fixes 60 Hz exclusive fullscreen falling back to DwmFlush when kernel vblank is unavailable.

## What this build is

```text
base: 2.2.37.4
fix: 60 Hz exclusive + kernel vblank unavailable no longer falls back to DwmFlush
fix: kernel unavailable uses the fixed 59.94 Hz timer
new: kernel retries every 300 ticks and after a display-mode change
new: logs the kernel failure reason and every vblank source change
```

## What this fixes

The old build used DwmFlush in 60 Hz exclusive fullscreen when kernel vblank initialization failed. DWM does not compose the game in exclusive fullscreen, so DwmFlush returned late:

- gameplay around 52;
- a 30 fps cutscene around 24.

2.2.37.5 changes this:

- exclusive fullscreen never uses DwmFlush;
- kernel unavailable means the fixed timer;
- kernel is retried every 300 ticks;
- a failed kernel wait returns to the timer.

This does not include Presenter / frame pacing, so sparse CG / menu presentation can still be slow.

## What Steady game speed does

It pins the **game clock** to a 59.94 Hz QPC timer instead of following the display / VRR / the exclusive-fullscreen
kernel vblank.

It is not a frame cap:

- it does not cap the render rate;
- it does not guarantee 60 fps;
- it only keeps the game logic / tick's base clock independent of the screen.

When to use it:

> Turn it on when CG, cutscenes or combat clearly run fast or slow. The game clock stays at 59.94 Hz and no longer
> follows the screen; it does not cap frames. Applies from the next start.

## Switch behaviour

- Toggling it writes `timer_vblank = 1/0` to `black.ini` at once;
- closing the launcher without starting the game keeps the choice;
- the game reads `black.ini` only when it starts, so there is no live change in a running game;
- with the switch off, the game's runtime path is close to 2.2.37.4, but the 2.2.37.5 vblank source fix still applies.

## Explicitly not included

- a presenter thread;
- a complete Present / Reset / focus / occluded state machine;
- non-blocking Present;
- a frame-rate cap;
- sparse CG / menu presentation or frame pacing;
- fixes for the risks of the 60 Hz display-mode switch.

So:

- `timer_vblank` cannot stop Present from blocking, an Alt+Tab device reset, or the black screen of a mode switch;
- sparse CG / menu presentation can still be slow; that needs Presenter / frame pacing;
- **60 Hz with V-Sync** still changes the display mode and still carries the black-screen / Alt+Tab / cutscene risk;
- 120 / 240 Hz divide 60 exactly and do not need it;
- 144 / 165 Hz gain evenness, at the cost of the mode switch.

## Tested

- window 60 Hz: `[d3d] vblank: source DwmFlush (window)`;
- exclusive 60 Hz, kernel available: `[d3d] vblank: source the display's vblank (exclusive)`;
- exclusive 60 Hz, kernel forced unavailable: `[d3d] vblank: source 59.94 Hz timer`;
- exclusive 60 Hz with Steady game speed: `[d3d] vblank: source 59.94 Hz timer`;
- Alt+Enter to a window: source changes from kernel to `DwmFlush`;
- the launcher writes `black.ini` immediately on On/Off;
- the launcher's On/Off switch is fully visible at 125% DPI.

## Help-text corrections

- Seam fill used to say "It can stay on"; since 2.2.32 it is off by default, so it now says "Off by default; only for visible cracks; MSAA makes it do nothing".
- 60 Hz with V-Sync now notes that 120/240 do not need it, 144/165 may, and to turn it off first if a cutscene slows or Alt+Tab stutters.
- Low-latency present now notes to turn it off first for cutscene-speed changes, 45/24 fps, or Alt+Tab stutter.

## Packages

- `ngb-hurricane-pc-2.2.37.5-win32.zip`
  - 3,445,744 bytes
  - SHA-256: `5882466d489d9456cef3ee61e5008c37c6dcfa8d3441a23a0b04beac04db229d`
  - Inside: `Smoothness-Settings-CN.md` and `Smoothness-Settings-EN.md` (Chinese / English smoothness guides).
- `ngb-hurricane-pc-2.2.37.5-win32-symbols.zip`
  - 39,671,903 bytes
  - SHA-256: `3c816baa53d2827ea7826ab20b4db52240425ead4ffe9518c9a1d8061712c1fc`

Author: bilibili 哈雅布撒 (QQ: 2682557218)
