# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 本目录是什么

《忍者龙剑传 黑之章 飓风版[中英] vb-zh-x2.1》PC 移植的 **路线 B：原生加载器 + HLE**（照 Spark198rus/black-pc 的做法，
32 位加载器把 x2.1 的 `default.xbe` 映射到原地址，原版 x86 代码直接在 CPU 上执行；内核、D3D8→D3D9、DirectSound→XAudio2、XAPI 输入由兼容层替换）。
路线 A（静态重编译，fork ngbrecomp，已发布 v0.9.13）在 `E:\games\emu\ninja gaiden black hurricane recomp\`，只读引用。

**先读**：`开发计划_路线B.md`（评估结论、分阶段计划与关口、风险、待决事项、决定记录）、`交接_路线A成果与路线B任务.md`（必读资料清单、路线 A 学到的事实、全部硬性约束）。
用户 2026-10-17 已批准计划；当前进度与决定见计划 §7，证据见 `docs/`。
现状（2026-11-10）：已远超计划阶段，发布到 2.2.15（`dist\`），支持黑之章四个镜像与 2004 版；工作分支 `native`（远端 `github`），
`release-2.1` 等是发布线，`backup/*` 是备份分支不要动。逐项现状与证据看 `docs/06_功能验证矩阵.md`、`docs/07_证据盘点.md`，
用户可见的功能与命令行开关看 `ngb-recomp-native/README.md` 和 `loader/README.md`。
`ngb-recomp-native/CLAUDE.md`、`NOTES.md` 是上游 black-pc（BLACK 原版）的，事实以本文件和 `docs/` 为准。

## 布局

```
ngb-recomp-native\   git 仓库：从 ...hurricane recomp\black-pc 本地 clone（起点 4c84944），所有移植代码在这里
  project\           本项目的文档与量测脚本（进仓库）；本文件、play.bat 在这里有副本，改了要同步
game\x21\            x2.1 解包文件的硬链接（指向路线 A input\x21，只读，不占空间）
build\               CMake 构建目录（纯英文路径）；当前用 build\ngb2（play.bat 跑它），其余是旧对照构建
run\<说明>\          每轮测试独立：hdd\（E/T/U/Z 分区）、日志、ini、shots\；run\play\ 归 play.bat
docs\  → ngb-recomp-native\project\docs   证据与结论，编号文件（00 输入清单 … 13）
tools\ → ngb-recomp-native\project\tools  量测脚本（xbe_*、disasm / xdis / callers、png_diff、frame_diff …）
work\                符号表（functions_x21.csv）、反汇编等中间物（不提交）
```

## 构建与运行

```
# 配置（只需一次；生成器 VS 15 2017 Win32，CMake 用路线 A 的便携版）
"E:\games\emu\ninja gaiden black hurricane recomp\toolchain\cmake-4.4.4-windows-x86_64\bin\cmake.exe" -S ngb-recomp-native\loader -B build\ngb2 -G "Visual Studio 15 2017" -A Win32
# 构建（输出 build\ngb2\RelWithDebInfo\：Ninja Gaiden Black Any.exe、blackrt.dll、… Launcher.exe）
"<同上 cmake.exe>" --build build\ngb2 --config RelWithDebInfo
# 试玩：用户存档的副本 run\play\hdd_user，日志 run\play\black.log，额外参数透传
play.bat [--audio ac3 ...]
# 发布包（对外操作，先问用户）
powershell -File ngb-recomp-native\tools\make_release.ps1 -Bin build\ngb2\RelWithDebInfo
```

- 版本号在 `loader/CMakeLists.txt` 的 `project(black_loader VERSION …)`；每次发版是一个只改版本号的提交（提交名就是版本号）。
- 没有单元测试；验证靠实际运行 + 证据：`--press-file <文件>` 启动后往文件追加命令即时执行（`shot:名字` 截帧、`trace[:N]` 记录绘制、
  `cfg:<开关>:0/1` 即时切换各项修正对照原样、`cfg:pixwatch:X,Y` 找改动某像素的绘制），`--press "20s:start,50s:a"` 按时间脚本输入，
  `--perf-at S:D` 记录性能。完整列表见 `ngb-recomp-native/README.md`「诊断工具」与 `loader/README.md`、`--help`。
- 构建日志按轮次放 `run\build_<说明>.log`。

## 代码架构（`ngb-recomp-native/loader/`）

- 产物：`black`（`stub/stub.c`，极小的 exe，在链接时预留 XBE 的地址范围）+ `blackrt.dll`（全部运行时）+ `launcher`（`../launcher/src/`，Win32 启动器：安装、存档重签、设置）。
- `src/main.cpp`、`image.cpp`、`game_load.cpp`：按 SHA-256 识别 XBE（`build_profile.h` 列出支持的构建），把各段映射回原地址，接内核导入后跳到入口；`XLaunchNewImage` 时以同参数重启载入附带程序。
- `src/hooks.cpp` + `*_functions.inc`：静态链接的 XDK 库（D3D8LTCG、DSOUND、XPP）按函数地址打跳转钩子换成 HLE。表由脚本生成，不要手改：
  `python -I ngb-recomp-native/tools/gen_ngb_tables.py work/functions_x21.csv ngb-recomp-native/loader/src/ngb`（附带程序用 `gen_aux_tables.py` → `src/auxtab/`）。
- `src/kernel/`：xboxkrnl HLE（`ktable.cpp` 导出表，`k_ngb.cpp` 忍龙专用）；`src/d3d/`：D3D8→D3D9，NV2A 顶点程序 / 寄存器合成器译为 HLSL（`nv2a_vsh`、`nv2a_psh`），`pushbuf.cpp`、`shader_cache.cpp`；
  `src/audio/`：软件 DirectSound 混音 → XAudio2；`src/input/`：XInput + SDL3；`src/net/`：Insignia 联网；`src/fe/`：游戏菜单里的 PC SETTINGS。
- 忍龙专用的补丁与地址：`src/ngb/`、`ngb_*.cpp`（宽屏、退出菜单、仪表盘）、`ng04_*.cpp`（2004 版）。各修正都有 `cfg:` 开关可切回原样，新修正照此做。
- 所有游戏相关数据只在运行时从用户的 XBE 读；仓库里只放地址、大小、符号名。

## 已确认的关键事实（细节与量测命令见计划 §1、docs/）

- x2.1 `default.xbe`：SHA-256 `aa3c5bec…`，基址 0x10000，size_of_image 0xE07000，入口 0x32B385，145 个内核导入，17 段；HANZI 段 0xDC7000（标志 0x7，可执行）。
- 全部静态库 XDK 1.0.5849，与 BLACK 相同（D3D8LTCG 同为 qfe 15）；原版与 x2.1 的 13 个库段字节完全相同，库函数表可共用。
- 145 个内核导入中 36 个 black-pc 没有；fs:[KPCR] 访问 41 处，位移种类与 black-pc 已处理的相同；XBE 头后到 `.text` 只有 0x13C 字节空隙（PE 头副本要另放）。
- 黑之章用 94 个 D3D8LTCG 符号（ngbrecomp `tools/d3d_symbols.txt`），black-pc 已实现 37 个；没有游戏代码直接写推送缓冲。
- 运行时执行到的特权指令：`cli` 0x3DFF03、`sti` 0x3DFF14（XPP 段）、`wbinvd` 0x3CDA95（D3D 段）。

## 硬性约束（沿用，详见交接文档 §3）

- **回复用户用繁体中文**；项目文档用简体。
- 所有产生的文件只放本目录；C 盘不下载、不安装、不写入；构建输出放纯英文路径。
- 每一步附证据（偏移、哈希、日志、截图），弄懂再往下走，拿不准就问；结论写明"已证实"与"不能由此推出"。
- 对外操作先问：下载（说明来源与大小）、提交推送、发布、开 issue。合并上游有冲突先逐个说明。
- 不删备份 / 镜像 / 用户文件；原版镜像、路线 A 目录、汉化工程目录只读。E 盘空间很紧（2026-10-17 约 6 GB，2026-11-10 约 52 GB），大文件操作前看剩余空间。
  run 目录：`bench_hdd`（测试存档母本）、`play`（play.bat）、`user_online` 长期保留；自动测试各用一份存档拷贝（约 644 MB），跑完查完就删掉自己的拷贝，
  日志 / 截图 / trace 等证据可留；整批删旧 run 目录先列清单给用户勾选（2026-11-10 用户决定，取代旧的"一次只留一个"）。
- 截图只截游戏窗口；只关自己开的测试进程。
- git：白名单 `.gitignore`，`.gitattributes` 设 `* -text`；绝不提交游戏数据、镜像、解包文件、第三方字体或库。fork 许可 GPL-2.0-or-later；可引用 ngbrecomp 的 MIT 部分（保留声明），不抄 Cxbx / xemu / xboxrecomp 的 GPL 代码。
  - 例外（用户 2026-10-22 决定）：`launcher/art/` 里的启动器背景图（取自用户自己的网站，隼龙主视觉版权属光荣特库摩）与程序图标 `icon.ico`（游戏里龙剑的贴图，用户同日决定「仍用龙剑图标」）进仓库与发布包，出处记在 `launcher/art/SOURCES.md`，README 法律声明已注明。不得据此扩大到其他游戏素材。
- 发布物写作者 "b站 哈雅布撒"、QQ 2682557218。GitHub 只经 `GH_CONFIG_DIR=E:\games\emu\ninja gaiden black local\tools\gh\config` 使用 gh。
- 禁止阅读 `D:\NGB-hanhua`、`E:\games\emu\games\hanhua-project\`。

## 工具与环境

- 编译：VS2017 Community（D 盘，MSVC 14.16 x86）+ 便携 CMake（路线 A `toolchain\cmake-4.4.4-windows-x86_64`）；备选 `toolchain\llvm-mingw-20260922` 的 clang-cl。
- Python：路线 A `toolchain\venv`（含 capstone）；量测脚本一律 `python -I` 运行。
- 参考源码（只读）：`…hurricane recomp\black-pc\`、`ngb-hurricane\`（路线 A 分支，含 xboxrecomp 内核与 ngbrecomp 的 D3D 表）、`ngbrecomp\`、`xboxrecomp\`。
- 对照平台：xemu `E:\games\emu\xemu\xemu-win-release\xemu.exe`；路线 A 的 PC 版。

## 相关 skill

- `game-localization`、`ninja-gaiden-port`：黑之章各功能的现象 → 根因 → 修法 → 取证，验证矩阵阶段要读。
