《忍者龙剑传 黑之章》（含飓风版中文汉化 vb-zh-x2.1）与《忍者龙剑传》（2004）的非官方 Windows 原生移植。**2.2.37.13**。

## 2.2.37.13：魂反护符触发时血条不再跳动，16:10 恢复居中的 HUD

### 先看这里：这次更新对你有什么影响

| 你的情况 | 更新后 |
|---|---|
| 21:9 或带鱼屏，魂反护符触发（复活）时 | 血条一直固定在左上角，不再在左边和中间之间来回跳 |
| 16:10 屏幕 | HUD 恢复整体居中：整套 16:9 布局在中间，上下留一点边（和 2.2.6 以前一样）；不再把血条等元素推到上下边缘 |
| 16:9、4:3 | 没有变化 |
| 其他 | 画面、声音、游戏速度、性能、鼠标、显示模式切换都和 2.2.37.12 相同 |

### 改了什么

1. **魂反护符触发时血条跳动**（21:9 及更宽，玩家反馈）：复活光效是一组很大的 2D 图片（约 200～290 × 170～260，画面为 720 × 480），以前和血条被归成同一个界面元素。这个元素的中心随光效扩大而移动，血条就在左边和中间之间来回跳。几块光效合起来还会被误认成“章节结算面板”，面板范围内的血条也被放回中间。
   现在界面上的 2D 块分三类：普通 HUD、量表（高而窄，如水下的氧气条）、画面特效（高超过画面 30%、宽超过 15%）。只在同类之间归组，画面特效始终按 16:9 构图居中。面板出现时，面板上新出现的内容跟着面板；面板出现前就在的 HUD 元素，只要整块落在原来的位置内，就保持在自己那一侧。
2. **16:10 恢复居中的 HUD**：16:10 比 16:9 窄，以前在游戏中把血条等元素分别贴到上下边缘；现在整套 16:9 布局居中显示（按你的意见，16:10 不做专门调整）。21:9 和带鱼屏仍然贴边。
3. **代码整理（行为不变）**：
   - 宽屏下 2D 的处理以前是一个近 200 行的函数，靠三个标志反复改写来判断。现在拆成画面形状、绘制分类（原样、只留中间 16:9、压缩）、沿轴压缩几部分，“16:9 区域”的计算只写一次。
   - 每个程序明确标注所属游戏（黑煞 / 黑之章 / NG2004 / 仪表盘）和角色（主程序、街机版、SFC、下载器、更新程序、仪表盘），各程序安装哪些补丁由一张表统一决定。以前这些判断分散在 20 个文件里，靠字段组合和字符串比较拼出来。

### 测试

- **魂反护符**（21:9，逐帧 HUD 日志）：两次触发，复活全过程血条都保持贴左上角，光效被识别为画面特效并居中。
- **其他画面不变**：同一份存档状态下，新旧版本各走一遍标题、主菜单、读档列表、章节画面、游戏中、暂停菜单。21:9 和 64:27 布局完全一致，仅动画部分（暂停菜单旋转的武器、章节文字淡入）有差别。16:10 只有游戏中的 HUD 回到居中。
- **程序识别重构**：本地 28 个程序（黑之章 x2.1 / 日版 x2.1 / va x2 / 美版 / 美版零售各自的主程序、街机版、SFC；NG2004 光盘版、更新版、SFC、下载器、更新程序；仪表盘；两个不支持的版本）启动到游戏入口前的安装日志，整理前后逐行一致；运行时的 SFC 音量提升、HUD 探针、退出菜单也一致。
- **性能**（待机演示 160 秒，与 2.2.37.12 各 2 次交错，核显）：游戏速度都是 93.7%；平均帧时间 3.73 / 3.75 对 3.76 / 3.77 ms。
- **冒烟测试**（发布包本身）：`Ninja Gaiden Black.exe` 跑黑之章 90 秒、`Ninja Gaiden.exe` 跑 2004 更新补丁版 20 秒，8493 个绘制状态全部回放、0 遗漏，都正常退出。
- **发布包**：Windows Defender 扫描四个程序，无威胁；14 个文件，和 2.2.37.12 相同；种子等数据文件与流畅度指南逐字节相同；程序文件属性显示 2.2.37.13。没有测试用代码。
- 未实测：章节结算画面在新面板规则下的效果（需要打完一章）。

## 发布包

- `ngb-hurricane-pc-2.2.37.13-win32.zip`
- 大小：3,511,831 字节
- SHA-256：`c6506df9db1eeaaa383d54ea73c22eacc455dc2a805f0a886162cfbb43c83553`
- 符号包 `ngb-hurricane-pc-2.2.37.13-win32-symbols.zip`：19,872,939 字节
- SHA-256：`b320259814aa7852448900138affbd03db317e5728ac1e205fadb839be0333e5`

