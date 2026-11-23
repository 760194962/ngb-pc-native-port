《忍者龙剑传 黑之章》（含飓风版中文汉化 vb-zh-x2.1）与《忍者龙剑传》（2004）的非官方 Windows 原生移植。**2.2.37.10**。

## 2.2.37.10：SFC 三部曲音量提高，主菜单文字下的白点修复

### 先看这里：这次更新对你有什么影响

| 你的情况 | 更新后 |
|---|---|
| 玩 SFC 三部曲（黑之章飓风版或 NG2004 自带的） | 声音提高约 10 dB，和本体音量接近，不用再把系统音量开到很大 |
| 看主菜单、章节画面等界面的文字 | 字下面那排小白点没有了（1.1.10 起出现） |
| 其他 | 本体、街机版忍龙、画面节拍、性能都和 2.2.37.9 相同 |

### 修了什么

1. **SFC 三部曲声音太小**：原版 SFC 程序没有像本体那样把混音通道的余量设为 0，并且把自己的音量设成了 −4.43 dB，比本体小约 11 dB。我们在同一台机器上实测，xemu 和移植版的 SFC 一样小（−31.9 / −32.5 dBFS），说明这是原版就有的，不是移植的错误。现在只给 SFC 三部曲的输出提高 10.5 dB（补回这两项），响度约 −21.6 dBFS，和本体第一章的 −20.9 dBFS 接近。BGM 和所有音效一起提高，比例不变；最大输出只到满刻度的一半，不会爆音。**这是有意偏离原版的改进。** 黑之章飓风版的 SFC（NinjaFCJ / NinjaFCU）和 NG2004 自带的 SFC 都适用；黑之章本体、NG2004 本体和黑之章自带的街机版忍龙不变。
2. **文字下面的白点**：1.1.10 起，主菜单等界面每个字下面有一排小白点，字头也像被削了一点（玩家反馈）。原因是 2D 方块对齐像素（QuadSnap）时，采样范围按“原机像素中心”计算，线性过滤仍会读到字库里**下一个字的顶端**（约 44%），再经按透明度加权保留成了纯白。现在线性过滤的采样不会超出每个方块自己的纹理范围，所有用同一方式画的文字、图标都覆盖，不只是主菜单。暂停菜单匕首中间的细线（QuadSnap 当初修的问题）仍然没有。
3. **QuadSnap 结构整理**：以前会先移动方块，再看能不能加保护；用游戏自带像素着色器的绘制被移动了却没有保护。现在只移动能保护的方块，其余按 1.1.10 之前的方式画（实际游戏中只有两个读整张画面的后期处理受影响，画面差别 1～2 级，看不出来）。
4. 日志里黑之章自带的街机版忍龙不再误写成 “NES trilogy”。

### 测试

- **SFC 音量**（第一关各录 20 秒，系统回环录音）：修改前移植版 −32.5 dBFS、xemu −31.9 dBFS；修改后 −21.6 dBFS；黑之章第一章 −20.9 dBFS。混音器没有任何样本超过 0.8。
- **白点**（主菜单每个选项下方比背景亮出 12 级以上的点数）：1.1.9 都是 0；修复前 12～48；修复后 0。章节画面的说明文字和主菜单是同一张字库、同一条绘制路径。
- **不影响其他画面**：修复前后各走一遍标题、主菜单、读档、章节画面、游戏中、暂停菜单，逐像素对比，除动画区域外几乎相同；暂停菜单匕首没有细线。
- **只有文字和图标需要这项保护**：带统计的版本实际游玩一圈，91800 个被对齐的 2D 方块中，没拿到保护的只有两个读整张画面的后期处理，没有文字或图标。
- **性能**（待机演示 160 秒，与 2.2.37.9 各 2 次交错）：游戏速度都是 93.7%；平均帧时间 3.74 / 3.75 对 3.81 / 3.75 ms，在正常波动内。
- **冒烟测试**（发布包本身）：`Ninja Gaiden Black.exe` 跑黑之章 90 秒、`Ninja Gaiden.exe` 跑 2004 更新补丁版 20 秒，绘制状态全部回放、0 条遗漏，都正常退出。
- **发布包**：Windows Defender 扫描四个程序，无威胁；14 个文件，和 2.2.37.9 相同；种子等数据文件逐字节相同；程序文件属性显示 2.2.37.10。没有测试用代码。

## 发布包

- `ngb-hurricane-pc-2.2.37.10-win32.zip`
- 大小：3,509,180 字节
- SHA-256：`3fbd6959cc6344956a8643b3a2483e95b3569412cc487b3d3769f02a09992e69`
- 符号包 `ngb-hurricane-pc-2.2.37.10-win32-symbols.zip`：27,952,467 字节
- SHA-256：`682258e44eee04a32a530cb58b9d335c03d7e7f24531c319c78700de06bea5da`

