# 忍龙黑之章 飓风版 x2.1 PC 移植 路线 B（原生加载器 + HLE）：评估结论与开发计划

2026-10-17 初版（待用户审阅；**批准前不写移植代码**）。
目标：`NINJA GAIDEN BLACK[飓风版][中英]哈雅布撒 vb-zh-x2.1[…].iso`（SHA-1 `08e3cbe7ad3bc2cddea1969b5159a5b7f409b8b8`），
`default.xbe` SHA-256 `aa3c5bec2923f83ccb37fe6a63ea5516b26d477b53c679a1620d19e4a8e4e8f6`（4,583,424 字节）。

路线 B 的做法：照 black-pc（Spark198rus，提交 `4c84944`）的方式，用 32 位加载器把 x2.1 的 `default.xbe` 映射到它原来的地址，
**原版 x86 代码直接在 CPU 上执行**；Xbox 内核、D3D8、DirectSound、XAPI（输入）换成兼容层（HLE）。x2.1 的 66 处 `.text` 补丁与 HANZI 段
新代码原样执行。路线 A（静态重编译，fork ngbrecomp，已发布 v0.9.13）继续作为对照与资料来源。

原则（沿用路线 A `开发计划.md` 与幻魔记录）：**每一步要有可复查的证据（哈希、日志、截图、对照表）才进入下一步**；每个阶段末尾设"关口"，
关口没过不往下走；结论写明"已证实"和"不能由此推出"。本文第 1 节的每条事实都注明了来源文件或量测命令。

---

## 1. 评估（2026-10-17 静态阅读与量测，全部只读）

### 1.1 black-pc 的架构（`…hurricane recomp\black-pc\`，README / NOTES.md / loader 源码）

| 部件 | 做法 | 黑之章可否沿用 |
|---|---|---|
| **地址空间** | `black.exe` 是无 CRT 的桩程序，链接在 `/BASE:0x10000 /FIXED`，带 0x7F0000 字节 BSS，让自己的映像先占住 XBE 地址段；`blackrt.dll` 把 XBE 覆盖映射上去（`loader/stub/stub.c`、`src/image.cpp`） | 可沿用；黑之章映像 0x10000–0xE17000（size_of_image 0xE07000），**保留区要扩到 ≥ 0xE07000** |
| **PE 头拼接** | Windows 启动后仍读主模块 PE 头，于是在 XBE 头之后、第一段之前的空隙放一份 NT 头副本，`e_lfanew` 写在 RSA 签名区（NOTES #9） | **要改**：BLACK 的空隙 0x5C0 字节；黑之章头长 0xEC4，到 `.text`（0x11000）只剩 **0x13C 字节**，NT 头副本（0xF8 + 每段 0x28）放不下，要另找位置（见阶段 3） |
| **内核 HLE** | 115 个导入全部实现（`src/kernel/`：内存、线程、同步、DPC/定时器、文件、Rtl、杂项）；连续内存 = 固定在 0x80000000 的 64 MB 块，物理地址 = 虚拟 − 0x80000000；线程 = 真 Windows 线程，固定到一个物理核；IRQL ≥ DISPATCH = 一把全局锁 | 可沿用；黑之章 145 个导入，**36 个 BLACK 没有**（见 1.3），要新写 |
| **fs: 段改写** | 加载时扫描代码里的 `fs:[0x04/0x20/0x24/0x28/0x58]`（Xbox KPCR 字段），把位移改成 Windows TLS 槽（`src/xthread.cpp`，BLACK 34 处） | 可沿用；黑之章 x2.1 代码段量得 **41 处，位移种类完全相同**（见 1.3） |
| **库函数替换** | D3D / DSOUND / XPP 三个库段里的**每个函数**要么有 HLE 实现、要么首次调用即停机并记录（`src/d3d/dispatch.cpp`）；LTCG 寄存器传参用生成的裸 thunk（参数串如 `"ecx edx s"`）；函数表 `*_functions.inc` 由 `notes/functions.csv`（Ghidra + XbSymbolDatabase）生成 | 机制可沿用；**表要为黑之章重做**（地址不同） |
| **D3D → Direct3D 9** | 73 个 HLE 入口（device 51 + resources 18 + draw 3 + 2 原生）；NV2A 顶点程序 → HLSL vs_2_x（`nv2a_vsh.cpp`）、寄存器合成器 → ps_2_x（`nv2a_psh.cpp`）；游戏直接写库内部数据（渲染状态影子数组、脏标志），所以绘制时从 Xbox 侧数组读状态；渲染目标按内存地址建宿主纹理，深度缓冲用 INTZ，内存别名（把深度当颜色读、把颜色当深度绑）有模型；伽马斜坡、任意渲染分辨率、宽屏、FXAA、接缝填补 | 机制可沿用；黑之章用到的 94 个 D3D 符号里 **37 个已有、57 个没有**（见 1.4），且黑之章用固定管线蒙皮、阴影贴图等 BLACK 不用的功能 |
| **DirectSound → XAudio2** | 50 个 DSOUND 钩子，软件混音器（缓冲、流、3D 距离/声像、混音仓折叠到立体声、FDN 混响） | 机制可沿用；黑之章 DSOUND 版本与 BLACK **相同**（5849 qfe 11），地址不同 |
| **输入** | XPP 段 6 个 XInput 钩子（XInitDevices、XGetDeviceChanges、XInputOpen/Close/GetState/SetState）→ PC XInput + 键鼠 | 可沿用，地址重做；幻魔记录的 XGetDevices/插入通知生命周期要注意 |
| **BLACK 专用部分** | `timing.cpp`（rdtsc 733 MHz）、60 fps 补丁、鼠标视角、自动瞄准、FOV、`src/fe/`（EA Apt 菜单改造）、`main.cpp` 里按 SHA-256 的 `known_build` 门控 | **全部不适用**，去掉或换成黑之章自己的 |

- 规模：loader 共 18,668 行 C/C++（`wc -l`），**要求 MSVC 或 clang-cl、C++20、CMake ≥ 3.24**（`CMakeLists.txt` 明确拒绝非 MSVC，8 个文件用 MSVC `__asm`）。
- 许可：GPL-2.0-or-later；仓库里**不含任何游戏字节**（`notes/functions.csv` 只有地址、大小、名字）；Cxbx / xemu 只作参考、不抄代码。
- black-pc 的"第一关口"写法（`docs/ROADMAP.md` 末尾清单）：XBE 解析 → 预留地址 → 映射段 → 每个内核序号先做记录桩 → 跳入口 → 停在第一个 D3D 调用。BLACK 用了约 50 ms 到达，幻魔约 17 ms。

