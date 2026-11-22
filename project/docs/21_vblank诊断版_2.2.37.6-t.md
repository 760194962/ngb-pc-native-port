# vblank 诊断版 2.2.37.6-t

用途：确认 60Hz 真全屏下 kernel vblank 变慢，是「事件本身晚到」还是「线程晚醒」。

## 测试包

```text
dist\ngb-hurricane-pc-2.2.37.6-t-win32.zip
```

这是诊断版，不是正式版。包内是 2.2.37.6-t，日志会多出 `vblank diag` 行。

## 测试设置

启动器 / black.ini：

```text
fullscreen = 2
vsync = 0
timer_vblank = 0
low_latency_present = 0
exclusive_60hz = 0
```

进入 60Hz 真全屏，游玩或播放 CG 至少 30 秒。最好覆盖：

- 游戏内正常移动；
- CG；
- 主菜单。

## 日志会多出什么

前 10 次 kernel wait：

```text
[d3d] vblank diag: kernel wait 1 raw 16.683 ms since the last tick (pre 0.095, blocked 16.589), status 0
```

每 300 次汇总：

```text
[d3d] vblank diag: kernel 300 waits, raw mean 17.338 ms (pre 0.015, blocked 17.323), min ... max ...; <15ms ..., 15-18 ..., 18-20 ..., 20-25 ..., >25 ...; late>18ms 32, fails 0
```

字段含义：

- `raw`：从上一次 tick 到这次 wait 返回的总时间；
- `pre`：进入 wait 前，已经离上一次 tick 过了多久；
- `blocked`：wait 实际阻塞了多久；
- `late>18ms`：raw 超过 18ms 的次数。

## 怎么读

如果：

```text
pre 很小，blocked 约 19ms
```

说明 kernel 事件本身晚到，不是线程晚醒。

如果：

```text
pre 约 2–3ms，blocked 约 16.7ms
```

说明事件本身正常，是 vblank 线程晚醒。

如果：

```text
blocked 约 16.7ms，但 raw 约 19ms
```

说明 wait 本身正常，线程在两次 wait 之间被拖住。

## 还要看

同一份日志里的：

```text
vblank: source ...
vblank: 3600 ... Hz
frame N presented
```

## 提交什么

把 `logs\black.log` 里包含以下内容的片段发来：

- `BLACK loader 2.2.37.6-t`
- `vblank: source ...`
- 前 10 条 `vblank diag`
- 至少一条 `vblank diag: kernel 300 waits`
- 一条 `vblank: 3600 ... Hz`

有这些就能判断是驱动事件、线程调度，还是 Present / GPU 拖住。
