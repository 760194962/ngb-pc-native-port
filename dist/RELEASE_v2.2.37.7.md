# 《忍者龙剑传 黑之章》PC 原生移植 **2.2.37.7**

**2.2.37.6 的边界修复。** Alt+Enter 切换窗口 / 真全屏时，立刻重查显示器是否 60Hz，不再等 300 tick。

## 这个版本是什么

```text
基线：2.2.37.6
修正：Alt+Enter 后立即刷新 display-60 状态
修正：高刷 + 60Hz 开关 + 回窗口时，不会短暂选到 165Hz DwmFlush
保留：60Hz 真全屏 tick 固定 timer
保留：窗口 60Hz 用 DwmFlush
```

## 为什么

2.2.37.6 里 `near_60` 每 300 tick 才更新。Alt+Enter 从 60Hz 真全屏回 165Hz 窗口时，`near_60` 还可能是旧的 true，于是短暂选了 DwmFlush，tick 可能跟 165Hz。

2.2.37.7 在 `host3d::exclusive()` 一变化时立刻重查：

```text
exclusive_now != last_exclusive
→ near_60 = display_near_60()
→ 再 choose_source()
```

## 验证

```text
host: Alt+Enter ...
vblank: source DwmFlush (window)
host: windowed

host: Alt+Enter ...
host: fullscreen ... (exclusive)
vblank: source 59.94 Hz timer (exclusive)
```

来源在同一个时间点切换，不再等 300 tick。

## 不包含

- Presenter 线程；
- 非阻塞 Present；
- CG / 主选单的稀疏呈现 / frame pacing。

## 发布包

- `ngb-hurricane-pc-2.2.37.7-win32.zip`
  - 3,446,780 字节
  - SHA-256：`069f002ccfa1cc122a6dc4c1d376ce9e5fc8a9aba3a7943d496e4c9cddaa1570`
  - 压缩包内附：`Smoothness-Settings-CN.md`、`Smoothness-Settings-EN.md`。
- `ngb-hurricane-pc-2.2.37.7-win32-symbols.zip`
  - 38,991,452 字节
  - SHA-256：`c0db2ee5cb096d94ea974ca6e2dd1a774c32527903dda61dfbbdcfea669492d2`

作者：b站 哈雅布撒（QQ：2682557218）

---

# Ninja Gaiden Black PC native port **2.2.37.7**

**An edge-case fix for 2.2.37.6.** Alt+Enter between windowed and exclusive fullscreen now rechecks the display's 60 Hz state immediately instead of waiting for 300 ticks.

## What this build is

```text
base: 2.2.37.6
fix: refresh the display-60 state on an exclusive/window change
fix: high refresh + 60 Hz switch + back to window no longer briefly selects 165 Hz DwmFlush
keep: 60 Hz exclusive tick is the fixed timer
keep: windowed 60 Hz uses DwmFlush
```

## Packages

- `ngb-hurricane-pc-2.2.37.7-win32.zip`
  - 3,446,780 bytes
  - SHA-256: `069f002ccfa1cc122a6dc4c1d376ce9e5fc8a9aba3a7943d496e4c9cddaa1570`
  - Inside: `Smoothness-Settings-CN.md` and `Smoothness-Settings-EN.md`.
- `ngb-hurricane-pc-2.2.37.7-win32-symbols.zip`
  - 38,991,452 bytes
  - SHA-256: `c0db2ee5cb096d94ea974ca6e2dd1a774c32527903dda61dfbbdcfea669492d2`

Author: bilibili 哈雅布撒 (QQ: 2682557218)