### 1.2 幻魔鬼武者记录给路线 B 的具体教训（`幻魔鬼武者_PC移植與Bug排查全紀錄_分享版_20261016.txt`）

路线 A 计划 §4 已吸收其方法论（阶段里程碑、固定上游、差异比对、独立存档、先测量再优化、验收四问、诊断与修正分离）。对路线 B 另有直接可用的技术点：

1. **XDK 版本决定一切**（§4.1）：幻魔 D3D 3948 与 BLACK 的 5849 贴图状态顺序、渲染状态编号、设备对象偏移都不同，不能只改地址。
   **黑之章与 BLACK 同为 1.0.5849，且 D3D8LTCG 同为 qfe 15**（见 1.3），这是黑之章比幻魔有利得多的一点，但偏移仍要逐项核对，不能默认相同。
2. **四个里程碑**（§3）：① 载入 + 内核初始化通过、停在第一个 D3D 调用 → ② 标志与标题画面 → ③ 新游戏、角色显示并能移动 → ④ 改造成可用 PC 版。本计划的阶段 3–5 照此划分。
3. **特权指令**（§4.5）：WBINVD 在用户态必崩，只替换**已核对原字节**的那一处，不扫全盘乱删。黑之章运行时实际执行到的 `cli` 0x3DFF03、`sti` 0x3DFF14（XPP 段）、`wbinvd` 0x3CDA95（D3D 段）
   已由路线 A 的 r003 日志记录（次数 125 / 120 / 5），三处都在被替换的库段里，由替换其所在函数解决（阶段 3 核对）。
4. **手柄插入通知**（§4.5）：XGetDevices 提前消耗插入通知会让游戏一直要求接手柄。
5. **每帧等两次垂直同步**（§05）：游戏自己调 BlockUntilVerticalBlank 再 Swap，black-pc 的 pace_swap 又等一次 → 半速。黑之章也调用 `D3DDevice_BlockUntilVerticalBlank`（`d3d_funcs.h` 第 51 项），阶段 4 要按幻魔的修法处理并用"游戏帧数 : 垂直同步数"量化。
6. **贴图缓存失效**（§07、§09）：原位改写的贴图不能只看地址/头；P8 图集配不同调色板要多版本缓存。黑之章的字库是 P8/A8 的 XPR 页，HUD 大量 P8，同样适用。
7. **NULL 索引指针、XYZRHW 线性贴图坐标、CopyRects 背景深度**（§4.2–4.4）：黑之章用 `CopyRects`（第 43 项）与 YUV（`SetRenderState_YuvEnable`，影片）。
8. **工程**：中文路径曾让 CMake Win32 构建以 0xC0000409 退出 → 构建输出放纯英文路径；测试用独立虚拟 HDD；正式版与审计版分开构建。

### 1.3 黑之章 x2.1 的事实（今日量测；命令与脚本见本目录 `tools/`）

**XBE 结构**（`python -I black-pc/tools/xbeinfo.py input/x21/default.xbe`）：

- 标题 `Ninja Gaiden: Black`，ID `5443000D`，zero-fill TLS 0xA8，`_tls_index` 在 0x45E84C；基址 0x10000，**size_of_image 0xE07000**（原版 0xDB6BC0），入口 0x32B385（与原版相同），内核 thunk 表 0x3E1420，**145 个内核导入**。
- 17 个段。库段：`D3DX`、`DSOUND`（0x37A6C0）、`XONLINE`、`XNET`、`PSFD00`、`XGRPH`、`D3D`（0x3C5260–0x3D95E0）、`XPP`（0x3D95E0–0x3E1414）、`DOLBY`、`XON_RD`；
  `.data` vsize 0x9A433C（≈ 9.6 MB BSS）；**`HANZI` 0xDC7000，vsize 0x50000，raw 0x3000，标志 0x7（可写 + 预载 + 可执行）**。
- 静态链接库 **全部 1.0.5849**：XAPILIB qfe 1、D3DX8、DSOUND qfe 11、XONLINES qfe 17、XBOXKRNL、LIBCMT、LIBCPMT、**D3D8LTCG qfe 15**、XGRAPHCL qfe 9。
  BLACK（`black-pc/notes/phase1-xbe-report.txt`）：XAPILIB qfe 1、DSOUND qfe 11、D3D8LTCG qfe 15、XGRAPHCL qfe 9 —— **四个共有库的版本与 qfe 完全相同**。
  不能由此推出：两份库段字节相同（链接地址不同，内联/裁剪可能不同），设备对象偏移是否相同要在阶段 2 实测。

**原版与 x2.1 逐段比对**（`tools/xbe_sections_diff.py`，SHA-256 原始字节）：

- `.text`、`.rdata`、`.data` 不同（x2.1 的补丁），`HANZI` 为 x2.1 新增；**其余 13 段（含 D3DX、DSOUND、XONLINE、XNET、PSFD00、XGRPH、D3D、XPP、DOLBY、XON_RD）字节与 VA 完全相同**。
  → 用原版做的库函数表、XbSymbolDatabase 匹配结果，对 x2.1 直接有效；ngbrecomp 的 `tools/d3d_symbols.txt`（94 个 D3D8LTCG 符号及地址）可直接作为起点。

**内核导入对照 BLACK**（`tools/compare_with_blackpc.py`）：145 vs 115，共有 109；**黑之章独有 36 个**（black-pc 没有实现，要新写）：