作者：b站 哈雅布撒（QQ：2682557218）

---

## English

An unofficial native Windows port of Ninja Gaiden Black (including the Hurricane Chinese translation vb-zh-x2.1) and Ninja Gaiden (2004). **2.2.37.13**.

## 2.2.37.13: the health bar steady through the Talisman of Rebirth, 16:10 back to the centred HUD

### Start here: what this update means for you

| Your setup | After the update |
|---|---|
| 21:9 or an ultrawide, the Talisman of Rebirth reviving Ryu | The health bar stays in the top-left corner, no longer jumping between the side and the middle |
| A 16:10 screen | The HUD is centred again: the whole 16:9 layout in the middle with a little room above and below (as before 2.2.6), no longer pushed to the top and bottom edges |
| 16:9, 4:3 | No change |
| Everything else | Picture, sound, game speed, performance, the mouse and display mode switching are as in 2.2.37.12 |

### What changed

1. **The health bar jumped during the Talisman of Rebirth** (21:9 and wider, a player's report): the revival's glow is drawn as large 2D pictures (about 200-290 x 170-260 of 720 x 480), which were grouped with the health bar into one HUD element whose middle moved as the glow grew, so the health bar jumped between the side and the middle. Several of them together were also taken for a chapter clear's panel, which puts what is inside it in the middle.
   The 2D pieces are now of three kinds: the HUD, gauges (tall and narrow, like the breath meter under water) and screen effects (over 30% of the screen tall and 15% wide), grouped only with their own kind; a screen effect stays in the middle as the 16:9 picture has it. While a panel is up, what is new on it goes with the panel; a HUD element that was there before keeps its side for every piece wholly within it.
2. **16:10 centres the HUD again**: 16:10 is narrower than 16:9, and in play the HUD's elements were pushed to the top and bottom edges; now the whole 16:9 layout is shown in the middle (no tuning wanted at 16:10). 21:9 and the ultrawides keep the HUD at the sides.
3. **Code cleanup (no change in behaviour)**:
   - The 2D on a wide screen was one 200-line function deciding through three flags rewritten along the way; it is now the shape, what each draw gets (as drawn, only its 16:9 middle, squeezed) and the squeeze along one axis, the 16:9 part worked out once.
   - Every program names its game (BLACK / Ninja Gaiden Black / Ninja Gaiden 2004 / the dashboard) and role (main, arcade, SNES, downloader, updater, dashboard), and one table decides what each gets installed; before, those checks were spread over 20 files, pieced together from fields and string compares.

### Tests

- **Talisman of Rebirth** (21:9, the HUD logged frame by frame): two revivals, the health bar top-left throughout, the glow taken as a screen effect in the middle.
- **Other screens unchanged**: the old and new builds through the title, main menu, load list, chapter card, play and pause menu from the same save state: at 21:9 and 64:27 the same layout, animated parts only (the pause menu's turning weapon, the chapter text fading in); at 16:10 only the HUD in play moved back to the middle.
- **Program identity**: the 28 local programs (the main, arcade and SNES programs of Ninja Gaiden Black x2.1, jp x2.1, va x2, US and US retail; NG2004 disc, title update, SNES, downloader, updater; the dashboard; two unsupported builds) booted to their entry point install the same, line for line; the SNES gain, HUD probe and EXIT GAME row at run time too.
- **Performance** (attract demo 160 s, two runs each against 2.2.37.12, alternated, integrated GPU): game speed 93.7% in all; average frame 3.73 / 3.75 vs 3.76 / 3.77 ms.
- **Smoke tests** (the package itself): `Ninja Gaiden Black.exe` ran Black 90 s and `Ninja Gaiden.exe` the NG2004 title update 20 s; all 8,493 draw states replayed, none left out; both exited normally.
- **The package**: Windows Defender scan of the four programs, no threats; 14 files as 2.2.37.12; seed data files and smoothness guides byte for byte as 2.2.37.12; the programs' properties say 2.2.37.13. No test code.
- Not tested yet: a chapter clear's panel under the new rule (it needs a chapter played to its end).

## Package

- `ngb-hurricane-pc-2.2.37.13-win32.zip`
- Size: 3,511,831 bytes
- SHA-256: `c6506df9db1eeaaa383d54ea73c22eacc455dc2a805f0a886162cfbb43c83553`
- Symbols `ngb-hurricane-pc-2.2.37.13-win32-symbols.zip`: 19,872,939 bytes
- SHA-256: `b320259814aa7852448900138affbd03db317e5728ac1e205fadb839be0333e5`

Author: 哈雅布撒 on Bilibili (QQ: 2682557218)
