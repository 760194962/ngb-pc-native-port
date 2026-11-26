# 《忍者龙剑传 黑之章》PC 原生移植 **2.3.0.1**

**2.3.0 的收尾修正：真全屏关 vsync 不再多等，低延迟呈现不再把撕裂带回画面中间。**

## 这个版本是什么

```text
基线：2.3.0
修正：真全屏 + vsync 关不再压住 Present
修正：低延迟呈现只在真全屏 + vsync 时提前送帧
修正：显示模式轻量重查 / 完整 timing 降频
修正：clock model 的 timing 改变与 FollowsUs 期限
保留：2.3.0 的帧节拍时钟、无边框切 60Hz、VRR hold、vsync 开 flip
```

## 玩家会感觉到什么

### 真全屏 + vsync 关

以前这条路径还会做一次 Present 等待：

```text
高刷屏幕：多等约 6ms
```

现在不再等待，画面仍按 2.3.0 的方式把节拍放在 vblank 前 1ms，让撕裂留在画面最上 / 最下边缘。

可能的感觉：

- 高刷下延迟略低；
- 撕裂位置仍在边缘；
- 不同屏幕 / 驱动仍可能有差异。

### 低延迟呈现

现在只在：

```text
真全屏 + vsync 开
```

才提前送帧。

真全屏 + vsync 关时不再提前，因为那会把撕裂线从边缘拉回画面中间。

窗口 / 无边框本来就不提前，行为不变。

### 其他

- 游戏速度不变；
- 窗口 / 无边框不变；
- vsync 开真全屏不变；
- VRR hold、无边框切 60Hz 仍是 2.3.0 的行为。

## 技术说明

- `frame_clock` 在 `exclusive && !vsync` 时不再 hold Present；`tick_phase` 维持 -1ms，让 flip 落在刷新间隙。
- `present_early` 的闸门由 `g_early_composed` 改为 `g_early_queued`：只有会排队 flip 的“真全屏 + vsync”才允许提前。
- 显示重查拆成：
  - 每 2 秒：只查当前模式大小 / 刷新率 / 色深；
  - 每 30 秒：完整 QueryDisplayConfig timing；
  - 窗口 / 独占状态变化仍每 15 tick 检查。
- Clock model：timing 改变直接进 Free；`leave_following` 只在 `FollowsUs` 期限到时离开。

## 验证

```text
建置：通过
基线：2.3.0 的 150 秒抓屏 / 21 种显示器仿真
本次：代码路径检查、建置、启动日志检查
```

**本次没有重新做 150 秒桌面复制抓屏，也没有在 Iris Xe / 120 / 240Hz 实机上重测。**

## 风险

- vsync 关的撕裂位置仍取决于屏幕 / 驱动；
- 模式改变后最多约 2 秒才会被轻量检查发现；
- 完整 timing 改为 30 秒一次，极端情况下显示时序变化会晚一点被跟上；
- 这不是 2.3.0 的完整重测，只是收尾修正。

## 发布包

- `ngb-hurricane-pc-2.3.0.1-win32.zip`
  - 3,530,011 字节
  - SHA-256：`23de62ab5a624d5aaf2c7b9ea165ed0580d05e409ec180d569b522fc7615bbbe`
  - 压缩包内附：`Smoothness-Settings-CN.md`、`Smoothness-Settings-EN.md`。
- `ngb-hurricane-pc-2.3.0.1-win32-symbols.zip`
  - 21,594,344 字节
  - SHA-256：`91311f09a0b589c242e95583b93ae22ff3d80834c5b88adf63d001d734cfa98f`

作者：b站 哈雅布撒（QQ：2682557218）

---

# Ninja Gaiden Black PC native port **2.3.0.1**

**A finishing fix for 2.3.0: exclusive fullscreen without V-Sync no longer waits, and low-latency present no longer moves the tear into the picture.**

## What this build is

```text
base: 2.3.0
fix: exclusive fullscreen without vsync no longer holds the Present
fix: low-latency present sends early only in exclusive fullscreen with vsync
fix: light mode recheck / less frequent full timing query
fix: clock model timing-change and FollowsUs deadline state
keep: 2.3.0 frame clock, borderless 60 Hz, VRR hold, exclusive vsync flip
```

## What players may notice

### Exclusive fullscreen + vsync off

This path used to hold the Present once:

```text
high-refresh screen: about 6 ms more
```

It no longer does. The 2.3.0 tick still sits 1 ms before vblank, keeping the tear at the top / bottom edge.

Possible differences:

- slightly lower latency on a high-refresh screen;
- the tear stays at the edge;
- screens and drivers may still differ.

### Low-latency present

It now sends early only in:

```text
exclusive fullscreen + vsync on
```

Without vsync it no longer does, because that would put the tear back in the middle.

Window / borderless already did not send early; unchanged.

### Everything else

- game speed unchanged;
- window / borderless unchanged;
- exclusive + vsync unchanged;
- VRR hold and borderless 60 Hz remain as in 2.3.0.

## Technical notes

- `frame_clock` no longer holds the Present when `exclusive && !vsync`; `tick_phase` stays -1 ms to put the flip in the blanking.
- `present_early` is gated by `g_early_queued` instead of `g_early_composed`: only exclusive fullscreen with vsync queues flips and may go early.
- Display rechecks are split:
  - every 2 s: current mode size / refresh / depth only;
  - every 30 s: full QueryDisplayConfig timing;
  - window / exclusive changes still every 15 ticks.
- Clock model: a timing change enters Free directly; `leave_following` leaves FollowsUs only at its deadline.

## Verification

```text
build: clean
baseline: 2.3.0's 150 s screen capture / 21-display simulation
this patch: code-path review, build, start-up log check
```

**No new 150 s Desktop Duplication run, and no Iris Xe / 120 / 240 Hz hardware retest for this patch.**

## Risk

- the tear position without vsync still depends on the screen / driver;
- a mode change can take up to about 2 s to be seen by the light check;
- full timing is now 30 s apart, so an extreme timing change is followed a little later;
- this is a finishing fix, not a full retest of 2.3.0.

## Packages

- `ngb-hurricane-pc-2.3.0.1-win32.zip`
  - 3,530,011 bytes
  - SHA-256: `23de62ab5a624d5aaf2c7b9ea165ed0580d05e409ec180d569b522fc7615bbbe`
  - Inside: `Smoothness-Settings-CN.md` and `Smoothness-Settings-EN.md`.
- `ngb-hurricane-pc-2.3.0.1-win32-symbols.zip`
  - 21,594,344 bytes
  - SHA-256: `91311f09a0b589c242e95583b93ae22ff3d80834c5b88adf63d001d734cfa98f`

Author: bilibili 哈雅布撒 (QQ: 2682557218)