| 类别 | 导入 | 备注 |
|---|---|---|
| 纤程 / 线程 | `MmCreateKernelStack`、`MmDeleteKernelStack`、`KeSetDisableBoostThread`、`KeWaitForMultipleObjects`、`KeSetTimerEx`、`KeTickCount` | XAPI 纤程（`SwitchToFiber` 0x32A293，ngbrecomp `src/fibers.c` 已分析）要用内核栈 |
| 计时 | `KeQueryPerformanceCounter`、`KeQueryPerformanceFrequency`、`NtSetSystemTime` | 与 BLACK 不同：BLACK 的 QPC 是裸 rdtsc；黑之章走内核调用，black-pc 的 `timing.cpp` 不适用，黑之章自己的 rdtsc 用法要另查 |
| I/O 管理器 | `IoCreateDevice`、`IofCallDriver`、`IoBuildSynchronousFsdRequest`、`IoSynchronousFsdRequest`、`IoSynchronousDeviceIoControlRequest`、`IoStartPacket`、`IoStartNextPacket`、`IoInvalidDeviceRequest`、`IoMarkIrpMustComplete` | 阶段 2 查调用者（推测 XNET / XONLINE 或光盘设备），xboxrecomp `kernel_io.c` 有实现可作行为参考 |
| 网络 | `PhyInitialize`、`PhyGetLinkState`、`XboxLANKey` | 主菜单 Ranking（Xbox Live 排行榜）；ngbrecomp 报告"链路已连接"并把 Ranking 换成设置 |
| 加密 | `XcRC4Key`、`XcRC4Crypt`、`XcVerifyPKCS1Signature`、`XcModExp`、`XcDESKeyParity`、`XcKeyTable`、`XcBlockCryptCBC`、`XePublicKeyData` | Xbox Live 认证 / 存档签名；x2.1 的 `savesig_patch`（任意主机存档）已改动签名检查，阶段 2 查哪些仍被调用 |
| 其他 | `MmQueryStatistics`、`NtDeleteFile`、`ObReferenceObjectByName`、`DbgBreakPoint`、`HalDiskModelNumber`、`HalDiskSerialNumber`、`HalIsResetOrShutdownPending` | |

