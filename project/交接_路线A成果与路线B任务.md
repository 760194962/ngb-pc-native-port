# 交接：路线 A（静态重编译）的成果、规范与资料 → 路线 B（原生加载器）计划任务

2026-10-17，由 `E:\games\emu\ninja gaiden black hurricane recomp`（路线 A）会话整理，交给本目录（`ninja gaiden black Hurricane recomp2`）的会话。
本文只是索引和要点；细节都在下面列出的原始文件里，**以原始文件为准，只读引用，不要在那些目录写入**。

---

## 0. 你的任务（用户 2026-10-17 指示）

学习 **black-pc（BLACK 的 PC 移植）与《幻魔鬼武者》PC 移植** 的做法，为《忍者龙剑传 黑之章 飓风版[中英] vb-zh-x2.1》
制定**另一个方向**的 PC 移植计划，即“路线 B”。

- **路线 A（已完成、已发布）**：fork ngbrecomp，用 xboxrecomp 把 x2.1 的 `default.xbe` 静态翻译成 C 再编译。
- **路线 B（你的任务）**：像 black-pc 那样，用 32 位加载器把**原版 x86 代码直接在 CPU 上执行**。
  内核、D3D8、DirectSound、XAPI、输入都由兼容层替换（HLE，类似 Cxbx），之后可按优先级把子系统逐步换成反编译出来的 C/C++。
- **用户的决定**（2026-10-16，见路线 A `开发计划.md` §1.4、§5）：两种方式都做，先 A 后 B。现在 A 已发布到 v0.9.13，开始规划 B。

**先交计划，不写代码**：
- 照路线 A `开发计划.md` 的格式写一份分阶段计划，每个阶段要有可验证的终点（关口）。
- 计划交用户审阅批准后再动手。
- 本目录要做什么、目录怎么安排，开工前先跟用户确认（本目录 `CLAUDE.md` 也这样要求）。

---

## 1. 必读资料（路径都在本机，只读）

### 1.1 路线 B 的直接参考

