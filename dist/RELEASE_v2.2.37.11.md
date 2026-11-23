《忍者龙剑传 黑之章》（含飓风版中文汉化 vb-zh-x2.1）与《忍者龙剑传》（2004）的非官方 Windows 原生移植。**2.2.37.11**。

## 2.2.37.11：启动即隐藏鼠标，Esc 放开鼠标

### 先看这里：这次更新对你有什么影响

| 你的情况 | 更新后 |
|---|---|
| 启动游戏（窗口、无边框、全屏都一样） | 光标立即消失，不用再先点一下画面 |
| 从游戏进入 SFC 三部曲、街机版或 Xbox 仪表盘 | 新程序启动时光标同样直接消失 |
| 想拿鼠标去做别的事 | 按 **Esc** 放开鼠标，光标立即出现；点一下画面重新抓住（这一下不会传给游戏） |
| 其他 | Alt+Tab、最小化、拖动窗口的方式，以及画面、声音、游戏速度、性能，都和 2.2.37.10 相同 |

### 改了什么

1. **启动时抓住鼠标**：以前游戏启动后光标一直留在画面上，要点一下画面才消失；SFC 三部曲、街机版、仪表盘都是另起的新程序，所以每进一次都要再点一次。现在窗口第一次到前台时就抓住鼠标：光标隐藏，并限制在画面内。
2. **Esc 放开鼠标**：黑之章、NG2004、SFC 三部曲、仪表盘都可以用。放开后光标立即显示，点一下画面重新抓住。
3. **其余行为保持不变**：Alt+Tab 或最小化时放开，回来后点一下画面抓住；拖动窗口前先按 Esc。如果启动时 Windows 没把游戏放到前台，而你是点标题栏把它切到前面的，这次不会自动抓住，以免拖动窗口时光标被拉进画面；点一下画面即可。
4. **代码整理**：鼠标相关代码原来分散在窗口处理（8 个消息分支、7 个全局变量）、对外接口（6 个函数）和按键绑定（自己的 Esc 状态）三处，现在集中到一个模块，状态只在窗口线程上修改。顺带修正：
   - 抓住鼠标那一刻正按着的所有鼠标键，松开后才会传给游戏（以前只处理左键）；
   - 放开时立即恢复箭头光标（以前无边框下要动一下鼠标才出现）；
   - 鼠标放开时，按键设置页面的滚轮绑定也能用（原本的设计如此，但之前没生效）；
   - SFC、NG2004 等程序里不再累积用不到的鼠标移动量。
5. **README**：注明本项目所基于的 black-pc 移植的是 BLACK，中文名《黑煞》（Criterion / EA 2006 年的射击游戏），不是《忍者龙剑传 黑之章》。

### 测试

- **鼠标规则**（窗口与无边框各一遍，逐步检查光标是否隐藏、是否限制在画面内，以及日志）：启动即抓住 → Esc 放开、光标立即出现 → 点击抓住 → 最小化放开 → 恢复后保持放开（和以前一样）→ 点击抓住，全部符合预期。
- **性能**（待机演示 160 秒，与 2.2.37.10 各 2 次交错，核显）：游戏速度都是 93.7%；平均帧时间 3.71 / 3.70 对 3.72 / 3.73 ms。
- **冒烟测试**（发布包本身）：`Ninja Gaiden Black.exe` 跑黑之章 90 秒、`Ninja Gaiden.exe` 跑 2004 更新补丁版 20 秒，8493 个绘制状态全部回放、0 遗漏，都正常退出。
- **发布包**：Windows Defender 扫描四个程序，无威胁；14 个文件，和 2.2.37.10 相同；种子等数据文件与流畅度指南逐字节相同；程序文件属性显示 2.2.37.11。没有测试用代码。
- 符号包比上次小一半：这次是完整重新编译，PDB 里没有增量编译累积的旧记录；`blackrt.map` 大小相同，符号完整。

## 发布包

- `ngb-hurricane-pc-2.2.37.11-win32.zip`
- 大小：3,508,455 字节
- SHA-256：`74709f00993f295b241b62ac2d1dbae59109ca0a064b0a5a72d30c14606e4504`
- 符号包 `ngb-hurricane-pc-2.2.37.11-win32-symbols.zip`：14,869,173 字节
- SHA-256：`1f7e87d48709221e41823354cf1ecaec98c7e3772220b78ced1ba6a17f4c30a2`

