# Present 模式状态修正（2.2.37.7 工作区）

日期：2026-11-22。目标：Present 不再用「請求狀態」決定 flip / copy，而是用「device 實際 swapchain」。

## 問題

原本：

```text
if (g_exclusive_now && g_cfg.vsync)
    Present(nullptr, ...)
else
    Present(&src, ...)
```

`g_exclusive_now` 是 Alt+Enter 的請求；device 要等下一個 Swap 才 Reset。過渡期可能拿舊的 windowed device 走 exclusive Present。

而且實際有三條路徑：

```text
window             → COPY + rect
exclusive + vsync 關 → COPY + rect
exclusive + vsync 開 → DISCARD / 2 buffers + Present(nullptr)
```

## 設計

新增 `PresentationMode`：

```text
WindowedCopy
ExclusiveCopy
ExclusiveFlip
```

這個值是 **device 實際狀態**，只在 `CreateDeviceEx` / `ResetEx` 成功後更新：

```text
pp.Windowed                               → WindowedCopy
!Windowed && DISCARD && BackBufferCount>=2 → ExclusiveFlip
其他                                      → ExclusiveCopy
```

`Present` 只看這個模式：

```text
ExclusiveFlip → Present(nullptr)
其他          → Present(&src)
```

`g_exclusive_now` 仍然代表請求；它不再參與 Present 路徑。

## 保留行為

- 真全屏 + vsync 開：flip，`INTERVAL_ONE`；
- 真全屏 + vsync 關：copy，`INTERVAL_IMMEDIATE`；
- 窗口 / 無邊框：copy，DWM。

這次先修正狀態，不改變三者的實際 swapchain 行為。

## 測試

### 窗口 + vsync

```text
host: windowed: vsync is not used
host: present copy (window)
```

### 真全屏 + vsync 開

```text
host: exclusive fullscreen 1920x1200 at 165 Hz, vsync on, present flip
```

### 真全屏 + vsync 關

```text
host: exclusive fullscreen 1920x1200 at 165 Hz, vsync off, present copy (exclusive)
```

### Alt+Enter

```text
exclusive fullscreen, vsync on, present flip
→ device reset, windowed, vsync off, present copy (window)
→ device reset, exclusive fullscreen, vsync on, present flip
```

模式跟著實際 Reset 後的路徑切換，不再由 `g_exclusive_now` 猜。
