**简体中文** | [English](README.en.md)

# NGB PC Native Port（忍者龙剑传 黑之章 飓风版 · PC 原生加载器）

《忍者龙剑传 黑之章》飓风版中文汉化 **vb-zh-x2.1**（初代 Xbox）的非官方 Windows 原生移植。

`Ninja Gaiden Black Hurricane.exe` 读取你自己的 `default.xbe`，把原版 x86 游戏代码直接放在 PC 上运行；Xbox 内核、Direct3D 8、DirectSound
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
- **退出游戏**：主菜单最下面多了一项「退出游戏」，选中后用游戏自己的确认框问「要退出游戏吗？」，Ⓐ是退回桌面，Ⓑ否回到菜单。
- **联网排行榜（可选）**：用自己在 xemu 里注册过的 Insignia 主机与账号，在游戏的「排行榜」里登录并查看真实榜单
  （已实测）。默认关闭，打开方法见下文「联网」。
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

**需要**：Windows 10 或 11（64 位系统即可，程序本身是 32 位），支持 Direct3D 9 的显卡（集成显卡也可以），以及你自己的
vb-zh-x2.1 游戏镜像。不需要另外安装任何运行环境：用到的 Direct3D、XAudio2、XInput 都是 Windows 自带的，C++ 运行库已编进程序。

**用启动器（推荐）**：

1. 解压发布包。里面的 `NGB-Hurricane\` 文件夹放在哪里都可以：

   ```
   NGB-Hurricane\
     Ninja Gaiden Black Hurricane Launcher.exe   启动器：从这里进
     Ninja Gaiden Black Hurricane.exe            游戏程序，连同 blackrt.dll
     game\                                          游戏安装位置（启动器装进来）
     savedata\                                      Xbox 硬盘：存档与游戏设定（第一次运行时自动建立）
   ```

2. 打开启动器，在「安装」页选你的 x2.1 镜像（`.iso`），装到「游戏安装路径」（默认是启动器旁的 `game`）。装完会逐个核对游戏
   程序，也可以选择把镜像移到回收站。已经解包过的游戏，在同一页选那个文件夹即可。
   装好后直接双击 `Ninja Gaiden Black Hurricane.exe` 也能开始游戏（它会找启动器装好的游戏）。
3. 在「显示」「图形」「控制」页按需要调整（按「保存」才生效，每项旁的「?」有说明），回到「开始游戏」页开始。

「存档」页可以备份 / 还原存档、与 xRevive 互相导入导出；「关于」页有版本与法律声明。

**用 run.bat（开发用）**：仓库根目录的 `run.bat` 按下面的结构直接启动游戏，不经过启动器：

```
ngb-pc-native-port\
  run.bat
  build\RelWithDebInfo\Ninja Gaiden Black Hurricane.exe   构建产物，连同 blackrt.dll（见"构建"）
  game\x21\default.xbe                                       你自己解包的 x2.1 镜像（整个镜像的文件）
  savedata\                                                  Xbox 硬盘
  logs\                                                      日志 black.log、截图 shots\、命令文件 presses.txt
