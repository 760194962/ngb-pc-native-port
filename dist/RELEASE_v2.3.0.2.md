# 《忍者龙剑传 黑之章》PC 原生移植 **2.3.0.2**

**2.3.0.1 的低延迟呈现边界修正。**

## 这个版本是什么

```text
基线：2.3.0.1
修正：低延迟呈现只在真全屏 + 垂直同步开时提前
修正：窗口 / 无边框不再提前
修正：真全屏 + 垂直同步关不再提前
保留：真全屏 + 垂直同步开的 deadline 判断
保留：没有 deadline 时的旧 at-once
```

默认 `low_latency_present = 0`，所以不打开这个开关的玩家没有行为变化。

## 2.3.0.1 的问题

2.3.0.1 / 2.3.0.1-t 的 `present_before_tick()` 顺序是：

```text
deadline == 0 → 直接提前
然后才检查是不是真全屏 + vsync
```

所以某些模式下，低延迟仍然会提前送帧。

受影响的情况：

- 窗口 / 无边框 + 高刷或不可跟随显示；
- 真全屏 + vsync 关 + 高刷或不可跟随显示；
- 120 / 240Hz 多刷新的一拍。

可能表现：

- 窗口 / 无边框一帧重复、一帧丢失；
- vsync 关时撕裂线回到画面中间；
- 不会崩溃，不会改变游戏速度。

## 修正后的规则

```text
低延迟关闭 → 不提前
窗口 / 无边框 → 不提前
真全屏 + vsync 关 → 不提前

真全屏 + vsync 开：
  有 deadline → 按 deadline 判断
  没有 deadline → 保留旧 at-once
```

这里的“真全屏 + vsync”是实际 device 状态，不是 Alt+Enter 的请求状态。

## 技术说明

- `present_before_tick()` 先检查 `host3d::present_flip()` 与 `g_cfg.vsync`；
- 通过后才看 `g_early_deadline`；
- `deadline == 0` 不再绕过模式检查；
- 没有新增 120/240 的等待规则；
- 详细分析：`project/docs/28_低延迟呈现提前送错误分析_2.3.0.1.md`。

## 验证

```text
建置：通过
代码路径：窗口 / 无边框 / vsync 关先返回 false
真全屏 + vsync：保留 deadline / at-once
```

**本次没有在 120 / 240Hz 或 VRR 实机上重测低延迟。**

## 风险

- 低延迟仍是实验性功能，默认关闭；
- 没有 deadline 的 120/240 或不可跟随显示仍保持旧行为；
- 这次修的是模式边界，不是新增实测。

## 发布包

- `ngb-hurricane-pc-2.3.0.2-win32.zip`
  - 3,530,865 字节
  - SHA-256：`e1076529a4aebb351248a225c07d977f3d710af7d57ebd32cc6f02c2c7a6d857`
  - 压缩包内附：`Smoothness-Settings-CN.md`、`Smoothness-Settings-EN.md`。
- `ngb-hurricane-pc-2.3.0.2-win32-symbols.zip`
  - 22,467,515 字节
  - SHA-256：`ace08e7990873c15eb40011e7d18e8fbee7128a7fff44f10135e50333ac883be`

作者：b站 哈雅布撒（QQ：2682557218）

---

# Ninja Gaiden Black PC native port **2.3.0.2**

**Low-latency present boundary fix for 2.3.0.1.**

## What this build is

```text
base: 2.3.0.1
fix: low-latency present goes early only in exclusive fullscreen with vsync
fix: window / borderless no longer sends early
fix: exclusive fullscreen without vsync no longer sends early
keep: deadline gate for exclusive + vsync
keep: the old at-once path when there is no deadline
```

`low_latency_present = 0` by default, so players who do not turn it on see no change.

## The 2.3.0.1 problem

2.3.0.1 / 2.3.0.1-t ordered `present_before_tick()` as:

```text
deadline == 0 → send early
then check exclusive + vsync
```

So low-latency could still send early in modes where it should not.

Affected:

- window / borderless on a high-refresh or non-followed display;
- exclusive without vsync on such a display;
- a 120 / 240 Hz multi-refresh tick.

Possible result:

- one frame repeated, one lost in a window / borderless;
- the tear moves back into the picture without vsync;
- no crash and no game-speed change.

## The fixed rule

```text
low-latency off → no early
window / borderless → no early
exclusive without vsync → no early

exclusive with vsync:
  deadline present → use the deadline
  no deadline → keep the old at-once path
```

"Exclusive + vsync" is the actual device state, not the Alt+Enter request.

## Technical notes

- `present_before_tick()` first checks `host3d::present_flip()` and `g_cfg.vsync`;
- only then does it look at `g_early_deadline`;
- `deadline == 0` no longer bypasses the mode check;
- no new wait rule for 120 / 240 Hz;
- analysis: `project/docs/28_低延迟呈现提前送错误分析_2.3.0.1.md`.

## Verification

```text
build: clean
code path: window / borderless / vsync off return false first
exclusive + vsync: deadline / at-once preserved
```

**No new low-latency run on 120 / 240 Hz or VRR hardware.**

## Risk

- low-latency present is still experimental and off by default;
- 120 / 240 and non-followed displays keep the old behavior when there is no deadline;
- this fixes the mode boundary, not a new measurement.

## Packages

- `ngb-hurricane-pc-2.3.0.2-win32.zip`
  - 3,530,865 bytes
  - SHA-256: `e1076529a4aebb351248a225c07d977f3d710af7d57ebd32cc6f02c2c7a6d857`
  - Inside: `Smoothness-Settings-CN.md` and `Smoothness-Settings-EN.md`.
- `ngb-hurricane-pc-2.3.0.2-win32-symbols.zip`
  - 22,467,515 bytes
  - SHA-256: `ace08e7990873c15eb40011e7d18e8fbee7128a7fff44f10135e50333ac883be`

Author: bilibili 哈雅布撒 (QQ: 2682557218)
