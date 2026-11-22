# 《忍者龙剑传 黑之章》PC 原生移植 **2.2.37.7**

**2.2.37.6 的边界修复。** Alt+Enter 切换窗口 / 真全屏时，立刻重查显示器是否 60Hz，不再等 300 tick。

> **同一版本重新打包（2026-11-22）：** 增加「遊戲取得前景時自動捕獲滑鼠」，啟動 / Alt+Tab 回來不用再點一下；第一下點擊不會被吃掉。點標題欄 / 邊框時會先釋放滑鼠，避免 ClipCursor 把視窗點擊變成拖動。版本號仍為 2.2.37.7。

## 这个版本是什么

```text
基线：2.2.37.6
修正：Alt+Enter 后立即刷新 display-60 状态
修正：高刷 + 60Hz 开关 + 回窗口时，不会短暂选到 165Hz DwmFlush
新增：游戏取得前景时自动捕获鼠标，第一下点击不吞
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
  - 3,446,828 字节
  - SHA-256：`5f01925677d9dd498d059b75826a975647951dc78d734e7734495ced0b6fa95f`
  - 压缩包内附：`Smoothness-Settings-CN.md`、`Smoothness-Settings-EN.md`。
- `ngb-hurricane-pc-2.2.37.7-win32-symbols.zip`
  - 38,994,150 字节
  - SHA-256：`c3fbdaf4317f02ac85675a0458c4eb0c14276a84383ccfe0ee35d8bceb4a5a5b`

作者：b站 哈雅布撒（QQ：2682557218）

---

# Ninja Gaiden Black PC native port **2.2.37.7**

**An edge-case fix for 2.2.37.6.** Alt+Enter between windowed and exclusive fullscreen now rechecks the display's 60 Hz state immediately instead of waiting for 300 ticks.

> **Same version repackaged (2026-11-22):** the mouse is captured automatically when the game owns the foreground, so no click is needed after launch or Alt+Tab; the first click is not swallowed. A title-bar / resize click releases the mouse first, so ClipCursor cannot turn it into a drag. The version stays 2.2.37.7.

## What this build is

```text
base: 2.2.37.6
fix: refresh the display-60 state on an exclusive/window change
fix: high refresh + 60 Hz switch + back to window no longer briefly selects 165 Hz DwmFlush
new: capture the mouse when the game owns the foreground; the first click is not swallowed
keep: 60 Hz exclusive tick is the fixed timer
keep: windowed 60 Hz uses DwmFlush
```

## Packages

- `ngb-hurricane-pc-2.2.37.7-win32.zip`
  - 3,446,828 bytes
  - SHA-256: `5f01925677d9dd498d059b75826a975647951dc78d734e7734495ced0b6fa95f`
  - Inside: `Smoothness-Settings-CN.md` and `Smoothness-Settings-EN.md`.
- `ngb-hurricane-pc-2.2.37.7-win32-symbols.zip`
  - 38,994,150 bytes
  - SHA-256: `c3fbdaf4317f02ac85675a0458c4eb0c14276a84383ccfe0ee35d8bceb4a5a5b`

Author: bilibili 哈雅布撒 (QQ: 2682557218)