作者：b站 哈雅布撒（QQ：2682557218）

---

## English

An unofficial native Windows port of Ninja Gaiden Black (including the Hurricane Chinese translation vb-zh-x2.1) and Ninja Gaiden (2004). **2.2.37.11**.

## 2.2.37.11: the mouse hidden from the start, Esc lets go of it

### Start here: what this update means for you

| Your setup | After the update |
|---|---|
| Starting the game (window, borderless or fullscreen) | The cursor goes at once; no click on the picture needed first |
| Going from the game into the SNES trilogy, the arcade game or the Xbox dashboard | The new program hides the cursor from its start too |
| You want the mouse for something else | Press **Esc**: the cursor shows at once; a click in the picture takes it back (that click does not reach the game) |
| Everything else | Alt+Tab, minimizing, moving the window, picture, sound, game speed and performance are as in 2.2.37.10 |

### What changed

1. **The mouse held from the start**: the cursor used to stay over the picture until a click; the SNES trilogy, the arcade game and the dashboard each start a new program, so every visit needed another click. Now the game takes the mouse the first time its window comes to the front: the cursor hidden and kept inside the picture.
2. **Esc lets go of the mouse**: in Ninja Gaiden Black, Ninja Gaiden 2004, the SNES trilogy and the dashboard. The cursor shows at once; a click in the picture takes it back.
3. **Everything else as before**: Alt+Tab or minimizing lets go, and a click in the picture takes it back; press Esc before moving the window. If Windows did not bring the game to the front at its start and you brought it there by clicking its title bar, it does not take the mouse then (it would pull the cursor into the picture during a move); a click in the picture does.
4. **Code cleanup**: the mouse code was spread over the window handling (8 message cases, 7 globals), the interface (6 functions) and the key bindings (an Esc flag of their own); it is one module now, its state changed on the window's thread only. Along the way:
   - all mouse buttons held as the game takes the mouse reach the game only once released (it was the left button only);
   - letting go shows the arrow at once (borderless showed it only at the next move);
   - with the mouse let go, the key bindings page can bind the wheel (meant so, but it did not work);
   - the SNES trilogy, Ninja Gaiden 2004 and the others no longer add up mouse movement they never read.
5. **READMEs**: black-pc, the port this project is built on, is for BLACK, Chinese name 《黑煞》 (Criterion / EA's 2006 shooter), not Ninja Gaiden Black.

### Tests

- **Mouse rules** (window and borderless, checking each step for a hidden cursor, the clip to the picture and the log): held at the start -> Esc lets go, cursor shown at once -> a click holds it -> minimized lets go -> restored stays free (as before) -> a click holds it; all as expected.
- **Performance** (attract demo 160 s, two runs each against 2.2.37.10, alternated, integrated GPU): game speed 93.7% in all; average frame 3.71 / 3.70 vs 3.72 / 3.73 ms.
- **Smoke tests** (the package itself): `Ninja Gaiden Black.exe` ran Black 90 s and `Ninja Gaiden.exe` the NG2004 title update 20 s; all 8,493 draw states replayed, none left out; both exited normally.
- **The package**: Windows Defender scan of the four programs, no threats; 14 files as 2.2.37.10; seed data files and smoothness guides byte for byte as 2.2.37.10; the programs' properties say 2.2.37.11. No test code.
- The symbols package is half the size of the last one: a full rebuild, so the PDB has none of the stale records incremental builds piled up; `blackrt.map` is the same size, the symbols are complete.

## Package

- `ngb-hurricane-pc-2.2.37.11-win32.zip`
- Size: 3,508,455 bytes
- SHA-256: `74709f00993f295b241b62ac2d1dbae59109ca0a064b0a5a72d30c14606e4504`
- Symbols `ngb-hurricane-pc-2.2.37.11-win32-symbols.zip`: 14,869,173 bytes
- SHA-256: `1f7e87d48709221e41823354cf1ecaec98c7e3772220b78ced1ba6a17f4c30a2`

Author: 哈雅布撒 on Bilibili (QQ: 2682557218)
