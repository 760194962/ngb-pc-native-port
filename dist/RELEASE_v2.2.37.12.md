《忍者龙剑传 黑之章》（含飓风版中文汉化 vb-zh-x2.1）与《忍者龙剑传》（2004）的非官方 Windows 原生移植。**2.2.37.12**。

## 2.2.37.12：Alt+Enter 依次切换窗口、无边框、窗口、全屏

### 先看这里：这次更新对你有什么影响

| 你的情况 | 更新后 |
|---|---|
| 游戏中按 Alt+Enter | 依次切换：窗口 → 无边框 → 窗口 → 全屏 → 窗口 …，无边框和全屏之间总隔着窗口 |
| 每次切换 | 画面右下角显示约 4 秒，例如“显示模式：窗口（Alt+Enter → 全屏）”：现在是什么模式，下一次切到哪里 |
| 退出游戏 | 记住最后的模式，以及循环进行到哪一步；下次启动接着来 |
| 其他 | 画面、声音、游戏速度、性能、鼠标都和 2.2.37.11 相同 |

### 改了什么

1. **Alt+Enter 依次切换三种模式**：从全屏（无边框或独占）按 Alt+Enter 回到窗口；从窗口按 Alt+Enter 进入“下一种全屏”，并且每进入一种全屏，下一次就换成另一种。所以顺序总是窗口 → 无边框 → 窗口 → 全屏 → 窗口 …，无边框和全屏不会直接互切。以前从窗口只能切到启动器里选的那一种全屏。
2. **切换提示**：每次切换都在画面右下角显示当前模式，以及下一次 Alt+Enter 会切到哪里。进入或退出独占全屏时显示器会黑约 1.5 秒，所以提示显示 4 秒。显卡不支持独占模式而退回窗口时，会提示“全屏不可用”。
3. **退出时记住**：`black.ini` 的 `fullscreen`（最后的模式：0 窗口、1 无边框、2 全屏）和 `fullscreen_kind`（从窗口按 Alt+Enter 去哪种全屏：1 无边框、2 全屏）。启动器里选的显示模式决定启动时的模式。
4. **游戏内 PC SETTINGS 的全屏开关**遵循同一规则：开 = 切到下一种全屏，关 = 回到窗口。
5. **代码整理**：以前一个变量同时表示“当前是不是独占全屏”和“从窗口按 Alt+Enter 去哪种全屏”，读设置时要靠一个专门的函数按条件拼凑；一个函数包办所有进出路径。现在模式是一个枚举（窗口 / 无边框 / 全屏），设置里分成“当前模式”和“下一种全屏”两个含义单一的字段；切换规则只写在一处，进入全屏、离开全屏各一个函数。
6. **顺带修正一个状态不一致**：从独占全屏退回窗口要等下一帧设备重置完成。如果在这一帧之内又按 Alt+Enter，记录的模式和实际模式会不一致。现在设备还在切换时，新的 Alt+Enter 不生效，玩家几乎察觉不到。

### 测试

- **完整循环**（从窗口启动，按 6 次 Alt+Enter）：无边框 → 窗口 → 全屏 → 窗口 → 无边框 → 窗口，每一步的窗口样式、大小和设备状态都正确。
- **记住并接着循环**：无边框之后关闭，再启动按 Alt+Enter 直接进全屏；在全屏中关闭，以全屏启动后依次是窗口 → 无边框；`black.ini` 每次保存的两个值都正确。
- **切换途中连按**：全屏中同一瞬间按两下，第一下回到窗口，第二下被忽略（日志有记录），之后再按正常进入无边框。
- **游戏内全屏开关**：开、关、开、关依次为无边框 → 窗口 → 全屏 → 窗口。
- **提示**：在窗口、无边框、独占全屏三种模式下截图，右下角提示的文字都正确；启动器中英文说明截图检查排版正常。
- **切换时的停顿**：进入或退出独占全屏的那一帧停约 0.2～0.5 秒（切换显示模式本身的时间），与 2.2.37.11 相同。测试中有一次进入独占全屏时停了 2 秒，同样的步骤重复测试没有再出现。
- **性能**（待机演示 160 秒，与 2.2.37.11 各 2 次交错，核显）：游戏速度都是 93.7%；平均帧时间 3.73 / 3.73 对 3.73 / 3.71 ms。
- **冒烟测试**（发布包本身）：`Ninja Gaiden Black.exe` 跑黑之章 90 秒、`Ninja Gaiden.exe` 跑 2004 更新补丁版 20 秒，8493 个绘制状态全部回放、0 遗漏，都正常退出。
- **发布包**：Windows Defender 扫描四个程序，无威胁；14 个文件，和 2.2.37.11 相同；种子等数据文件逐字节相同；程序文件属性显示 2.2.37.12。没有测试用代码。

## 发布包