作者：b站 哈雅布撒（QQ：2682557218）

---

## English

An unofficial native Windows port of Ninja Gaiden Black (including the Hurricane Chinese translation vb-zh-x2.1) and Ninja Gaiden (2004). **2.2.37.10**.

## 2.2.37.10: the SNES trilogy louder, no white dots under the main menu's letters

### Start here: what this update means for you

| Your setup | After the update |
|---|---|
| You play the SNES trilogy (Black Hurricane's or Ninja Gaiden 2004's) | About 10 dB louder, close to the main game; no need to turn the system volume far up |
| Text in the main menu, chapter screens and other menus | The row of small white dots under the letters is gone (there since 1.1.10) |
| Everything else | The main games, the arcade Ninja Gaiden, frame pacing and performance are as in 2.2.37.9 |

### What was fixed

1. **The SNES trilogy was too quiet**: the original SNES programs never set the mix bins' headroom to 0 as the main game does, and set their own voice to -4.43 dB: some 11 dB under the main game. Measured on one PC, xemu and the port play it equally quiet (-31.9 / -32.5 dBFS), so the original does this, not the port. Now the SNES trilogy's output alone is raised 10.5 dB (both given back): about -21.6 dBFS, next to Black's chapter 1 at -20.9 dBFS. Music and effects go up together; the output reaches half of full scale at most, so no clipping. **A deliberate change from the console.** It covers Black Hurricane's SNES trilogy (NinjaFCJ / NinjaFCU) and Ninja Gaiden 2004's own; the main games and Black's bundled arcade Ninja Gaiden are unchanged.
2. **White dots under letters**: since 1.1.10 each letter of the main menu and other screens had a row of small white dots under it and looked a little cut at the top (a player's report). With 2D quads snapped to pixels (QuadSnap), the read range followed the console's pixel centres, and linear filtering still took the **top of the next glyph** in the font (about 44%), which the alpha-weighted read kept white. A linear read now stays inside each quad's own texture area: all text and icons drawn this way are covered, not just the main menu. The pause menu dagger's thin line (what QuadSnap fixed) stays gone.
3. **QuadSnap restructured**: quads were moved first and protected only if possible; draws with the game's own pixel shader were moved without protection. Now only quads it can protect are moved; the others are drawn as before 1.1.10 (in play, only two post passes that read the whole picture; 1-2 levels, not visible).
4. The log no longer calls Black's bundled arcade Ninja Gaiden "NES trilogy".

### Tests

- **SNES volume** (20 s of the first stage each, system loopback recording): before, the port -32.5 dBFS and xemu -31.9 dBFS; now -21.6 dBFS; Black's chapter 1 -20.9 dBFS. No mixer sample over 0.8.
- **White dots** (points under each main menu item brighter than the background by 12+): 1.1.9 0 everywhere; before the fix 12-48; now 0. The chapter screens' text uses the same font and draw path.
- **Other screens unchanged**: title, main menu, load list, chapter screen, play and pause menu compared pixel by pixel before and after: nearly identical outside animated areas; no line down the pause menu dagger.
- **Only text and icons needed this**: a census build played around: of 91,800 snapped 2D quads, the only unprotected ones were two post passes reading the whole picture; no text or icons.
- **Performance** (attract demo 160 s, two runs each against 2.2.37.9, alternated): game speed 93.7% in all; average frame 3.74 / 3.75 vs 3.81 / 3.75 ms, within normal spread.
- **Smoke tests** (the package itself): `Ninja Gaiden Black.exe` ran Black 90 s and `Ninja Gaiden.exe` the NG2004 title update 20 s, all draw states replayed, none left out, both exited normally.
- **The package**: Windows Defender scan of the four programs, no threats; 14 files as 2.2.37.9; its seed data files byte for byte as 2.2.37.9; the programs' properties say 2.2.37.10. No test code.

## Package

- `ngb-hurricane-pc-2.2.37.10-win32.zip`
- Size: 3,509,180 bytes
- SHA-256: `3fbd6959cc6344956a8643b3a2483e95b3569412cc487b3d3769f02a09992e69`
- Symbols `ngb-hurricane-pc-2.2.37.10-win32-symbols.zip`: 27,952,467 bytes
- SHA-256: `682258e44eee04a32a530cb58b9d335c03d7e7f24531c319c78700de06bea5da`

Author: 哈雅布撒 on Bilibili (QQ: 2682557218)
