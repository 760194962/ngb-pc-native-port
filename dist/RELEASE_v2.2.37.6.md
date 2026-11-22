# 《忍者龙剑传 黑之章》PC 原生移植 **2.2.37.6**

**2.2.37.5 的修复版。** 60Hz 真全屏的 tick 改为固定 59.94Hz timer，不再依赖 kernel vblank。

## 这个版本是什么

```text
基线：2.2.37.5
修正：60Hz 真全屏的 tick 固定用 timer
移除：kernel vblank 作为游戏 tick 来源
保留：窗口 60Hz 仍用 DwmFlush
保留：高刷 / 固定游戏速度仍用 timer
```

## 为什么

诊断版 `2.2.37.6-t` 的日志证明：60Hz 真全屏、vsync 关时，kernel vblank 的 `pre` 约 0.01–0.02ms，`blocked` 约 19.4ms，`raw` 约 19.4ms。也就是说，不是执行绪晚醒，也不是 Present 拖住，而是 `D3DKMTWaitForVerticalBlankEvent` 本身在 16.66ms 和 19–21ms 之间变化。

它会造成：

- 游戏内约 52；
- CG 约 24；
- 过一段时间自己回到 60，但不稳定。

所以 2.2.37.6 不再让 kernel vblank 决定游戏时间。

## 行为

```text
60Hz 真全屏       → 59.94Hz timer
窗口 60Hz         → DwmFlush
高刷 / VRR        → 59.94Hz timer
固定游戏速度开    → 59.94Hz timer
```

60Hz 真全屏的 tick 从此不受 kernel vblank 的 51–54Hz 影响。

## 不包含

- Presenter 线程；
- 非阻塞 Present；
- CG / 主选单的稀疏呈现 / frame pacing。

CG / 主选单的 tick 不再被 52 拖慢；如果画面仍然稀疏，那是 Presenter 的下一步。

## 验证

```text
BLACK loader 2.2.37.6
host: exclusive fullscreen 1920x1200 at 60 Hz, vsync off
vblank: source 59.94 Hz timer (exclusive)

BLACK loader 2.2.37.6
vblank: source DwmFlush (window)
```

60Hz 真全屏的 frame 60 / 120 约每 1 秒一组，tick 走 timer。

## 发布包

- `ngb-hurricane-pc-2.2.37.6-win32.zip`
  - 3,446,707 字节
  - SHA-256：`778e11ae68558fbb0d9f9fd4bd017b171c2b48da84d512cb06afd9ee3e7b83e8`
  - 压缩包内附：`Smoothness-Settings-CN.md`、`Smoothness-Settings-EN.md`。
- `ngb-hurricane-pc-2.2.37.6-win32-symbols.zip`
  - 38,854,745 字节
  - SHA-256：`fe62067a5bc7eb035135ca8486c1b3b5e140ad983eb05f0aa242ae9c7d59e567`

作者：b站 哈雅布撒（QQ：2682557218）

---

# Ninja Gaiden Black PC native port **2.2.37.6**

**A fix release for 2.2.37.5.** The 60 Hz exclusive-fullscreen tick is now a fixed 59.94 Hz timer instead of kernel vblank.

## What this build is

```text
base: 2.2.37.5
fix: 60 Hz exclusive fullscreen uses the timer for the tick
remove: kernel vblank as a game-tick source
keep: windowed 60 Hz uses DwmFlush
keep: high refresh / Steady game speed use the timer
```

## Why

The 2.2.37.6-t diagnostic log showed that with 60 Hz exclusive fullscreen and V-Sync off, `pre` was about 0.01–0.02 ms, `blocked` about 19.4 ms and `raw` about 19.4 ms. The vblank thread was not waking late, and Present was not holding it: `D3DKMTWaitForVerticalBlankEvent` itself varied between 16.66 ms and 19–21 ms.

That caused:

- gameplay around 52;
- a 30 fps cutscene around 24;
- occasional self-recovery to 60, but not stable.

2.2.37.6 therefore stops letting kernel vblank decide game time.

## Behaviour

```text
60 Hz exclusive     → 59.94 Hz timer
60 Hz windowed      → DwmFlush
high refresh / VRR  → 59.94 Hz timer
Steady game speed   → 59.94 Hz timer
```

## Not included

- a presenter thread;
- non-blocking Present;
- sparse CG / menu presentation or frame pacing.

## Tested

```text
BLACK loader 2.2.37.6
host: exclusive fullscreen 1920x1200 at 60 Hz, vsync off
vblank: source 59.94 Hz timer (exclusive)

BLACK loader 2.2.37.6
vblank: source DwmFlush (window)
```

## Packages

- `ngb-hurricane-pc-2.2.37.6-win32.zip`
  - 3,446,707 bytes
  - SHA-256: `778e11ae68558fbb0d9f9fd4bd017b171c2b48da84d512cb06afd9ee3e7b83e8`
  - Inside: `Smoothness-Settings-CN.md` and `Smoothness-Settings-EN.md`.
- `ngb-hurricane-pc-2.2.37.6-win32-symbols.zip`
  - 38,854,745 bytes
  - SHA-256: `fe62067a5bc7eb035135ca8486c1b3b5e140ad983eb05f0aa242ae9c7d59e567`

Author: bilibili 哈雅布撒 (QQ: 2682557218)
