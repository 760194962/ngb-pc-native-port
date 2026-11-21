《忍者龙剑传 黑之章》PC 原生移植 **2.2.37.2**。

这是一个修正版，只改游戏节拍来源，不碰画面、存档、音效和联网。

## 修复：VRR / 高刷下游戏速度错误

2.2.35 为了改善 120/180/240 Hz 的画面均匀度，让游戏每 n 次屏幕刷新走一帧。固定刷新时这是 60 Hz，但 VRR 下屏幕的合成节拍可能变成 80 Hz，游戏就跑到 **80 帧 / 4/3 速**。

2.2.37.1 只检查了“节拍变慢”，没有检查“变快”，所以 80 帧没有被拦住。

2.2.37.2 改成：

```text
60 Hz 屏幕      → 仍然跟随桌面合成
120/180/240 Hz  → 固定 59.94 Hz timer
VRR 120/240     → 固定 59.94 Hz timer
```

游戏 tick 不再跟这些屏幕走，因此：

- 不会 80 帧；
- 不会 4/3 速；
- 不会 24 / 40 帧的减速；
- 60 Hz 的屏幕对齐保持不变。

## 保留的两道防线

2.2.37.1 的低速 fallback 和 2.2.37.2 新增的高速 fallback 都保留，作为 **60 Hz 跟屏幕时**的最后防线：

- 连续 2 秒低于 59.94 的 90% → timer；
- 连续 2 秒高于 59.94 的 110% → timer。

在 120/180/240 下已经直接走 timer，这两道防线不会参与，也不会影响速度。

## 代价

120 / 180 / 240 Hz 的画面均匀度回到 2.2.34 的做法：

- 可能看到少量重复帧或跳帧；
- 但游戏速度正确。

这是有意的取舍：**先保证游戏速度，再谈高刷均匀度。**

## 测试情况

- 已在 120 Hz 桌面启动确认：日志写 `the display at 120 Hz: 59.94 Hz timer`，不再走 n 次刷新；
- 60 Hz 的跟屏幕路径保留；
- VRR 120 / 240 的实际效果还需要玩家反馈；
- 如果仍然出现 80 帧，请把 `logs\black.log` 里所有 `[d3d] vblank` 行发到 issue。

## 还没有做

这次没有把 Present 和游戏 tick 彻底拆开。  
目标架构（游戏 tick 固定 timer、呈现独立跟 VRR）另见设计文档；2.2.37.2 是稳定化的第一步。

## 发布包

- `ngb-hurricane-pc-2.2.37.2-win32.zip`
- 大小：3,502,445 字节
- SHA-256：`b5052bcc3a63e63bb18724b98c80f6010a66eb5f581ef0ad1031e03ce20e385d`

符号包：

- `ngb-hurricane-pc-2.2.37.2-win32-symbols.zip`
- 大小：39,124,535 字节
- SHA-256：`77dfed3501af13a479396cc3e22254802c11b6cf094d8818db6f3d16e646090c`

作者：b站 哈雅布撒（QQ：2682557218）

---

A native Windows port of *Ninja Gaiden Black* (original Xbox): **2.2.37.2**.

This is a fix release. It changes only the source of the game clock; graphics, saves, audio and networking are untouched.

## Fix: game speed under VRR / high refresh

2.2.35 paced the game once every n screen refreshes to even out 120/180/240 Hz. That is 60 Hz on a fixed refresh, but under VRR the display can give a different tick; a player reported **80 fps / 4/3 speed** at 120 Hz VRR.

2.2.37.1 checked only for a tick that was too slow, so 80 fps was not caught.

2.2.37.2 now uses:

```text
60 Hz      -> follows the desktop composition
120/180/240 -> fixed 59.94 Hz timer
VRR        -> fixed 59.94 Hz timer
```

The game tick no longer follows those displays, so it cannot be sped up or slowed down by them.

## Kept safety checks

The slow fallback from 2.2.37.1 and the new fast fallback remain for the 60 Hz display-follow path:

- two seconds below 90% of 59.94 Hz -> timer;
- two seconds above 110% of 59.94 Hz -> timer.

At 120/180/240 Hz the timer is used directly, so these checks do not participate in the game speed.

## Cost

The evenness at 120/180/240 Hz goes back to 2.2.34's behaviour:

- a few repeated or skipped frames are possible;
- the game speed is correct.

This is deliberate: correct game time first, presentation evenness later.

## Testing

- Verified on a 120 Hz desktop: the log says `the display at 120 Hz: 59.94 Hz timer`;
- the 60 Hz display-follow path is unchanged;
- VRR 120/240 still needs player reports;
- if 80 fps persists, send the `[d3d] vblank` lines from `logs\black.log`.

## Not done yet

Present and the game tick are not fully split. The target design is in `project/docs/15_呈现节拍目标架构设计.md` on the source repository.

## Packages

- `ngb-hurricane-pc-2.2.37.2-win32.zip`
- 3,502,445 bytes
- SHA-256 `b5052bcc3a63e63bb18724b98c80f6010a66eb5f581ef0ad1031e03ce20e385d`

Symbols:

- `ngb-hurricane-pc-2.2.37.2-win32-symbols.zip`
- 39,124,535 bytes
- SHA-256 `77dfed3501af13a479396cc3e22254802c11b6cf094d8818db6f3d16e646090c`

Author: bilibili 哈雅布撒 (QQ 2682557218)