BLACK 独有 6 个（`NtCreateSemaphore` 等）黑之章不用。xboxrecomp（路线 A 的 `ngb-hurricane\xboxrecomp\src\kernel\`）对这 36 个都有实现，可作行为参考（GPL/MIT 许可见 §3）。

**fs: 段访问**（`tools/xbe_fs_scan.py`，capstone 线性扫描 x2.1 可执行段）：
`fs:[0]` 76 处（SEH 链，Windows 相同，不改）；**`fs:[0x4]` 9、`fs:[0x20]` 14、`fs:[0x24]` 7、`fs:[0x28]` 9、`fs:[0x58]` 2，共 41 处**，分布在 `.text`（XAPI 代码区 0x328xxx–0x370xxx）、DSOUND、XONLINE、XNET、D3D；
没有 `fs:[0x8]`；`fs:[0x4]` 全部是读（含纤程切换 0x32A299 / 0x32A2D7），不会改写 TLS 数组槽。五种位移与 black-pc 已处理的一模一样。
`.rdata`/`.data`/HANZI 里的命中是数据误判，不计。不能由此推出：没有经寄存器间接访问 KPCR 的地方（`fs:[reg]` 在代码段共 33 处，阶段 2 逐处看）。

**内存与线程**（路线 A `docs/04_合并记录.md` C3、xboxrecomp `platform/vcpu.h`、ngbrecomp `src/fibers.c`）：

- 游戏第一次连续内存分配就是主堆：x2.1 为 38,674,432 字节（0x24E2000，比原版多 0x70000），之后 DirectSound 49,152 字节块等 → black-pc 的"64 MB 固定在 0x80000000"模型装得下。
- 黑之章依赖 Xbox 单 CPU 优先级调度（xboxrecomp 注释：CRI 中间件唤醒服务线程后直接读结果）；路线 A 曾因调度器与渲染锁死锁（issue #40）。
  black-pc 用"真线程 + 固定单核 + Windows 线程优先级 + IRQL 全局锁"，与 Xbox 行为接近但不等价，列为主要风险（§3）。
- XAPI 纤程：`SwitchToFiber` 直接换 esp；原生执行下这是普通 x86 行为，不需要路线 A 那种宿主栈切换，但 `MmCreateKernelStack` 要真分配栈内存。
- 影片：CRI Sofdec 软解（`mwPlyGetStat` 0x344BE0，`.sfd`），音频 CRI ADX 走 DirectSound 流 → 不需要 BLACK 的 XMV/WMA 那类解码器，但 DSound 流接口必须完整。

**飓风 / 汉化补丁**（`…black local\work\release\xrevive_submit\addon_spec_en.md`，与成品 XBE 逐字节核对过）：

- `.text` 66 处（360 字节）+ HANZI 32 段新代码（3838 字节）、34 处进入点（32 call + 2 jmp），**全部直接 call/jmp**，无间接跳转指向新代码、无自改代码；
  HANZI 其余是 BSS：译文缓冲区 0xDD7000（0x30000）、字库码表 0xE07000（0x10000），映射为零页即可。
- 路线 A 在静态翻译里必须专门处理的四类问题——`pushad/popad/pushfd/popfd`（20 段）、调用后标志位回传（8 处）、从新代码跳回原函数中间（0x212EFF）、
  洞穴读调用方 ebp（0xDC8380 / 0xDC84A0 / 0xDC8720）——在原生执行下**都是普通 x86 语义，不需要任何处理**。这是路线 B 最直接的优势，阶段 4 要用运行时探针证明每个入口确实被执行到。
- 防改校验 `0x164E50`：24 项校验针对**进度数值**（标志 `[0x7F1A94+i]`，ngbrecomp TECHNICAL.md、汉化工程 docs/24），不校验代码字节；把 HANZI 映射成可执行不触及它。不能由此推出：游戏没有别的完整性检查（阶段 4 用写监视核实）。

**光盘上的其他 XBE**（`docs/manifest_x21.tsv`）：`NinjaFCJ.xbe`、`NinjaFCU.xbe`（SFC 三部曲，x2.1 新增）、`NinjaTomoe.xbe`、`NinjaTrilogy.xbe`（原盘自带）。
主程序经 `XLaunchNewImage` → `HalReturnToFirmware`（填 LaunchDataPage 后重启）切换。路线 A 做不到；路线 B 的加载器可以**换 XBE 重启自身**（阶段 6 评估）。

### 1.4 黑之章的 D3D 用法与 black-pc 的覆盖面（ngbrecomp `docs/renderer.md`、`src/d3d/d3d_funcs.h`；`tools/compare_with_blackpc.py`）

- 黑之章调用 94 个 D3D8LTCG 符号；`SetRenderState_Simple` 329 处调用、`SetPixelShader` 51 处；141 个游戏函数经 LTCG 内联直接写 D3D 全局
  （SetTransform 把矩阵拷进设备 +0xDE0、脏位 0x3D83D0；全局设备指针 0x3D83C8；推送缓冲指针/上限 0x3D5F20 / 0x3D5F24）。
  **没有游戏代码直接写推送缓冲**（BLACK 有 3 处）→ 不需要 BeginPush/EndPush 解释器。
- 与 black-pc 已实现的 73 个 HLE 对照：**37 个已有**（Clear、Swap、SetRenderTarget、SetTransform/Viewport、SetTexture/Palette、SetStreamSource、
  Create/SetVertexShader、SetPixelShader(Constant)、DrawVertices(UP)/DrawIndexedVertices、资源引用计数、SetFence、BlockOnTime、CreateDevice 等）；
  **57 个没有**，其中约 30 个是单个渲染状态 / 贴图状态的设置函数（只写影子数组，工作量小，BLACK 走的是 `SetRenderStateNotInline`），
  **真正要新做的**：`CopyRects`、`SetGammaRamp`、`Reset`、`BlockUntilVerticalBlank`、`PersistDisplay`、`CreatePixelShader`、
  `LoadVertexShader`/`SelectVertexShader`/`SetShaderConstantMode`、`SetRenderState_VertexBlend`（固定管线蒙皮，NV2A 模式 1–6；ngbrecomp 证实没有它角色关节散架）、
  `EndVisibilityTest`、`RunPushBuffer`（预录推送缓冲，阶段 2 查是否真被调用）、`SetTileNoWait`、4 个 `SetTextureState_*`；
  `MakeSpace`/`KickOff`/`BlockOnResource`/`CMiniport_*`/`CDevice_*` 是库内部函数，调用者被替换后不应到达，保留"首次调用即停"。
- 黑之章需要、BLACK 不需要的渲染特性（ngbrecomp 实测）：固定管线顶点混合蒙皮、V8U8 凹凸贴图（第 2 章水面）、阴影贴图（小的纯深度目标，比较函数方向相反）、
  渲染到纹理（镜头切换淡入淡出拷进 512×512、菜单反馈背景）、伽马斜坡（画面调整）、未绑贴图的固定管线阶段、像素中心半像素差。
  可借鉴：ngbrecomp `src/d3d/gpu/`（TGP482 的 NV2A→HLSL 翻译、顶点/贴图解码，**MIT**，`src/d3d/NOTICE`）；black-pc 自己的 `nv2a_vsh/psh.cpp`。

### 1.5 本机环境（2026-10-17 实测）

- Visual Studio 2017 Community 在 D 盘（MSVC 14.16.27023，x86 工具链齐全），Windows SDK 10.0.17763 在 C 盘（**用户已装，不是我们写入的**）。
  black-pc 要 C++20；MSVC 14.16 对 C++20 支持不完整 → 阶段 1 先试 cl，不行就用 `toolchain\llvm-mingw-20260922` 里的 clang 以 `clang-cl` 模式配 VS2017 的库（black-pc 明确接受 clang-cl）。
- 便携工具（路线 A 的 `toolchain\`，只读复用）：CMake 4.4.4、clang 23、Python venv（含 capstone 5.0.9）。PATH 上没有 cl/cmake/ninja。
- **没有** XbSymbolDatabase、Ghidra、RenderDoc、x64dbg；Java 只有 JDK 1.8（Ghidra 12 要 21）。
- 硬件：i5-12500H、16 GB、RTX 3060 Laptop + Iris Xe（D3D9 都支持）。**E 盘仅剩约 6.2 GB**；路线 A 一个测试目录（含硬盘缓存里安装的 AFS）约 912 MB。
- 本目录 `CLAUDE.md` 的描述已过时：目录里现在没有 ISO；"全程用简体中文"已被用户 2026-10-16 的"繁体中文回复"取代。

### 1.6 路线 B 相对路线 A 的分工与卖点

| | 路线 A（已发布） | 路线 B（本计划） |
|---|---|---|
| x2.1 补丁 | 翻译器要加 pushad/标志位/ebp/跳转中间等专项支持（docs/03 A1–A3） | 原样执行，无需处理 |
| 其他 XBE（SFC 三部曲、街机） | 不支持（只翻译 default.xbe） | 加载器可换 XBE 重启，可评估支持 |
| 支持另一版本（vb-jp-x2.1） | 重新翻译 + 重新合并手写函数 | 库段相同、函数表相同，只需另一份哈希与 `.text` 补丁位置核对 |
| 图形后端 | Vulkan 1.3（Intel 核显 / 老机器受限） | Direct3D 9（SM3，覆盖面更广） |
| 程序形态 | 64 位，220 万行生成 C | 32 位，原版代码 + ≈2 万行兼容层 |
| 成熟度 | ngbrecomp 多月打磨（渲染一致性、设置菜单、宽屏 HUD 排布、各向异性 mip 等） | 从 BLACK 的层起步，黑之章的图形适配要全部重做 |

---

## 2. 开发计划（路线 B）

每个阶段：**目标 → 步骤 → 关口（必须拿到的证据）**。没过关口不进入下一阶段。文档编号见第 6 节。

### 阶段 0：工作区与输入冻结（需要用户确认目录用途与布局）

建议布局（全部在 `E:\games\emu\ninja gaiden black Hurricane recomp2\`；目录名含空格但纯 ASCII，构建输出不含中文）：

```
recomp2\
  CLAUDE.md                    改写为路线 B 的现状（待批准后改）
  开发计划_路线B.md             本文
  交接_路线A成果与路线B任务.md
  tools\                       量测与生成脚本（已有 3 个，只读量测）
  docs\00..                    证据与结论（编号文件）
  ngb-native\                  我们的分支：从 ...hurricane recomp\black-pc 本地 git clone，起点 4c84944，分支名待定
  game\x21 -> 硬链接 ...hurricane recomp\input\x21  （只读，不占空间；加载器的 "game" 目录）
  build\                       CMake 构建目录（纯英文）
  run\r001_...\                每轮测试独立：hdd\（E/T/U/Z 分区）、*.log、*.ini、shots\
  work\                        符号表、反汇编中间物（不提交）
