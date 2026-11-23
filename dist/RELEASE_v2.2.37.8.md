# 《忍者龙剑传 黑之章》PC 原生移植 **2.2.37.8**

**窗口 / 真全屏状态整理，以及真全屏 vsync 关的呈现路径统一。**

## 这个版本是什么

```text
基线：2.2.37.7
整理：窗口 / 独占 / 焦点 / Reset 收进单一 WindowState
修正：无边框全屏不再只靠 WS_CAPTION 推断
修正：Alt+Enter 退出真全屏后，设置会正确保存为窗口
修正：独占 display mode 读不到时回退窗口，不留下半全屏状态
修正：SetWindowPos 失败时回滚窗口
统一：真全屏 + vsync 关也使用 DISCARD / 2 buffers / flip
保留：窗口 / 无边框 COPY + DWM
保留：真全屏 + vsync 开 flip / INTERVAL_ONE
```

## 玩家会感觉到什么

### 正常窗口 / 无边框

没有变化：

```text
COPY + DWM
```

速度和延迟应与 2.2.37.7 相同。

### 真全屏 + 垂直同步开

没有变化：

```text
DISCARD / 2 buffers / flip / INTERVAL_ONE
```

### 真全屏 + 垂直同步关

呈现路径从：

```text
COPY / 1 buffer / Present(&src)
```

改为：

```text
DISCARD / 2 buffers / Present(nullptr) / INTERVAL_IMMEDIATE
```

可能感觉到的差别：

- 撕裂位置不同；
- 延迟可能变低，也可能因双 buffer 排隊而不同；
- 高负载时可能更顺，也可能更不稳。

这是本版最需要留意的行为变化。遇到明显变差，请回报显卡、刷新率、vsync 设置和日志。

### Alt+Enter

- 退出真全屏后，关闭游戏再开不会又变全屏；
- 全屏 style 和 device 不再容易卡在半状态；
- display mode 读不到时会回退窗口；
- `SetWindowPos` 失败会回滚。

## 技术说明

- `WindowState` 保存 requested / presentation / fullscreen / focus / restyle / reset；
- `presentation` 是一个 atomic，Present 和 frame layout 不会看到两个不同 swapchain；
- Reset 请求、原因、lost 时间也在同一个状态对象；
- 真全屏的 flip / copy 路径由实际 `D3DPRESENT_PARAMETERS` 决定。

详细记录：`project/docs/26_Present模式状态_2.2.37.7.md`。

## 验证

```text
窗口：present copy (window)
无边框：fullscreen → Alt+Enter → windowed
真全屏 + vsync 开/关：present flip
真全屏 Alt+Enter：reset → windowed copy → restyle
Alt+Enter 退出后 black.ini：fullscreen = 0
```

## 不包含

- Presenter 线程；
- 非阻塞 Present；
- CG / 主选单的 frame pacing；
- 新的 vblank / timer 策略。

## 发布包

- `ngb-hurricane-pc-2.2.37.8-win32.zip`
  - 3,448,898 字节
  - SHA-256：`163dfa912d7f2430304e41cbc02a7ae8a775a7f48a9755ed5a6f8a09696fd7ee`
  - 压缩包内附：`Smoothness-Settings-CN.md`、`Smoothness-Settings-EN.md`。
- `ngb-hurricane-pc-2.2.37.8-win32-symbols.zip`
  - 38,770,294 字节
  - SHA-256：`7117f1a5cdc410856f9dd5cc070666faafd4f9ee9a5ffc0092b6382ba2c266c0`

作者：b站 哈雅布撒（QQ：2682557218）

---

# Ninja Gaiden Black PC native port **2.2.37.8**

**Window / exclusive state cleanup, and one presentation path for exclusive fullscreen with vsync off.**

## What this build is

```text
base: 2.2.37.7
refactor: window / exclusive / focus / reset in one WindowState
fix: borderless fullscreen is no longer inferred from WS_CAPTION only
fix: leaving exclusive with Alt+Enter now saves the windowed choice
fix: an unavailable exclusive display mode falls back to a window
fix: a failed SetWindowPos rolls the window style back
unify: exclusive + vsync off also uses DISCARD / 2 buffers / flip
keep: window / borderless COPY + DWM
keep: exclusive + vsync on flip / INTERVAL_ONE
```

## What players may notice

### Windowed / borderless

Unchanged:

```text
COPY + DWM
```

Speed and latency should match 2.2.37.7.

### Exclusive fullscreen + vsync on

Unchanged:

```text
DISCARD / 2 buffers / flip / INTERVAL_ONE
```

### Exclusive fullscreen + vsync off

The path changed from:

```text
COPY / 1 buffer / Present(&src)
```

to:

```text
DISCARD / 2 buffers / Present(nullptr) / INTERVAL_IMMEDIATE
```

Possible differences:

- tearing position;
- latency may be lower, or different because of the second buffer;
- heavier loads may be smoother, or less stable.

This is the change to watch. If it is clearly worse, report the GPU, refresh rate, vsync setting and log.

### Alt+Enter

- leaving exclusive no longer saves fullscreen for the next start;
- the fullscreen style and device no longer stay half-transitioned;
- an unavailable display mode falls back to a window;
- a failed SetWindowPos rolls back.

## Technical notes

- `WindowState` owns requested / presentation / fullscreen / focus / restyle / reset;
- `presentation` is one atomic: Present and frame layout cannot see two swapchains;
- reset requests, reasons and lost time live in the same state object;
- the exclusive flip / copy path comes from the actual `D3DPRESENT_PARAMETERS`.

Details: `project/docs/26_Present模式状态_2.2.37.7.md`.

## Verification

```text
window: present copy (window)
borderless: fullscreen -> Alt+Enter -> windowed
exclusive + vsync on/off: present flip
exclusive Alt+Enter: reset -> windowed copy -> restyle
black.ini after leaving exclusive: fullscreen = 0
```

## Not included

- a Presenter thread;
- non-blocking Present;
- CG / main-menu frame pacing;
- a new vblank / timer policy.

## Packages

- `ngb-hurricane-pc-2.2.37.8-win32.zip`
  - 3,448,898 bytes
  - SHA-256: `163dfa912d7f2430304e41cbc02a7ae8a775a7f48a9755ed5a6f8a09696fd7ee`
  - Inside: `Smoothness-Settings-CN.md` and `Smoothness-Settings-EN.md`.
- `ngb-hurricane-pc-2.2.37.8-win32-symbols.zip`
  - 38,770,294 bytes
  - SHA-256: `7117f1a5cdc410856f9dd5cc070666faafd4f9ee9a5ffc0092b6382ba2c266c0`

Author: bilibili 哈雅布撒 (QQ: 2682557218)
