# Present 模式状态修正（2.2.37.7 工作区）

日期：2026-11-22。目标：Present 不再用「請求狀態」決定 flip / copy，而是用「device 實際 swapchain」；同時把獨佔全屏收成一條 flip 鏈。

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
window               → COPY + rect
exclusive + vsync 關 → COPY + rect
exclusive + vsync 開 → DISCARD / 2 buffers + Present(nullptr)
```

## 設計

`PresentationMode` 只留兩種：

```text
WindowedCopy
ExclusiveFlip
```

這個值是 **device 實際狀態**，只在 `CreateDeviceEx` / `ResetEx` 成功後更新：

```text
pp.Windowed → WindowedCopy
!Windowed   → ExclusiveFlip
```

`Present` 只看這個模式：

```text
ExclusiveFlip → Present(nullptr)
WindowedCopy  → Present(&src)
```

`g_exclusive_now` 仍然代表請求；它不再參與 Present 路徑。

## 獨佔 swapchain

現在兩種 vsync 狀態都使用：

```text
SwapEffect = D3DSWAPEFFECT_DISCARD
BackBufferCount = 2
Present(nullptr)
```

差別只在：

```text
vsync 開：PresentationInterval = ONE
vsync 關：PresentationInterval = IMMEDIATE
```

這樣獨佔只有一條 flip 鏈，不再有 `ExclusiveCopy`。

## 測試

### 窗口

```text
host: present copy (window)
```

### 真全屏 + vsync 開

```text
host: exclusive fullscreen 1920x1200 at 165 Hz, vsync on, present flip
```

### 真全屏 + vsync 關

```text
host: exclusive fullscreen 1920x1200 at 165 Hz, vsync off, present flip
```

### Alt+Enter

```text
exclusive, vsync off, present flip
→ device reset, windowed, vsync off, present copy (window)
→ device reset, windowed, vsync off, present copy (window)
```

模式跟著實際 Reset 後的路徑切換。

## 風險

- vsync 關的獨佔從 COPY/1 buffer 改成 flip/2 buffers；
- 撕裂位置、延遲、CPU/GPU 重疊可能改變；
- 需要更廣泛測試 60 / 165Hz、不同驅動、Alt+Tab、模式切換；
- 若 flip 在某些驅動上不接受，CreateDeviceEx / ResetEx 會失敗，需要回退方案。