```

- 不复制 black-pc / ngbrecomp / 原版目录；它们在路线 A 目录里只读引用。
- 记录输入哈希（两份 XBE、ISO、三个上游提交号）→ `docs/00_输入清单.md`（可直接引用路线 A 的 docs/00 并复算）。
- **关口 0**：哈希可复算；`ngb-native` 的 `git status` 干净、`git log -1` = 4c84944；E 盘剩余空间记入文档。

### 阶段 1：工具链与 black-pc 原样构建（不改一行）

- 用本机 VS2017 MSVC x86（或 clang-cl）+ 便携 CMake 配置 `ngb-native/loader`，输出到 `build\`；目标：**未修改的 black-pc 能编出 `black.exe` + `blackrt.dll`**。
  失败则记录 C++20 缺口，切换 clang-cl 重试；两种都记入 `docs/01_工具链.md`。
- 用它跑 `black.exe --info game\x21\default.xbe`：证明其 XBE 解析器能处理 17 段 / 145 导入 / 9 个库（预期随后因"未知版本"拒绝运行，这是正常的）。
- 需要下载的东西（每项下载前单独告诉你来源、大小、放在哪，等你同意）：
  1. **XbSymbolDatabase 源码**（Cxbx-Reloaded，几 MB，git clone 到 `recomp2\tools\`，本地用 CMake 编 CLI）——必需，用来给 DSOUND / XPP / XONLINE / XNET 段的函数命名；D3D 段已有 ngbrecomp 的 94 个符号；
  2. Ghidra + JDK 21（约 500 MB+）——**建议不装**：函数边界用路线 A 已有的 xboxrecomp 函数识别结果（x2.1 共 11,614 个函数）+ capstone 自扫；
  3. RenderDoc / x64dbg——图形阶段再按需申请。
- **关口 1**：构建产物哈希记录；`--info` 输出与 `xbeinfo.py` 一致；运行后检查 C 盘（`%LOCALAPPDATA%`、`%APPDATA%`、`%TEMP%`）没有新文件。

### 阶段 2：静态分析——黑之章的库函数表与地址事实（只分析，不改代码）

1. **库段函数表**：对 D3D、DSOUND、XPP、XGRPH、D3DX、XONLINE、XNET、PSFD00、DOLBY 每段列出全部函数（地址、大小、名字），
   来源 = xboxrecomp 函数识别（路线 A `build\` 下 x2.1 的结果）∪ XbSymbolDatabase 命名 ∪ ngbrecomp `d3d_symbols.txt`；
   生成 `*_functions.inc` 的输入（格式同 black-pc `notes/functions.csv`）。
2. **D3D 调用普查**：`.text` + HANZI 对 94 个符号的调用点与次数；内联写 D3D 全局的位置（设备指针 0x3D83C8、渲染状态影子数组基址、贴图状态数组、脏标志、推送缓冲指针）；
   游戏直接读写的设备对象偏移清单（已知 +0xDE0 变换矩阵；对照 BLACK 的 +0xC60 / +0xEE0 / +0x1DB4 等核出黑之章的值）。
   **方法**：BLACK 与黑之章的 D3D8LTCG 同为 qfe 15，把两段 D3D 代码按函数逐一比对（机器码去掉重定位后是否一致）；一致的函数其偏移可直接搬，不一致的逐条反汇编。
3. **内核导入普查**：145 个导入各自的调用者（`.text` 还是哪个库段），特别是 36 个新导入与 I/O 管理器那一组；XONLINE / XNET 在启动流程里被谁调用（XNetStartup、XOnlineStartup …）。
4. **特权与特殊指令**：`cli`/`sti`/`wbinvd`/`in`/`out`/`rdtsc`/`cpuid` 的位置（capstone，与路线 A 日志的三处对上），各自所在函数、是否会被替换覆盖。
5. **fs: 41 处**逐处标注所在函数；`fs:[reg]` 33 处逐处看是否是 KPCR 访问。
6. **PE 头拼接位置**：量 XBE 头 0x0–0xEC4 内哪些区域游戏运行期从不读取（RSA 签名 0x4–0x104 已知可用；证书 / 段头 / 库版本表运行期被 XAPI 读哪些），给出 NT 头副本的放置方案。
7. **x2.1 新代码清单**：从 addon_spec 复制 32 个入口 + 34 处挂钩的地址表（阶段 4 探针用）。
- **关口 2**：`docs/02_静态分析.md`——每张表有来源与复算命令；被 `.text` 调用的 D3D / DSOUND / XPP 函数**没有一个未命名**（或列入"待识别"并说明办法）；D3D 设备对象偏移表每项注明"已核对 / 推测"。

### 阶段 3：加载器与内核——跑到第一个 D3D 调用

- 分支 `ngb-native`，每项单独提交、写明原因：
  1. 桩程序保留区 ≥ 0xE07000（含头）；PE 头副本按阶段 2 方案放置；`main.cpp` 的 `known_build` 改为 x2.1 的 SHA-256；库函数表换成黑之章的；fs 改写期望数 41；
  2. 去掉 BLACK 专用补丁（timing、60fps、mouselook、autoaim、fov、fe），改成按构建标识门控或删除；
  3. 内核：36 个新导入先做"首次调用记录 + 安全返回"的桩（black-pc 的 `ktable.cpp` 机制），按日志逐个实现启动路径上真正用到的：
     `MmCreateKernelStack`（纤程栈）、`KeQueryPerformanceCounter/Frequency`、`KeTickCount`（必须走动，否则轮询超时死循环——xboxrecomp 注释里的 Halo 教训）、
     `KeWaitForMultipleObjects`、`KeSetTimerEx`、`Phy*`（报告无链路或有链路，二选一并记录）、`Xc*`、`Io*`；
  4. 文件层：`D:` = `game\x21`（只读）；E/T/U/Z 分区 = `run\<n>\hdd\`；黑之章会把关卡 AFS 安装到硬盘缓存分区（≈ 900 MB）——一次只保留一个 run 目录；
  5. 库段 D3D / DSOUND / XPP 全部函数挂"首次调用即停"钩子；XONLINE / XNET / XGRPH / D3DX / DOLBY / PSFD00 按阶段 2 的调用普查决定"原生执行"还是"挂钩"，并写明理由；
  6. 三处特权指令所在的库函数必须在被替换之列，否则单独打补丁（先核对原字节）。
- **关口 3**：日志显示 XAPI 初始化 → CRT → `main` → 第一个 D3D 调用（函数名、调用者、耗时），**没有未实现的内核调用、没有异常**；
  fs 改写数 = 41；HANZI 段映射为可读写可执行（`VirtualQuery` 证据）；C 盘无写入。

### 阶段 4：图形——标志画面、标题、中文主菜单与选项（证明 HANZI 原生执行）

- D3D HLE 适配：地址与设备偏移改成黑之章的；渲染状态 / 贴图状态单项设置函数（≈ 30 个，写影子数组）；
  新实现 `CreatePixelShader`、`LoadVertexShader`/`SelectVertexShader`/`SetShaderConstantMode`、`BlockUntilVerticalBlank`（与 pace_swap 的"只等一次"）、
  `CopyRects`、`SetGammaRamp`、`PersistDisplay`、`Reset`、`EndVisibilityTest`、`RunPushBuffer`（若被调用）、`SetTileNoWait`；
  固定管线：顶点混合蒙皮（模式 1–6）、材质 / 光照、未绑贴图的阶段；YUV 贴图（影片帧）；P8 + 调色板（字库页 256×2048、HUD）；阴影贴图比较方向；半像素。
  NV2A 顶点程序 / 合成器翻译沿用 black-pc 的，缺的参考 ngbrecomp `src/d3d/gpu/`（MIT，保留版权声明）。
- 探针：在 32 个 HANZI 入口与 34 处挂钩加运行时计数（black-pc 的 `--probe` 机制），证明"中文菜单 / 准星行 / 交差之法选项"等画面出自原生执行的新代码。
- **关口 4a**：启动标志、"Warnings" 画面、标题画面（含 Hurricane 角标）截图（只截游戏窗口）；帧数 : 垂直同步数 ≈ 1 : 1。
  **关口 4b**：中文主菜单、选项 → 控制器设置（含"交差之法 飓风包 / 黑之章"行）、画面设置（含弓箭准星行）截图；探针日志列出已命中的 HANZI 入口；与 xemu 上 x2.1 的同画面并排对照。

### 阶段 5：音频、输入、存档——进第一章

- DSOUND HLE 地址与钩子表换成黑之章的；流接口（CRI ADX 音乐 / 语音）、3D 音效、混音仓；DSP 效果镜像若被下载则按 BLACK 的方式折叠。
- XPP：XInput 钩子地址；插入通知生命周期（幻魔教训）；键鼠先用 black-pc 的绑定框架，默认键位参照路线 A 的表。
- 存档：`UDATA\5443000d`、`TDATA\5443000d\system.dat`；用路线 A 的 `tools/xemu_saves.py` 导入 xemu 存档做对照测试。
- **关口 5**：新游戏 → 第 1 章可玩、有声音、存档读档；x2.1 功能抽查：扳机选服装、交差之法两种模式切换、宁静腕轮、结算行；
  帧率 / 游戏速度量化（black-pc 的 perf 记录：每 60 帧 ≈ 1.0 s）；死锁 / 卡顿观察写明"已观察到 / 未观察到"。

### 阶段 6：功能验证矩阵（对照路线 A docs/05 的清单）

- 汉化：标题、菜单、物品、商店、存读档、地图、章节标题、对白、过场字幕、结算；
- 飓风功能：交差之法（docs/36）、宁静腕轮、妖刀鬼彻、带金圣甲虫二周目与 Game Over（docs/24）、服装（docs/23）、还魂护符（docs/32）、准星、视角、任务模式（docs/34）；
- **SFC 三部曲 / 街机**：实现 `XLaunchNewImage` → 以 LaunchDataPage 重启加载器并加载 `NinjaFCJ.xbe` 等；这些 XBE 的库版本、导入、D3D 用法要单独普查（它们是另外的程序），结论可能是"支持 / 有限 / 不支持"；
- 与路线 A 的 PC 版、xemu 三方对照同一存档同一场景。
- **关口 6**：矩阵全部有结论（通过 / 有限制 / 不支持 / 未测原因），没有空项。

### 阶段 7：PC 功能与发布形态（需要用户决定）

- 沿用 black-pc 的通用功能：任意分辨率、宽屏（黑之章自己有 16:9 模式）、无边框全屏、改键、设置文件、`--background` 测试、perf 记录。
- 黑之章自己的 60 fps 逻辑不需要 BLACK 那种 30→60 补丁。
- 默认画质（用户决定，2026-10-19）：保持现有内置默认——渲染为 Xbox 720×480 的纵向 3 倍（16:9 下 2880×1440），后处理抗锯齿开，缩放到窗口 / 屏幕显示。核显 Iris Xe 第 1 章约 9 ms/帧。注：本作档案 black=false，键鼠、影片跳过、PC 设置菜单等 black-pc 功能对本作都未启用（证据盘点 2026-10-19）。
- 接缝填补、后处理抗锯齿默认开（用户决定，2026-10-19："现在这个画质就很好"）。
- 显卡选择（用户决定，2026-10-19）：设置里加下拉框，默认独立显卡；机器没有独显时自动降为集成显卡。开发测试仍默认用集显（见记忆规则），与发布默认值分开。
- PC 设置入口（用户决定，2026-10-19）：单独做一个 PC 设置小程序（中文界面，可用鼠标操作），读写 black.ini；不放进游戏自己的选项页。
- 图形预设（用户要求，2026-10-19）：设置小程序里提供几组图形组合，如"最轻量""最高画质""动作""电影"等；各组参数做设置程序时列出请用户确认。
- PC 设置菜单：黑之章菜单是游戏自己的行表（路线 A 已有做法：ngbrecomp 的 RECOMP SETTINGS 借用选项页框架），**不是** BLACK 的 Apt；中文用 NGB Hanzi 子集 / 预渲染图片（鼎猎宋刻体不可嵌入）。
- 发布：fork 为 GPL-2.0-or-later（black-pc 的许可），分发程序即分发源码；程序里不含任何游戏字节（玩家自备 x2.1 镜像解包）；程序名、版本号、标题格式、作者署名（b站 哈雅布撒 / QQ 2682557218）届时请你定。
- 联网（用户计划，2026-10-19）：日后接入 signia 服务器（用户原话"signia"，应即 Xbox Live 复活服务 Insignia，开工前与用户确认）。现状：XNetStartup 返回无网络（WSAENETDOWN）；排行榜页"新增用户"会让游戏重启到 Live 仪表板（xonlinedash.xbe），加载器随即退出。网络相关的桩不要堵死日后真正的 XNet / XOnline 实现；外部连接与下载先问用户。

---

## 3. 风险与对策

| 风险 | 依据 | 对策 / 判断方法 |
|---|---|---|
| D3D 设备对象偏移、状态数组布局与 BLACK 不同 | 同 qfe 15 但链接不同 | 阶段 2 按函数比对两段 D3D 机器码；只搬"比对一致"的偏移，其余反汇编核出 |
| 固定管线蒙皮 / 光照 / 阴影贴图 black-pc 没有 | BLACK 是 RenderWare 着色器管线 | 阶段 4 新写 HLSL；参考 ngbrecomp `gpu/`（MIT）与 xemu 行为（不抄 GPL 代码） |
| 单 CPU 优先级调度依赖（CRI 服务线程）、渲染锁死锁 | xboxrecomp `vcpu.h`、路线 A #40 | black-pc 的单核固定 + 优先级映射先实测；若出现随机挂起，再评估 xboxrecomp 式"令牌调度"（工作量大，单独立项） |
| 每帧等两次 vblank → 半速 | 幻魔 §05 | pace_swap 只在未满足间隔时等待；用"帧数 : 同步数"量化 |
| PE 头副本放不下（空隙 0x13C） | 头长 0xEC4 | 阶段 2 量测运行期不读的头区域；桩程序段数减到最少（/MERGE）；或把副本放进签名区 |
| 36 个新内核导入里有未知语义 | I/O 管理器、Xc* | 先记录桩，按启动日志逐个实现；xboxrecomp 的实现作行为参考 |
| XONLINE / XNET 启动初始化挂起或走错 | `PhyInitialize`、Ranking | 阶段 2 普查调用路径；`KeTickCount` 走动；Ranking 行处理参照 ngbrecomp |
| 特权指令（cli/sti/wbinvd 三处）在未被替换的函数里 | 路线 A r003 日志 | 阶段 2 定位所在函数；必须在替换表内，否则按幻魔做法改单条指令（先核原字节） |
| 影片 YUV、P8 调色板、原位改写贴图缓存 | 幻魔 §07 §09 | 贴图缓存键含内容与调色板；每帧首次绑定检查 |
| 工具链：VS2017 对 C++20 不全 | `CMakeLists.txt` | clang-cl 备选；阶段 1 先验证原样构建 |
| E 盘空间（6.2 GB） | 实测 | run 目录一次一个；game 用硬链接；构建用 RelWithDebInfo 单配置 |
| 另一完整性检查未知 | 已知 0x164E50 只查进度 | 阶段 4 对 HANZI 与 `.text` 补丁位置加写 / 读监视 |

## 4. 执行规范（沿用路线 A §4 与交接文档 §3）

- 一次只改一处，每处有验收；诊断可以绕过，正式修正保留原语义。
- 测试一律用 `run\<编号>\` 的独立 HDD / 设置 / 日志；每轮前后记录哈希；只截游戏窗口；只关自己开的进程。
- 回复用繁体中文；项目文档简体。文件只放本目录；C 盘不写；下载、提交推送、发布、开 issue 先问；合并上游有冲突先逐个说明。
- 不删备份 / 镜像 / 用户文件；原版镜像与路线 A 目录只读。
- 结论写清"已证实"与"不能由此推出"；"没报错"不等于"正确"。

## 5. 待用户决定（开工前）

1. **本目录用途与布局**：按第 2 节阶段 0 的布局使用 `recomp2`？`CLAUDE.md` 是否现在改写为路线 B 现状？
2. **分支 / 程序命名**：fork 目录名（建议 `ngb-native`）、程序名（如 `ngbh.exe` + `ngbhrt.dll`）、窗口标题与发布名格式。
3. **下载清单**（阶段 1）：XbSymbolDatabase 源码（必需）；Ghidra + JDK 21（建议不装）；RenderDoc / x64dbg（按需）。
4. **编译器**：先用你已装的 VS2017（D 盘）；不行再用便携 clang-cl。两者都不写 C 盘。
5. **许可边界**：fork 保持 GPL-2.0-or-later；可以引用 ngbrecomp 里的 MIT 部分（保留声明）；Cxbx / xemu / xboxrecomp 的 GPL 代码只读不抄（除非你另行决定）。
6. **第一个里程碑**：按阶段 3 → 4a → 4b 的顺序，还是你希望先直奔第 1 章？
7. **SFC 三部曲 / 街机**：是否列为路线 B 的目标之一（阶段 6 评估后再定也可以）。

## 6. 文档索引（随进度补充）

- `docs/00_输入清单.md`（阶段 0）
- `docs/01_工具链.md`（阶段 1）
- `docs/02_静态分析.md`（阶段 2：库函数表、D3D 普查、内核导入普查、fs/特权指令、PE 头方案）
- `docs/03_加载器与内核.md`（阶段 3）
- `docs/04_图形.md`（阶段 4）
- `docs/05_音频输入存档.md`（阶段 5）
- `docs/06_验证矩阵.md`（阶段 6）
- `tools/xbe_sections_diff.py`、`tools/xbe_fs_scan.py`、`tools/compare_with_blackpc.py`：本文 1.3 / 1.4 的量测脚本（只读）。

## 7. 决定记录

- 2026-10-16 用户：两种方式都做，先 A 后 B；A 已发布 v0.9.13（2026-10-17）。
- 2026-10-17 用户：先交路线 B 计划审阅，批准前不写代码；开工前确认本目录用途与布局。
- 2026-10-17 用户批准计划，并对 §5 作出决定：
  1. 本目录按阶段 0 的布局使用，`CLAUDE.md` 改写为路线 B 现状；
  2. fork 目录名 **`ngb-recomp-native`**（程序名、窗口标题、发布名待阶段 7 再定）；
  3. 同意下载 XbSymbolDatabase 源码（Ghidra / JDK 21 不装；RenderDoc、x64dbg 按需再问）；
  4. 编译器先用 VS2017，不行再换 clang-cl；
  5. 许可由我选择：fork 保持 GPL-2.0-or-later，可引用 ngbrecomp 的 MIT 部分（保留声明），Cxbx / xemu / xboxrecomp 的 GPL 代码只读不抄；
  6. 里程碑顺序未另指示，按阶段 3 → 4a → 4b；
  7. SFC 三部曲 / 街机：容易做就列为目标（阶段 6 评估后决定）。
- 2026-10-17 进度：关口 0、1 通过（docs/00、01）；阶段 2 进行中（docs/02：函数表、调用普查、内核导入普查、特权指令、fs、PE 头位置已做；
  10 个未命名 D3D 函数、设备偏移表、XNET 启动路径待做）。E 盘已清出约 97 GB，§1.5 的空间顾虑解除。
- 2026-10-17 用户：同意现在开始阶段 3，阶段 2 剩余四项（docs/02 §10：10 个未命名 D3D 函数、待核的设备偏移、RunPushBuffer 来源、XGRPH/D3DX/PSFD00 小函数）
  在阶段 3 期间并行补齐，**不得遗漏**（阶段 4 关口前必须全部关闭）。
- 2026-10-17 **关口 3 通过**（docs/03）：分支 `native` 提交 82cc336 / 2958fa5 / ff58a52；x2.1 在 84 ms 内从 XAPI 初始化跑到第一个 D3D 调用
  `Direct3D_CreateDevice`（0x3CC310），无未实现内核调用、无异常、fs 改写 41。下一步：阶段 4（D3D HLE 适配，从 CreateDevice 起逐函数推进），
  同时补齐阶段 2 遗留四项。
- 2026-10-18 阶段 4 进展（docs/04）：第一章场景**正确渲染**（r021 截图：隼龙、岩壁、瀑布水雾）。本轮修的根因都有证据：灯光数组（device+0x7A4）、SetRenderTarget 重设视口、
  RunPushBuffer = 光盘里的索引流（cmn.afs 逐字节相同）、XAPI 纤程栈要同步 TEB 边界、D3D9On12 复制 MinVertexIndex+NumVertices 个顶点、
  LTCG 内联写入的矩阵（device+0xC60）与材质（device+0xF00）在绘制前从设备对象同步。用户提醒：进关前要按 X。改动未提交，待用户确认。
- 2026-10-18 下午 阶段 4 进展（docs/04 §3.8）：用户实机试玩第二轮修掉四个根因——预变换顶点的贴图坐标缩放（快照 / 影片 / 发光链复制整张单色）、常数 alpha 混合（撿道具弹窗被盖）、深度格式贴图头取样返回深度值（刀光 / 重攻击光爆条纹）、Z bias 与 polygon offset 映射（岩石光块随走动闪）。序章 CG 随之正常播放。未解决：隼龙没有影子（机制已查明，见 docs/04 §3.8 末与 §4）。本轮改动未提交（等用户确认）。
- 2026-10-18 下午 第三轮（docs/04 §3.9）：rdtsc 陷阱（733.33 MHz）+ 引擎计时除数让影子回来并稳定；深度贴图 2D 取样回传深度值（xemu 的比较模型会让效果全消失）；未绑贴图层为白（双截棍特效）；线段按线宽×倍率展开（击墙火花）；固定管线贴图层改为自生成着色器（绕过 D3D9On12 的 alpha 调制缺陷）。待用户在 xemu 对照刀光外观与影子深浅。已提交 `4eed2b4`（分支 `native`，19 个文件）。
- 2026-10-18 关口 4 自动验收（docs/04 §3.10）：4a 通过（警告→影片→标题含 Hurricane 角标与中文署名，59.5 fps）；4b 中文主菜单、控制器设置（交差之法）、画面设置（弓箭准星、影片比例）截图齐，HANZI 32 个入口中 12 个在菜单流程原生执行；待用户提供 xemu 同画面截图做并排对照。新增 `--coverage`、`--no-rdtsc`（未提交）。
- 2026-10-18 关口 4 与 xemu 并排对照完成（四组画面内容一致，docs/04 §3.10），**关口 4 通过**；下一步关口 5。
- 2026-10-18 刀光 / 光爆 / 水面：深度格式头在固定管线下改取背缓冲颜色（与 xemu 实测对照选出，docs/04 §3.11），用户确认全部正常。未提交。
- 2026-10-18 傍晚（docs/04 §3.13）：开门闪黑解决——HLE 把后缓冲放在 device+0x1A18（原版是前缓冲），转场快照取到当帧而非上一帧；改为 +0x1A14 后缓冲、+0x1A18 独立前缓冲并在 Swap 时复制。用户确认不闪、特效正常。未提交。
- 2026-10-18 傍晚 关口 5 自动部分（docs/05）：新游戏进第 1 章、有声音、存档写入通过；第 1 章 perf：速度 96.4%、平均帧 15.5 ms；自动测试误覆盖用户一个存档（docs/05 §4），已改为每次复制硬盘。待用户：存读同一格、服装 / 交差之法 / 宁静腕轮 / 结算行。
- 2026-10-18 夜（docs/05 §6–7）：开场 CG 字幕重复 = 影片跑 60 帧（原机严格呈现间隔下 30 帧），`pace_swap` 修正后 30 帧、字幕不再重复；第 1 章帧时间 16.1 → 10.8 ms（贴图调色板快路径、震动节流、动态环形缓冲）。待：实时过场帧率实测、XNetStartup 崩溃。未提交。
- 2026-10-18 **关口 5 通过**（docs/05 §8）：用户确认结算行；全部清单项有结论。遗留：帧率模式 5 的场景、联网功能崩溃（暂缓）。下一步阶段 6 功能验证矩阵。
- 2026-10-18 阶段 6 开始（docs/06）：矩阵初稿（汉化 8 项通过、飓风功能 3 项通过、其余待测）；附带 XBE 普查：4 个均为非 LTCG D3D8（5659 ×2、5849 ×2），支持需每个 XBE 一套地址表 + XLaunchNewImage 重启，估计数天，待用户决定是否投入。
- 2026-10-19（docs/04 §3.14–3.15）：未绑贴图的固定管线层改为整层跳过（`cfg:untex:3`，血条与三部曲选择页与 xemu 逐像素一致，`untex:2` 删除）；点精灵状态传给宿主并按原版库只用第 3 层（火把火星正确，烟仍偏淡待对照）；生成表目录 aux → auxtab（Windows 保留名）。