```

核对 `default.xbe` 的哈希与上表一致：`Get-FileHash game\x21\default.xbe -Algorithm SHA256`。找不到程序或游戏时，`run.bat`
的窗口里会说明缺了什么。

游戏里按 **Alt+Enter** 切换窗口 / 全屏，设定会保存。全屏分两种：独占全屏（`black.ini` 里 `fullscreen = 2`）与无边框
（`fullscreen = 1`），可在启动器的「显示」页选择。

**显卡**：默认用独立显卡，没有独显时用集成显卡。程序第一次运行时，若 Windows 的图形设置里还没有它的偏好，就替它登记为
"高性能"；之后想换，在启动器的「图形」页，或 Windows 设置 > 系统 > 屏幕 > 显示卡（图形）里改，程序不会覆盖你的选择。日志里 `host: Direct3D 9
device ... on '显卡名'` 一行记着实际用的是哪张卡。

**放在别的位置**：把 `default.xbe` 拖到 `run.bat` 上启动；或者在命令行里先设好路径再运行：

```bat
set EXE=D:\ngb\Ninja Gaiden Black Hurricane.exe
set GAME=D:\ngb\x21\default.xbe
set HDD=D:\ngb\savedata
set LOGDIR=D:\ngb\logs
run.bat
```

**存档**在 `savedata\Partition1\`：`UDATA\5443000d\` 是各存档格，`TDATA\5443000d\system.dat` 是系统存档。备份时复制整个
`savedata` 文件夹即可。

**截图**：往 `logs\presses.txt` 追加一行 `shot:名字`，下一帧存为 `logs\shots\名字.png`。更多命令见"诊断工具"。

**图形设定**（启动器「图形」页，或 `black.ini`）：`shadow` 阴影贴图倍数（1 = Xbox 原版 128×128，默认 4 = 512×512，最高
16）；`vsync = 1` 垂直同步（只在独占全屏下起作用，防撕裂；窗口和无边框全屏本来不撕裂；实测几乎不影响速度；默认关）；`perfhud = 1` 在画面右上角显示帧率、显卡、CPU、内存与磁盘使用率
（默认关）；`aniso` 各向异性纹理过滤（1 = 关，2–16）。命令行也可用 `--vsync`、`--perfhud`、`--aniso N`；命令行的值只用于
这一次，不会写回 `black.ini`。

**画面比例**：`widescreen = 1` 时 `aspect = 16:9`（默认）、`16:10` 或 `21:9`（实验性），命令行 `--aspect W:H`；`widescreen = 0`
为 4:3。21:9 时 3D 视野左右变宽（上下不变），16:10 时左右略窄；HUD、文字、菜单、标题画面与片头视频保持 16:9 比例（21:9
左右留黑边，16:10 上下留黑边）；21:9 下商店、游戏结束等画面尚未逐一检查。只对忍龙主程序生效，内置的 SFC / 街机程序保持 16:9。

**键盘与手柄**：游戏本身只认手柄；键盘的每个键直接按下手柄上的一个输入（默认 WASD 移动、方向键视角、J / K 攻击、空格跳、
Esc 为 Start……），在启动器的「控制」页可改，也可把手柄按钮重新映射。`black.ini` 里是 `kb_<输入> = 键[,键]` 与
`padmap_<输入> = <输入>`。按住 Alt 时按键不会送进游戏。

**着色器缓存**：程序第一次遇到某个着色器时要现场编译，会卡一下；编译结果存在程序旁的 `cache\shaders\`，下次启动时一次读进
内存，之后就不再卡。删掉这个文件夹没有坏处，只是会重新编译一遍。

`run.bat` 实际执行的命令（也可以自己手动输入）：

```
"Ninja Gaiden Black Hurricane.exe" --windowed --hdd savedata --log logs\black.log --screenshot logs\shots --shot-every 100000000 --press-file logs\presses.txt game\x21\default.xbe
```

- `--shot-every 100000000` 让定时截图实际上不发生，只在你用 `shot:` 要求时才截。默认值 60 是每秒截一张，会造成卡顿。
- `"Ninja Gaiden Black Hurricane.exe" --help` 列出全部选项。其中一部分（视野、自动瞄准、PC SETTINGS 菜单等）是 BLACK 专用的，对忍龙不起作用。

**联网（Insignia 排行榜，可选）**

移植版不自带账号：它借用你在 xemu 里已经注册好的那台「主机」。游戏自己的联网代码原样运行，和在 xemu 里登录一样。

1. **先在 xemu 里注册 Insignia**（只需一次）：按 Insignia 官网的说明，用 xemu 注册主机和账号，并确认在 xemu 里能登录
   Xbox Live。注册完成后，你的 EEPROM 文件（`eeprom.bin`）和硬盘镜像（`xbox_hdd.qcow2`）里就有了这台主机的身份和账号。
2. **关掉 xemu**：同一台主机不能同时登录两次。移植版发现 xemu 在运行时不会开联网。
3. **启动器「联网」页**：
   - 选「开」，然后用「选择文件…」选这两个文件。两个文件只读，不会被修改。
   - 下面的「主机」一行会显示检查结果：主机身份是否有效、是否注册过、有哪些账号。
   - 按「保存」。

   这一页写的是游戏程序旁边的 `black.ini`（`online = 1`、`online_eeprom = …`、`online_hdd = …`），也可以直接改这个文件。
4. **进游戏**：主菜单选「排行榜」，选自己的玩家代号即可登录，然后查看榜单。连上时画面右下角会提示「联网：已连接 Insignia」。

**要不要开 InsigniaDNS？** 默认不用。「联网」页的 DNS「查询方式」：

- **直接用 Insignia DNS**（默认，推荐）：游戏的所有 DNS 查询都由移植版转给 Insignia 的 DNS（`46.101.64.175`，可用 `online_dns = …`
  改），不管 xemu 里怎么设定的，也不用开 InsigniaDNS。只在移植版里生效，不改主机文件。
- **用 InsigniaDNS**：所有查询转给一台开着 InsigniaDNS.exe 的电脑，地址在「InsigniaDNS 地址」里填。点「找本机」会找这台
  电脑上正在运行的 InsigniaDNS 并填上它的地址。InsigniaDNS 只在局域网地址（`192.168.x.x`）上监听，填 `127.0.0.1` 不行。
  这个地址存在移植版的设置里，不看 xemu 里填的 DNS，所以换了电脑或网络只要在这里改。代理软件截走公网 DNS 时用这个。

「当前 DNS」一行显示游戏实际会用的 DNS：「自动 46.101.64.175（Insignia 的 DNS）」或「手动 192.168.x.x（InsigniaDNS）」，随上面的选择变化。
xemu 主机设定里填的 DNS 不用，也不再显示：移植版会把游戏的每个 DNS 查询都改发到这里显示的地址。

**连不上时：**

- 先按「联网」页的「检查网络」。它会用当前的查询方式问一次 `AS.XBOXLIVE.COM`：
  - 答出地址：DNS 正常；
  - 答出 `198.18.x.x`：代理软件（Clash 等）的 TUN / fake-ip 模式截走了 DNS，先关掉 TUN；
  - 没有回应：检查网络，或那台电脑上的 InsigniaDNS。
  
  局域网地址（`192.168.x.x`）上的 InsigniaDNS 一般不会被 TUN 截走，所以开着 TUN 时可以改用「用 InsigniaDNS」。
- 游戏里也会在右下角提示连不上的原因：DNS 被截走、DNS 没有回应、xemu 正在运行、文件无效等。
- 游戏的「无法连接 / 修复玩家代号」框里按 Ⓐ 现在和 Ⓑ 一样是取消。这些框的 Ⓐ 原本会去 Xbox 仪表板，移植版没有仪表板。
- 日志 `black.log` 里搜 `online:` 和 `[net]`，能看到身份是否读成功、DNS 问到了什么。

**联网是怎么做到的（通俗版）**

初代 Xbox 的官方联网服务 Xbox Live 早已关闭。Insignia 是玩家社区搭的替代服务器，专门扮成当年的 Xbox Live，让老游戏重新
能联网。移植版要连上它，办了四件事：

1. **联网功能用游戏原来的那一套。** 游戏里本来就有完整的联网代码（登录、排行榜、加密通信），移植版一行没重写，让它原样
   运行。对游戏来说，它就是在像当年一样连 Xbox Live。
2. **给它一块「假网卡」。** 真 Xbox 里有网卡，PC 上没有。移植版用软件模拟了一块 Xbox 网卡：游戏往网卡发的数据，移植版交给
   Windows 送上网；服务器的回信，再塞回这块网卡给游戏。就像游戏把信投进自家信箱，移植版当邮递员。
3. **借一台「已登记的主机」的身份。** Insignia 只认登记过的 Xbox。你在 xemu 里注册过的那台虚拟 Xbox，身份证（主机编号和
   密钥）在 EEPROM 文件里，玩家账号在硬盘镜像里。移植版只读这两个文件拿身份去登录，不改也不上传。所以同一台主机不能在
   xemu 和移植版里同时登录，就像一张身份证不能同时在两处刷。
4. **换一本「电话簿」（DNS）。** 联网第一步是查「Xbox Live 的服务器在哪」，这叫 DNS，相当于查电话簿。普通电话簿里 Xbox Live
   的号码早停机了；Insignia 的电话簿把这些名字指到 Insignia 自己的服务器。不管游戏去查哪本，移植版都换成 Insignia 的：
   「直接用 Insignia DNS」查 Insignia 官方那本；「用 InsigniaDNS」查你电脑上 InsigniaDNS.exe 那本，给代理软件截走查询时用。

结果：游戏以为自己连上了当年的 Xbox Live，Insignia 以为来的是一台真 Xbox（你注册的那台），两边对上，就能登录、看排行榜。

一句话：游戏原封不动，移植版给它造了一块网卡，借了你注册过的主机身份，再把查服务器地址的电话簿换成 Insignia 的。

细节与证据见 `project/docs/09_联网_Insignia.md`。

## 构建

本项目用 Visual Studio 2017（x86 C++ 工具）与 CMake 构建，产出 32 位程序：

```
cmake -S loader -B build -G "Visual Studio 15 2017" -A Win32
cmake --build build --config RelWithDebInfo
```

得到 `build\RelWithDebInfo\Ninja Gaiden Black Hurricane.exe` 与 `blackrt.dll`。`loader/CMakePresets.json` 里另有上游的 Visual Studio
2022 / 2026 预设。

**图标**：两个程序默认带 `launcher/art/icon.ico`（龙剑，见"法律声明"）。要换别的图标或不要图标，配置时用
`-DPORT_ICON=<.ico 路径>` 或 `-DPORT_ICON=`。

**发布包**：运行 `tools\make_release.ps1 -Bin <构建目录>\RelWithDebInfo`，得到 `dist\ngb-hurricane-pc-<版本>-win32.zip`
（启动器、游戏程序、`blackrt.dll`、中英文 README、LICENSE）与符号包。脚本会检查游戏程序带有图标。

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
| `launcher/` | 启动器（开发中，见 `project/docs/08_PC启动器计划.md`）：从镜像安装、选择游戏安装路径、启动游戏；`art/` 为界面背景图 |
| `notes/` | 上游 BLACK 的分析结果 |

反编译与反汇编输出、游戏文件、测试运行目录都只留在本地，不进仓库。

## 原理

`Ninja Gaiden Black Hurricane.exe` 是一个链接在 XBE 基址的小程序，抢在 Windows 往这段地址放别的东西之前占住它。`blackrt.dll` 把
`default.xbe` 映射进这段地址，提供游戏导入的 Xbox 内核函数，并在 Xbox 库（Direct3D 8、DirectSound、XAPI）的入口
打补丁，转到 PC 上的实现。

忍龙主程序是 LTCG 构建（寄存器传参、函数内联），库函数地址表是逐个核对的；附带程序是普通 D3D8，地址表由
XbSymbolDatabase 的识别结果生成。每一条模拟规则都要有依据：原版程序、xemu / 文档、硬件规格或实测对照。依据记在
`project/docs/07_证据盘点.md`。

## 法律声明

本项目与 Koei Tecmo、Team NINJA 及 Microsoft 无关，也未获其认可。Ninja Gaiden 是 Koei Tecmo 的商标，Xbox 是
Microsoft 的商标。仓库不含游戏代码或数据，也不含 Microsoft Xbox 开发包的代码。请使用自己合法取得的游戏。

例外是 `launcher/art/` 里的图：启动器的背景图取自作者自己的网站，其中隼龙的主视觉版权属于 Koei Tecmo；程序图标是游戏里
龙剑的贴图。两者都只作为本非商业粉丝移植的界面使用，出处见 `launcher/art/SOURCES.md`。

本项目是自由软件，以 GNU 通用公共许可证第 2 版或（由你选择）任何更新的版本发布，见 `LICENSE`。

## 致谢

- [black-pc](https://github.com/Spark198rus/black-pc)（Spark198rus）：本项目所基于的加载器。
- [ngbrecomp](https://github.com/gamboozino/ngbrecomp)：忍龙主程序的 D3D 符号表。
- [xemu](https://xemu.app) 与 [Cxbx-Reloaded](https://github.com/Cxbx-Reloaded/Cxbx-Reloaded)：Xbox 内核与 NV2A
  行为的参考（未复制代码）。
- [genma-pc](https://github.com/w1284603830/genma-pc)：同类的 Xbox 原生移植，CLIPPLANE 等行为的参照。
- [XbSymbolDatabase](https://github.com/Cxbx-Reloaded/XbSymbolDatabase)：识别游戏里的 Xbox 库函数。

开发：哈雅布撒（b 站），与 Claude Code 合作。