| 资料 | 路径 | 要点 |
|---|---|---|
| **black-pc 源码**（上游原样副本，提交 `4c84944`，Spark198rus/black-pc） | `E:\games\emu\ninja gaiden black hurricane recomp\black-pc\` | 先读 `README.md`、`NOTES.md`、`CLAUDE.md`、`PC_SETUP.md`、`TODO.md`、`loader/README.md`、`docs/`、`notes/`、`ghidra_scripts/`。`black.exe`（32 位加载器）+ `blackrt.dll`（运行时）。HLE 内核 + D3D8→D3D9 + DSound→XAudio2 + XInput，支持高分辨率、宽屏、60 fps、鼠标、游戏菜单里的 PC 设置 |
| **幻魔鬼武者 PC 移植与 Bug 排查全记录** | `E:\games\emu\ninja gaiden black hurricane recomp\幻魔鬼武者_PC移植與Bug排查全紀錄_分享版_20261016.txt`（58 KB） | 另一位开发者用上面这类方法做成幻魔的经验。方法论（分阶段里程碑、固定上游版本、差异比对、独立存档、先测量再优化、验收四问、“诊断可绕过，修正须保留原语义”）已写进路线 A 计划 §4 |
| 用户给路线 A 的原始任务说明 | `E:\games\emu\ninja gaiden black hurricane recomp\任务.md` | 用户对 black-pc、幻魔记录的描述，以及“每一步 100% 有证据”的要求 |
| 路线 A 计划里对 black-pc 的评估 | `E:\games\emu\ninja gaiden black hurricane recomp\开发计划.md` §1.3、§1.4 | 结论：x2.1 补丁在路线 B 下原样生效（含 HANZI 段新代码），但 D3D/音频要按黑之章的 XDK 版本重做（幻魔 D3D 3948、黑之章 5849、BLACK 又不同），黑之章渲染比两者都复杂 |
| xRevive 实验（另一种“XBE 当数据装入 + 机器码翻译 + 内核/D3D 替代层”的移植器） | `E:\games\emu\ninja gaiden black local\docs\37_xRevive兼容性实验.md` | 同类方案对 x2.1 的实测：哪些补丁生效、哪些不生效、为什么 |

### 1.2 路线 A 的成果（路线 B 可以借鉴或对照）

| 资料 | 路径 / 地址 |
|---|---|
| 路线 A 计划与决定记录 | `E:\games\emu\ninja gaiden black hurricane recomp\开发计划.md` |
| 路线 A 文档 | 同目录 `docs\00_输入清单.md`（上游提交号与哈希）、`01_工具链.md`（本机工具链：llvm-mingw、CMake、Ninja、MSYS2 包，全部便携在 `toolchain\`）、`02_原版基线.md`、`03_冲突清单.md`（x2.1 改动与 ngbrecomp 手写函数的冲突）、`04_合并记录.md`（全部合并、修正、发布、调查记录，**最重要**） |
| fork 源码（路线 A 成品） | `E:\games\emu\ninja gaiden black hurricane recomp\ngb-hurricane\`（分支 `hurricane`）；GitHub `akirahayabusa/ngbrecomp-hurricane-zh-x2.1`（公开），最新 `hurricane-v0.9.13` |
| ngbrecomp 内的 Xbox 运行时 | `ngb-hurricane\xboxrecomp\src\kernel\`（内核 HLE：文件、路径、线程、同步、内存，可作内核函数行为参考）、`src\nv2a_vk\`（xemu 的 NV2A Vulkan 渲染器）、`src\apu\`（音频）、`src\platform\vcpu.c`（虚拟单 CPU 调度） |
| 上游原样副本 | 同目录 `ngbrecomp\`、`xboxrecomp\`（只读） |
| 玩家网站 | https://akirahayabusa.github.io/ngb-hurricane-patch/ （源码 `E:\games\emu\ninja gaiden black local\work\gh_pages\`，含 PC 版一节） |

### 1.3 黑之章本身的知识（汉化工程，只读）

- `E:\games\emu\ninja gaiden black local\CLAUDE.md`：已确认的引擎事实与用户的全部决定。
- `E:\games\emu\ninja gaiden black local\docs\01`–`37`：每个功能的取证过程。x2.1 的代码改动在 XBE 的 HANZI 段（VA 0xDC7000 起），新代码全部经直接 call/jmp 进入（docs/37 已证实）。
- 本目录 `CLAUDE.md` 已经列出这些资料。

---

## 2. 路线 A 里学到的、路线 B 用得上的事实

1. **游戏与版本**：美版 NTSC-U，标题 ID `5443000D`，XDK 5849。x2.1 成品镜像 SHA-1 `08e3cbe7…`，`default.xbe` SHA-256 `aa3c5bec…`（完整值见路线 A `docs/00_输入清单.md`）。原版镜像 SHA-1 `d35859d1…`。
2. **x2.1 的补丁洞穴依赖调用方的 ebp**：部分洞穴从不建帧（FPO）的函数调用，读调用方的 ebp：
   - `0xDC8380`：Game Over 状态；
   - `0xDC84A0`：还魂护符列表标记；
   - `0xDC8720`：控制器设置交差之法行。

   路线 A 的静态翻译里 ebp 是函数内局部变量，所以要专门处理（翻译器加了 `--ebp-to-callee`）。路线 B 原生执行就没有这个问题，这是 B 的优势之一。
3. **SFC 三部曲、街机版是另外的 XBE**：`XLaunchNewImage` 换程序。路线 A 做不到，主菜单已隐藏这两项。路线 B 如果加载器支持换 XBE，理论上可以做，是可评估的加分项。
4. **文字与字库**：x2.1 的中文走游戏自己的 `jpfont.map`、`fjp_*.spr` 字形页（B1 页 256×2048，2048 格已用完）。“亚洲”语言即中文，语言字节 `[0xBCE030]`。路线 B 原生执行，原样生效。PC 自己新增的菜单要另画中文：路线 A 用思源黑体 CN Bold 子集，更名为 NGB Hanzi（OFL）；鼎猎宋刻体授权不允许嵌入程序，只能用预先画好的图片。
5. **存档**：Xbox 存档可以直接用（`UDATA\5443000d\<存档>\save000.dat` + `SaveMeta.xbx`、`TDATA\5443000d\system.dat`）。路线 A 实测可读；路径映射见 `xboxrecomp\src\kernel\kernel_path.c`。
6. **路线 A 修过的坑**（三项都已被原作者 0.9.13 采纳，issue #40–#42；同类问题在 B 里也要留意）：
   - **Windows 路径编码**：窄字符 API 拿到的是系统代码页（中文系统是 GBK），路径含中文就出错。修法是 manifest 加 `activeCodePage UTF-8`（#41）。
   - **线程调度与渲染锁**：客体线程拿着渲染锁时被调度器挂起，就会死锁（0 FPS）（#40）。
   - **显存**：系统内存不足时，Vulkan 大块分配失败，xemu 渲染器直接崩溃。
   - **安装与重启**：返回主菜单时整个程序重启，沿用原命令行会误装第二次。
   - **磁盘写满**：游戏写设置文件被截成 0 字节，会提示“无法读取游戏设置”（非 bug，用户已复现）。
7. **其他上游已知问题**：“画面设置 → 画面调整”卡顿，ngbrecomp 原版同样如此。

---

## 3. 规范与用户的硬性约束（全部沿用）

- **语言**：回复用户用**繁体中文**（用户 2026-10-16 起的要求，优先于本目录 `CLAUDE.md` 里“简体”的旧写法）；项目文档沿用简体。
- **文件位置**：所有产生的文件只放本目录；C 盘不下载、不安装、不写入（工具便携放 `tools/`、`toolchain/`）。构建输出放纯英文路径（幻魔记录：中文路径导致构建崩溃）。
- **证据**：每一步要有证据（偏移、十六进制、哈希、日志、截图），弄懂再往下走，拿不准就停下来问。结论写明“已证实”和“不能由此推出”的内容，“没报错”不等于“正确”。
- **文档**：关键发现随时写进 `docs/` 编号文件；`CLAUDE.md` 只放已确认事实的摘要。
- **git**：白名单 `.gitignore`（只提交工具、文档、源码），`.gitattributes` 设 `* -text`。绝不提交游戏数据、镜像、解包文件、第三方字体或库、模拟器环境。
- **对外操作先问**：下载（说明来源与大小）、提交推送、发布、开 issue，都要先问用户。
- **合并上游时有冲突**：先逐个说明，等用户同意再解决和提交。
- **删除**：不删备份、镜像、用户文件，要删先单独问。覆盖存档或硬盘前先备份。
- **磁盘**：E 盘空间很紧（2026-10-17 一度写满），大文件操作前看剩余空间；大量读写的任务一次只跑一个。
- **截图**：只截游戏窗口本身（按进程路径用 PrintWindow），不截用户的屏幕。
- **进程**：只关自己开的测试进程，关之前先确认不是用户正在玩的那一个。
- **发布物**：写作者“b站 哈雅布撒”、QQ 2682557218。PC 版标题格式 `Ninja Gaiden Black Hurricane_x2.1_v<版本>`（这是路线 A 的规则；路线 B 的命名届时问用户）。
- **GitHub**：只通过 `GH_CONFIG_DIR=E:\games\emu\ninja gaiden black local\tools\gh\config` 使用 gh。不要替用户输入密码或令牌。
- **禁止**：不读 `D:\NGB-hanhua`、`E:\games\emu\games\hanhua-project\`（已报废项目）。原版镜像只读。

---

## 4. 建议的计划要回答的问题（供参考，不是结论）

1. **black-pc 的架构**：加载器、内核 HLE、D3D8→D3D9、DSound、XInput 各怎么实现？哪些是游戏专用的，哪些可以复用到黑之章？
2. **黑之章的 XDK 5849 库怎么识别**：D3D8、DSound、XAPI 这些库函数在 x2.1 XBE 里的位置（签名匹配）。路线 A 的 xboxrecomp 有库函数识别，ngbrecomp 的 `src/d3d/` 有一份 D3D 函数表（`d3d_funcs`），都可以对照。
3. **渲染后端**：选 D3D9 还是 Vulkan？黑之章用到推送缓冲（push buffer）、顶点着色器等 NV2A 特性，要逐项评估。路线 A 有两个渲染器可对照：ngbrecomp 自己的 Vulkan 渲染器 `src/d3d/vk/`，以及 xemu 的 NV2A 渲染器。
4. **第一个关口**：怎样最小地证明“原生执行 x2.1 代码”可行，比如跑到标题画面，或进到第一个场景？
5. **HANZI 段**：路线 B 的加载器要把非标准段映射成可执行。黑之章的防改校验 `0x164E50` 会不会受影响？
6. **与路线 A 的分工**：哪些问题 B 能解决而 A 不能（例如 SFC 三部曲），值得作为 B 的卖点。

---

路线 A 会话：`ninja-gaiden-black-local-0a`（在 `E:\games\emu\ninja gaiden black local` 启动）。有路线 A 的问题可以发消息问，它会回复。
