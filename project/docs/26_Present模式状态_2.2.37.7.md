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

`WindowState` 是唯一的窗口/device 狀態機，保存：

```text
requested  = Windowed / Exclusive（請求）
actual     = Windowed / Exclusive（device 實際）
present    = Copy / Flip（實際 swapchain）
focused    = 焦點
fullscreen = 視窗是否覆蓋螢幕（borderless 或 exclusive）
restyle_pending = 等 device 變 windowed 後再改視窗外觀

reset_requested = Reset 請求編號，0 = 無
reset_seq       = 最後一次請求編號
reset_reason    = 請求原因（和 requested 由 reset_lock 一起發布）
device_lost     = device 是否 lost
lost_since      = lost 開始時間
```

無邊框全屏現在由 `fullscreen` 表示，不再只靠 `WS_CAPTION` 推斷。`reset_requested` 是跨執行緒請求；Swap 用 `take_reset()` 消費，`recover_device()` 用同一份狀態執行 Reset。Reset 不再有第二份 `g_reset_requested`。

只有 `CreateDeviceEx` / `ResetEx` 成功後，`commit(pp)` 才更新 `actual` / `present`：

```text
pp.Windowed → WindowedCopy
!Windowed   → ExclusiveFlip
```

`Present` 只看 `present`：

```text
Flip → Present(nullptr)
Copy → Present(&src)
```

`requested` 只代表 Alt+Enter 的請求，不參與 Present。

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

## 邊界修正

- `exclusive_params()` 回報是否真的讀到 display mode；讀不到時請求回退到 windowed，並在 Reset 後 restyle，不留下「fullscreen style + windowed device」。
- `check_exclusive_present()` 在 create/reset 後檢查 exclusive 是否真的是 `DISCARD` + 2 buffers；不是就 log。
- `restyle_pending` 只在 `SetWindowPlacement` / `SetWindowPos` 都成功後才清除。
- 退出獨佔時先把 `g_cfg.fullscreen` 設回 false，`settings_save()` 不會再把「已選窗口」寫回全屏。
- capture/resume 仍以 `GetForegroundWindow()` 為主，`WindowState.focused` 只作狀態記錄，不阻擋原本可 capture 的情況。
- `g_bb_w/g_bb_h` 改為 atomic，避免 reset 執行緒寫、render 執行緒讀的 plain data race。

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
