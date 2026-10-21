**简体中文** | [English](README.en.md)

# NGB PC Native Port（忍者龙剑传 黑之章 飓风版 · PC 原生加载器）

《忍者龙剑传 黑之章》飓风版中文汉化 **vb-zh-x2.1**（初代 Xbox）的非官方 Windows 原生移植。

`black.exe` 读取你自己的 `default.xbe`，把原版 x86 游戏代码直接放在 PC 上运行；Xbox 内核、Direct3D 8、DirectSound
等系统部分由加载器在 PC 上重新实现（HLE）。游戏代码不经模拟器逐条解释，也不在磁盘上修改。

本项目从 [black-pc](https://github.com/Spark198rus/black-pc)（BLACK 的原生移植）分支而来，在它的加载器上加入了
忍龙主程序与附带程序的支持。原有的 BLACK NTSC-U 支持仍然保留。

**需要自备游戏。** 本仓库不含任何游戏代码、数据或 Microsoft XDK 代码。

## 支持的版本

加载器按 XBE 的 SHA-256 识别版本，其他版本不运行。

| 程序 | SHA-256 |
|---|---|
| 主程序 `default.xbe`（vb-zh-x2.1） | `aa3c5bec2923f83ccb37fe6a63ea5516b26d477b53c679a1620d19e4a8e4e8f6` |
| `NinjaTomoe.xbe`（原版附带程序，日版） | `2aff93ad098b3b5762d294d9c17a44c36d04bfa31843e413cf5e2963f14282ff` |
| `NinjaTrilogy.xbe`（原版附带程序，美版） | `31f8efd073abba2ac339ee44f4b13011844afc470b166f826dc12f6b79ff5621` |
| `NinjaFCJ.xbe`（SFC 三部曲，日版，x2.1 加入） | `b5b8754e6008f3b2e773c1eb9afe50ee9decebf0eadb945a9953369b1b9f4f31` |
| `NinjaFCU.xbe`（SFC 三部曲，美版，x2.1 加入） | `9fac4bb5162b8a99c65c0fbb6289ee81db4c42bdbd253807e74261a68e3399b1` |

附带程序从主菜单的"街机版忍者龙剑传"和"SFC忍者龙剑传"进入：游戏调用 `XLaunchNewImage`，加载器以同样的参数重启并
载入目标 XBE。

## 现状

逐项结论与证据见 `project/docs/06_功能验证矩阵.md`。

- **汉化**：标题、主菜单、存读档页、暂停菜单 / 武器 / 招式表、章节标题卡、开场 CG 与实时过场字幕、结算画面、道具 /
  商店 / 地图 / 对白，均已验证。
- **飓风版功能**：交差之法两种模式、宁静腕轮、扳机选服装、妖刀鬼彻扣血、带金圣甲虫二周目、还魂护符开关、弓箭准星 /
  视角、任务模式，均已实际游玩验证。
- **附带程序**：街机版与 SFC 版从主菜单进入可以游玩。
- **存档**：存档、读档、存后读回同一格均已验证。存档是标准 Xbox 格式，复制到 xRevive 的存档目录后可以直接读取（已实测）。
- **声音**：软件 DirectSound 混音器，经 XAudio2 输出，带混响。
- **性能**：第 1 章战斗场景在 Intel Iris Xe 核显上平均帧时间 10.45 ms（60 fps 的预算是 16.68 ms；r178，
  `project/docs/05_音频输入存档.md`）。
- **输入**：XInput 手柄。

### 修过的原版缺陷

以下问题原机上同样存在，移植版修掉了。每项修正都可以用开关即时切回原样对照（`project/docs/07_证据盘点.md`）：

- 第 2 章水面：边缘的黑 / 白锯齿；镜头贴着水面时的拉长条纹（顶点上的除法改为逐像素除法）。
- 熔岩房间：右边的黑条（画面复制少画一列）、热浪效果的白斑、右边缘的模糊条。

## 已知问题

- 镜子：第一人称近看时，屏幕最右有一条带横纹的暗带。这是原版缺陷，反射镜头没有拍到镜子的这一块；已查明，决定不修
  （`project/docs/07_证据盘点.md`）。
- 3D 音效与混响是对 Xbox 的近似，尚未逐项核对（`project/docs/07_证据盘点.md` #9）。
- 排行榜"新增用户"：原机此时会进入仪表板，移植版改为重新启动本篇，窗口会关闭再打开。
- 不带启动数据直接运行 `NinjaFCJ.xbe` 会崩溃（玩家从主菜单进入不会走这条路）。
- 仍在核实中的模拟规则列在 `project/docs/07_证据盘点.md` 的核实进度里。

## 运行

1. 把 x2.1 镜像解包到一个文件夹（例如 `game\x21\`），核对 `default.xbe` 的哈希：

   ```powershell
   Get-FileHash game\x21\default.xbe -Algorithm SHA256
   ```

2. 启动（`project/play.bat` 用的就是这类命令）：

   ```
   black.exe --windowed --hdd savedata --log black.log game\x21\default.xbe
   ```

   - `--hdd <文件夹>`：Xbox 硬盘（存档与游戏设定）放在哪里。
   - `--windowed`：窗口模式。游戏里按 **Alt+Enter** 切换窗口 / 全屏，设定会保存。
   - `--screenshot <目录>`：定时截图，频率由 `--shot-every N`（每 N 帧一张，默认 60）决定。玩的时候如果开了截图，
     请把 N 设得很大（例如 `--shot-every 100000000`），否则每秒截一张图会造成卡顿。

`black.exe --help` 列出全部选项。其中一部分（视野、自动瞄准、PC SETTINGS 菜单等）是 BLACK 专用的，对忍龙不起作用。

## 构建

本项目用 Visual Studio 2017（x86 C++ 工具）与 CMake 构建，产出 32 位程序：

```
cmake -S loader -B build -G "Visual Studio 15 2017" -A Win32
cmake --build build --config RelWithDebInfo
```

得到 `build\RelWithDebInfo\black.exe` 与 `blackrt.dll`。`loader/CMakePresets.json` 里另有上游的 Visual Studio
2022 / 2026 预设。

## 诊断工具

用 `--press-file <文件>` 启动后，往这个文本文件里追加一行命令，游戏会即时执行：

| 命令 | 作用 |
|---|---|
| `shot:名字` | 截下一整帧，存为 `名字.png` |
| `trace` / `trace:N` | 记录下一帧（或之后 N 帧）的每个绘制：状态、贴图、顶点、调用者、顶点程序读到的常数，并转储渲染目标 |
| `cfg:pixwatch:X,Y` | 在追踪帧里逐个绘制回读屏幕上的一点，记下改动它的绘制 |
| `cfg:pixwatchrt:地址` | 同上，但盯某张渲染贴图（按数据地址） |
| `cfg:skipcaller:地址` | 跳过调用栈里含该返回地址的绘制，用来找是谁画了某个东西 |
| `cfg:<开关>:0` / `:1` | 即时关闭 / 打开各项修正，对照原样，例如 `rhwedge`、`edgemirror`、`vsdivide`、`rhwoffset` |

启动参数 `--debug-frame N` 可以直接指定要追踪的帧。

## 目录

| 位置 | 内容 |
|---|---|
| `loader/` | 加载器源码：`kernel/` 内核 HLE；`d3d/` Direct3D 8 → 9，NV2A 顶点程序与寄存器合成器译为 HLSL；`audio/` DirectSound 混音；`input/` 输入；`ngb/` 忍龙专用的地址表与补丁 |
| `project/docs/` | 过程文档：输入清单、工具链、静态分析、加载器与内核、图形、音频输入存档、功能验证矩阵、证据盘点 |
| `project/tools/` | 分析与比对脚本：XBE 普查、函数表、图像比对、xemu 存档导出等 |
| `notes/` | 上游 BLACK 的分析结果 |

反编译与反汇编输出、游戏文件、测试运行目录都只留在本地，不进仓库。

## 原理

`black.exe` 是一个链接在 XBE 基址的小程序，抢在 Windows 往这段地址放别的东西之前占住它。`blackrt.dll` 把
`default.xbe` 映射进这段地址，提供游戏导入的 Xbox 内核函数，并在 Xbox 库（Direct3D 8、DirectSound、XAPI）的入口
打补丁，转到 PC 上的实现。

忍龙主程序是 LTCG 构建（寄存器传参、函数内联），库函数地址表是逐个核对的；附带程序是普通 D3D8，地址表由
XbSymbolDatabase 的识别结果生成。每一条模拟规则都要有依据：原版程序、xemu / 文档、硬件规格或实测对照。依据记在
`project/docs/07_证据盘点.md`。

## 法律声明

本项目与 Koei Tecmo、Team NINJA 及 Microsoft 无关，也未获其认可。Ninja Gaiden 是 Koei Tecmo 的商标，Xbox 是
Microsoft 的商标。仓库不含游戏代码或数据，也不含 Microsoft Xbox 开发包的代码。请使用自己合法取得的游戏。

本项目是自由软件，以 GNU 通用公共许可证第 2 版或（由你选择）任何更新的版本发布，见 `LICENSE`。

## 致谢

- [black-pc](https://github.com/Spark198rus/black-pc)（Spark198rus）：本项目所基于的加载器。
- [ngbrecomp](https://github.com/gamboozino/ngbrecomp)：忍龙主程序的 D3D 符号表。
- [xemu](https://xemu.app) 与 [Cxbx-Reloaded](https://github.com/Cxbx-Reloaded/Cxbx-Reloaded)：Xbox 内核与 NV2A
  行为的参考（未复制代码）。
- [genma-pc](https://github.com/w1284603830/genma-pc)：同类的 Xbox 原生移植，CLIPPLANE 等行为的参照。
- [XbSymbolDatabase](https://github.com/Cxbx-Reloaded/XbSymbolDatabase)：识别游戏里的 Xbox 库函数。

开发：哈雅布撒（b 站），与 Claude Code 合作。
