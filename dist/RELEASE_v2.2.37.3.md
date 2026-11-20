《忍者龙剑传 黑之章》PC 原生移植 **2.2.37.3**。

这是一个“跳过 2.2.35”的稳定版：以 2.2.34 的呈现节拍为基线，只合并 2.2.36、2.2.36.1、2.2.36.2、2.2.36.3 和 2.2.37 的功能。

## 这个版本是什么

```text
基线：2.2.34
功能：2.2.36 / 36.1 / 36.2 / 36.3 / 2.2.37
不包含：2.2.35 全部
不包含：2.2.37.1 / 2.2.37.2
```

## 呈现行为

```text
60 Hz      → 跟屏幕（2.2.34 行为）
120/240    → 59.94 Hz timer（2.2.34 行为）
144/165    → 59.94 Hz timer
VRR        → 不再被 n×60 带快或带慢
```

所以：

- 120/240 VRR 不会 80；
- 也不会 24 / 40–43 的 n×60 减速；
- 游戏速度固定 59.94；
- 120/240 的画面均匀度回到 2.2.34。

## 明确不包含 2.2.35

- 120/180/240 Hz 的 n 次刷新节拍；
- FLIPEX / Present Statistics；
- present_early 的 deadline / miss / 退避；
- 2.2.35 的 VRR 检查。

因此也没有 2.2.37.1 / 2.2.37.2 的 fallback。

## 代价

- 2.2.35 的“太早刷新多等一次”不在；
- 2.2.34 的低延迟呈现仍是“无条件提前”，没有 2.2.35 的截止判断；
- 60 Hz VRR 没有额外的低速/高速 fallback；
- 120/240 可能出现重复帧或跳帧，但游戏速度正确。

## 包含的功能

- 2.2.36：屏幕白光开关、21:9 白光/商店/2004 过场；
- 2.2.36.1：白光开关在 16:9 / 4:3、日志限流；
- 2.2.36.2：剩余空间测试写入、Linux/Wine 字体；
- 2.2.36.3：D3D 启动失败提示；
- 2.2.37：两个游戏各自的程序、启动器改名。

## 审计结果

- 分支从 `v2.2.34` 开始；
- 提交历史中没有 2.2.35 的任何 commit；
- 源码中不含 `g_vblank_synced`、`display_div`、FLIPEX / Present Statistics 等 2.2.35 代码；
- 2.2.36 / 2.2.37 的 24 个功能 commit 全部按顺序重新应用；
- 只有三处冲突：
  1. README / CMake 版本号；
  2. D3D 启动失败提示的 host.cpp；
  3. 2.2.35 测试字段被移除后，2.2.36 屏幕闪光的上下文差异。
- 冲突均按“没有 2.2.35”解决，编译通过；
- 120 Hz 桌面启动日志确认：`59.94 Hz timer`。

## 发布包

- `ngb-hurricane-pc-2.2.37.3-win32.zip`
- 大小：3,495,704 字节
- SHA-256：`0a9ba3f628dbd3ece897bc1f4be780b41bf308d9907253684bef0769cb1bb7e5`

符号包：

- `ngb-hurricane-pc-2.2.37.3-win32-symbols.zip`
- 大小：39,800,105 字节
- SHA-256：`cd7a857b12ae1a472550cc508a5a1e5f5041d196f20013d83e7b40250a7a64dd`

作者：b站 哈雅布撒（QQ：2682557218）

---

A native Windows port of *Ninja Gaiden Black* (original Xbox): **2.2.37.3**.

This is the "skip 2.2.35" stable build: it takes **2.2.34's presentation clock** as its base and merges only the features of 2.2.36, 2.2.36.1, 2.2.36.2, 2.2.36.3 and 2.2.37.

## What this build is

```text
base:    2.2.34
features: 2.2.36 / 36.1 / 36.2 / 36.3 / 2.2.37
not included: all of 2.2.35
not included: 2.2.37.1 / 2.2.37.2
```

## Presentation behaviour

```text
60 Hz      -> follows the screen (2.2.34 behaviour)
120/240    -> 59.94 Hz timer (2.2.34 behaviour)
144/165    -> 59.94 Hz timer
VRR        -> no longer sped up or slowed by n x 60
```

So:

- no 80 fps at 120/240 VRR;
- no 24 / 40-43 fps n x 60 slowdown;
- the game runs at a fixed 59.94;
- the evenness at 120/240 goes back to 2.2.34.

## Explicitly not included from 2.2.35

- n-th-refresh pacing at 120/180/240 Hz;
- FLIPEX / Present Statistics;
- present_early's deadline / miss / back-off;
- 2.2.35's VRR check.

There are therefore no 2.2.37.1 / 2.2.37.2 fallbacks either.

## Cost

- 2.2.35's "wait past a too-soon refresh" is absent;
- 2.2.34's low-latency present still presents early unconditionally, without 2.2.35's deadline check;
- 60 Hz VRR has no extra slow/fast fallback;
- 120/240 may repeat or skip a frame, but the game speed is correct.

## Included features

- 2.2.36: screen-flash switch, 21:9 flash/shop/2004 cutscenes;
- 2.2.36.1: flash switch at 16:9 / 4:3, log rate limit;
- 2.2.36.2: free-space test write, Linux/Wine fonts;
- 2.2.36.3: Direct3D start-failure message;
- 2.2.37: a program for each game, launcher renamed.

## Audit

- the branch starts at `v2.2.34`;
- no 2.2.35 commit is in its history;
- the source contains no `g_vblank_synced`, `display_div`, FLIPEX / Present Statistics or other 2.2.35 code;
- all 24 feature commits of 2.2.36 / 2.2.37 were reapplied in order;
- three conflicts were resolved:
  1. README / CMake version;
  2. the Direct3D start-failure host.cpp;
  3. the 2.2.36 screen-flash context after removing 2.2.35's test fields;
- all conflicts were resolved with "no 2.2.35"; the build passes;
- a 120 Hz desktop start logs `59.94 Hz timer`.

## Packages

- `ngb-hurricane-pc-2.2.37.3-win32.zip`
- 3,495,704 bytes
- SHA-256 `0a9ba3f628dbd3ece897bc1f4be780b41bf308d9907253684bef0769cb1bb7e5`

Symbols:

- `ngb-hurricane-pc-2.2.37.3-win32-symbols.zip`
- 39,800,105 bytes
- SHA-256 `cd7a857b12ae1a472550cc508a5a1e5f5041d196f20013d83e7b40250a7a64dd`

Author: bilibili 哈雅布撒 (QQ 2682557218)
