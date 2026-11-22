# vblank 来源修复（2.2.37.5）

> 已被 2.2.37.6 取代：60Hz 真全屏的 tick 现在直接固定用 timer，kernel vblank 不再作为游戏 tick 来源。本文保留为 2.2.37.5 的历史记录。

日期：2026-11-22。范围：60Hz 真全屏的 tick 来源。CG / 主选单的稀疏呈现不在这次修复内。

## 问题

60Hz 真全屏，即使关闭垂直同步，游戏仍可能掉到 52 左右；CG 约 24。Alt+Enter 回到窗口后游戏内恢复 60，但 CG / 主选单仍慢。

## 根因

`vblank_thread` 原来的选择顺序：

```text
kernel vblank 可用 && near_60 && exclusive
→ kernel vblank
否则如果 flush 可用
→ DwmFlush
否则
→ timer
```

`flush` 在 60Hz 下是 DwmFlush，但 fallback 没有检查 `exclusive()`。所以：

```text
60Hz 真全屏 + kernel 初始化失败
→ 落到 DwmFlush
→ 独佔下 DWM 不合成游戏画面
→ DwmFlush 晚返回
→ tick 约 52
```

垂直同步只影响 Present，不影响这个 tick 来源，所以关垂直同步救不了 52。

日志证据：60Hz 独佔日志只有：

```text
vblank: follows the display (DwmFlush at ~60 Hz)
```

没有 kernel vblank 后缀，说明 `KernelVblank::init()` 失败。

Alt+Enter 会走 `toggle_fullscreen()`，把 `g_exclusive_now` 设 false，device 变回窗口；窗口下 DwmFlush 有效，所以游戏内恢复 60。CG / 主选单 Present 次数少，仍会被 DWM 合并。

## 修复设计

引入统一的来源状态：

```text
Timer  = 固定 59.94 Hz
Dwm    = 仅窗口 60Hz
Kernel = 仅独佔 60Hz，且 init 成功
```

选择规则：

```text
独佔 + kernel ready + 60Hz → Kernel
窗口 + 60Hz + Dwm 可用       → Dwm
其他                         → Timer
```

关键约束：

```text
独佔全屏永远不允许 DwmFlush
kernel 不可用 → Timer，不是 DwmFlush
```

## 代码改动

`loader/src/d3d/device.cpp`：

1. `display_sync()` 改为 `load_dwm()`，只负责加载 DwmFlush，不再把显示刷新率混进加载条件。
2. `KernelVblank` 增加 `failure` 原因：gdi32、export、CreateDC、OpenAdapter、主显示器。
3. `vblank_thread` 增加 `Source { Timer, Dwm, Kernel }`。
4. `choose_source()` 统一选择来源，独佔分支不再落到 DwmFlush。
5. kernel 初始化失败后每 300 tick 重试；切模式 / 切独佔后可重新初始化。
6. kernel wait 失败时退回 Timer，下一次重试。
7. 日志记录来源变化和 kernel 失败原因，不再只有启动时一行。
8. 修正 `from_display` 只统计 Dwm，`from_kernel` 只统计 kernel。
9. gdi32 的 export 只加载一次，重试时复用，不在每 300 tick 重复 LoadLibrary。

设计原则：时间来源和显示来源分开；DwmFlush 只属于窗口；独佔没有 kernel 就用 timer。

## 验证证据

### 窗口 60Hz

```text
[d3d] vblank: source DwmFlush (window); DwmFlush available, kernel vblank not ready
```

### 独佔 60Hz，kernel 可用

```text
host: exclusive fullscreen 1920x1200 at 60 Hz, vsync on
[d3d] vblank: kernel vblank ready for exclusive fullscreen
[d3d] vblank: source the display's vblank (exclusive); DwmFlush available, kernel vblank ready
```

### 独佔 60Hz，强制 kernel 不可用

用临时测试开关强制 `KernelVblank::init()` 失败，构建后运行，验证 fallback；测试开关已移除，不在最终代码中。

```text
host: exclusive fullscreen 1920x1200 at 60 Hz, vsync on
[d3d] vblank: kernel vblank unavailable: forced unavailable for the fallback test
[d3d] vblank: source 59.94 Hz timer; DwmFlush available, kernel vblank not ready (see log)
```

这是这次修复的核心证据：独佔 + kernel 不可用 + DwmFlush 可用时，来源仍然是 Timer，不是 DwmFlush。

### 独佔 60Hz + 固定游戏速度

```text
[d3d] vblank: source 59.94 Hz timer; DwmFlush unavailable, kernel vblank not ready
```

### Alt+Enter 切回窗口

```text
host: Alt+Enter (key data 0x201c0001, style 0x14000000)
[d3d] reset asked (Alt+Enter: a window), frame 138
[d3d] vblank: source DwmFlush (window)
host: windowed
```

这条日志证明来源会随模式切换，不是启动时定死；独佔到窗口后立刻从 kernel 切到 Dwm。

### 版本

```text
BLACK loader 2.2.37.5 (blackrt)
```

## 还没解决

- CG 仍可能 24；
- 主选单仍可能偏慢；
- 这两个是稀疏 Present / frame pacing 的问题，不是 tick 来源；
- 需要 Presenter / frame pacing，不在这次改动内。

## 结论

2.2.37.5 修的是第一层：60Hz 独佔不再错误地使用 DwmFlush，kernel 不可用时回落到固定 timer。

如果 60Hz 独佔在 kernel 不可用的环境里仍然出现 52，先看日志是不是 `source 59.94 Hz timer`；如果是 timer 仍然 52，那问题就在 Present / GPU，不在 vblank 来源。