- `ngb-hurricane-pc-2.2.37.12-win32.zip`
- 大小：3,512,834 字节
- SHA-256：`4b7d4f78f28747bfb6a9b904343fd227a0234e490bc077118971189e4fe6b464`
- 符号包 `ngb-hurricane-pc-2.2.37.12-win32-symbols.zip`：19,298,132 字节
- SHA-256：`01a6513686dc104c3c3efd36557dfb049c473bddbb43ce02252e794d08365211`

作者：b站 哈雅布撒（QQ：2682557218）

---

## English

An unofficial native Windows port of Ninja Gaiden Black (including the Hurricane Chinese translation vb-zh-x2.1) and Ninja Gaiden (2004). **2.2.37.12**.

## 2.2.37.12: Alt+Enter goes round window, borderless, window, fullscreen

### Start here: what this update means for you

| Your setup | After the update |
|---|---|
| Alt+Enter in the game | Goes round: window -> borderless -> window -> fullscreen -> window ..., always a window between borderless and fullscreen |
| Each switch | About 4 s at the picture's bottom right, e.g. "Display mode: Window  (Alt+Enter: Fullscreen)": the mode now and where the next goes |
| Quitting | The last mode, and where the round is, are kept; the next start carries on |
| Everything else | Picture, sound, game speed, performance and the mouse are as in 2.2.37.11 |

### What changed

1. **Alt+Enter goes round the three modes**: from a fullscreen (borderless or exclusive) back to a window; from a window to the "next fullscreen", and each fullscreen entered makes the other one next. So it is always window -> borderless -> window -> fullscreen -> window ..., never borderless straight to fullscreen. From a window it used to go only to the fullscreen chosen in the launcher.
2. **A message on each switch**: the mode now and where the next Alt+Enter goes, at the picture's bottom right. Entering or leaving exclusive fullscreen blanks the screen for about 1.5 s, so it stays 4 s. Falling back to a window when the graphics card has no exclusive mode says "Fullscreen is not available".
3. **Kept on quitting**: `black.ini`'s `fullscreen` (the last mode: 0 window, 1 borderless, 2 fullscreen) and `fullscreen_kind` (where Alt+Enter goes from a window: 1 borderless, 2 fullscreen). The launcher's display mode is the one the game starts in.
4. **The in-game PC SETTINGS fullscreen switch** follows the same rule: on = the next fullscreen, off = a window.
5. **Code cleanup**: one variable meant both "exclusive now" and "where Alt+Enter goes from a window", pieced together by a function of its own as the settings were read, and one function did every way in and out. Now the mode is one enum (window / borderless / fullscreen), the settings have two fields of one meaning each (the mode now, the next fullscreen), the rule is written in one place, and entering and leaving fullscreen are a function each.
6. **A state mismatch fixed along the way**: going from exclusive fullscreen back to a window waits for the device's Reset at the next frame. Another Alt+Enter within that frame left the recorded mode and the real one different. Now a switch while the device is still switching is not taken; a player will hardly notice.

### Tests

- **The whole round** (started in a window, Alt+Enter 6 times): borderless -> window -> fullscreen -> window -> borderless -> window, with the right window style, size and device state at each step.
- **Kept and carried on**: quitting after borderless, then Alt+Enter after a restart went straight to fullscreen; quitting in fullscreen, a start in fullscreen then went window -> borderless; both `black.ini` values saved right each time.
- **Pressed twice mid-switch**: two presses at once in fullscreen: the first went to a window, the second was not taken (logged); the next press went borderless as it should.
- **In-game fullscreen switch**: on, off, on, off went borderless -> window -> fullscreen -> window.
- **The message**: screenshots in a window, borderless and exclusive fullscreen show the right text at the bottom right; the launcher's Chinese and English help checked in screenshots.
- **The pause on switching**: the frame that enters or leaves exclusive fullscreen takes about 0.2-0.5 s (the display mode change itself), as in 2.2.37.11. Once in testing, entering exclusive fullscreen took 2 s; the same steps repeated did not show it again.
- **Performance** (attract demo 160 s, two runs each against 2.2.37.11, alternated, integrated GPU): game speed 93.7% in all; average frame 3.73 / 3.73 vs 3.73 / 3.71 ms.
- **Smoke tests** (the package itself): `Ninja Gaiden Black.exe` ran Black 90 s and `Ninja Gaiden.exe` the NG2004 title update 20 s; all 8,493 draw states replayed, none left out; both exited normally.
- **The package**: Windows Defender scan of the four programs, no threats; 14 files as 2.2.37.11; seed data files byte for byte as 2.2.37.11; the programs' properties say 2.2.37.12. No test code.

## Package

- `ngb-hurricane-pc-2.2.37.12-win32.zip`
- Size: 3,512,834 bytes
- SHA-256: `4b7d4f78f28747bfb6a9b904343fd227a0234e490bc077118971189e4fe6b464`
- Symbols `ngb-hurricane-pc-2.2.37.12-win32-symbols.zip`: 19,298,132 bytes
- SHA-256: `01a6513686dc104c3c3efd36557dfb049c473bddbb43ce02252e794d08365211`

Author: 哈雅布撒 on Bilibili (QQ: 2682557218)
