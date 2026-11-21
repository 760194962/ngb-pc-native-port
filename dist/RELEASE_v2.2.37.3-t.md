# 《忍者龙剑传 黑之章》PC 原生移植 **2.2.37.3-t**

这是 **2.2.37.3** 的版本，只多了启动器的「稳定游戏速度」开关和相应设置，不改变关闭开关时的游戏行为。

## 这个版本是什么

```text
基线：2.2.37.3
新增：启动器「开始游戏」页的「稳定游戏速度」开关
新增：black.ini 的 timer_vblank
修正：--timer-vblank 在 black.ini 也有 timer_vblank 时的优先级
```

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
- 关闭这个开关时，游戏运行路径与 2.2.37.3 相同，只多一行日志 / 配置。

## 明确不包含

- Presenter 线程；
- Present / Reset / 焦点 / occluded 的完整状态机；
- 非阻塞 Present；
- 帧率上限；
- 对 60 Hz 模式切换风险的修复。

因此：

- `timer_vblank` 不能阻止 Present 卡住、Alt+Tab 的 device reset、全屏切模式黑屏；
- 「垂直同步时用 60 Hz」仍然会切显示模式，仍有黑屏 / Alt+Tab / 过场变慢的风险；
- 120 / 180 / 240 Hz 本来就能整除 60，不需要切 60 Hz；
- 144 / 165 Hz 才有视觉均匀度收益，但仍有模式切换代价。

## 已测试

- NGB：`timer_vblank = 1` → `[d3d] vblank: 59.94 Hz timer`；
- 2004（1.06.04）：`timer_vblank = 1` → `[d3d] vblank: 59.94 Hz timer`；
- NGB：`timer_vblank = 0` + 独占 60 Hz + V-Sync → `follows the display`；
- 启动器 On/Off 立即写入 `black.ini`；
- 启动器在 125% DPI 下 On/Off 完整显示。

## 说明文字修正

- 「接缝填补」原本写「保持开启即可」，但 2.2.32 起默认是关；已改成「默认关闭，只有接缝明显才开；MSAA 开启时无效」。
- 「垂直同步时用 60 Hz」补上 120/180/240 不需要、144/165 才有用、CG 变慢或 Alt+Tab 卡时先关。
- 「低延迟呈现」补上真全屏 / 高刷下出现 CG 变速、45/24 帧、Alt+Tab 卡顿时先关。

## 发布包

- `ngb-hurricane-pc-2.2.37.3-t-win32.zip`
  - 3,441,320 字节
  - SHA-256：`7900f21fabc137049c83d5a0ba68d71ca770a21a3447f40ff3366e7d5690291e`
- `ngb-hurricane-pc-2.2.37.3-t-win32-symbols.zip`
  - 40,376,141 字节
  - SHA-256：`35c3be5de51233921701c5656731bf47a4408ef8da2866215314e6984b357a91`

作者：b站 哈雅布撒（QQ：2682557218）

---

# Ninja Gaiden Black PC native port **2.2.37.3-t**

This is **2.2.37.3** with only the launcher's **Steady game speed** switch and its setting; with the
switch off, the game behaves as 2.2.37.3.

## What this build is

```text
base: 2.2.37.3
new: the Start page's Steady game speed switch
new: timer_vblank in black.ini
fix: --timer-vblank now wins over a timer_vblank line in black.ini
```

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
- with the switch off, the game's runtime path is the same as 2.2.37.3, apart from one log field / config line.

## Explicitly not included

- a presenter thread;
- a complete Present / Reset / focus / occluded state machine;
- non-blocking Present;
- a frame-rate cap;
- fixes for the risks of the 60 Hz display-mode switch.

So:

- `timer_vblank` cannot stop Present from blocking, an Alt+Tab device reset, or the black screen of a mode switch;
- **60 Hz with V-Sync** still changes the display mode and still carries the black-screen / Alt+Tab / cutscene risk;
- 120 / 180 / 240 Hz divide 60 exactly and do not need it;
- 144 / 165 Hz gain evenness, at the cost of the mode switch.

## Tested

- NGB: `timer_vblank = 1` -> `[d3d] vblank: 59.94 Hz timer`;
- 2004 (1.06.04): `timer_vblank = 1` -> `[d3d] vblank: 59.94 Hz timer`;
- NGB: `timer_vblank = 0` + exclusive 60 Hz + V-Sync -> `follows the display`;
- the launcher writes `black.ini` immediately on On/Off;
- the launcher's On/Off switch is fully visible at 125% DPI.

## Help-text corrections

- Seam fill used to say "It can stay on"; since 2.2.32 it is off by default, so it now says "Off by default; only for visible cracks; MSAA makes it do nothing".
- 60 Hz with V-Sync now notes that 120/180/240 do not need it, 144/165 may, and to turn it off first if a cutscene slows or Alt+Tab stutters.
- Low-latency present now notes to turn it off first for cutscene-speed changes, 45/24 fps, or Alt+Tab stutter.

## Packages

- `ngb-hurricane-pc-2.2.37.3-t-win32.zip`
  - 3,441,320 bytes
  - SHA-256: `7900f21fabc137049c83d5a0ba68d71ca770a21a3447f40ff3366e7d5690291e`
- `ngb-hurricane-pc-2.2.37.3-t-win32-symbols.zip`
  - 40,376,141 bytes
  - SHA-256: `35c3be5de51233921701c5656731bf47a4408ef8da2866215314e6984b357a91`

Author: bilibili 哈雅布撒 (QQ: 2682557218)
