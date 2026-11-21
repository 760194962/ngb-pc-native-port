**NINJA GAIDEN BLACK [飓风版][中英] PC 原生移植 v1.1.5-t**　PC 移植：b站 哈雅布撒（QQ：2682557218）

《忍者龙剑传 黑之章》飓风版中文汉化 **vb-zh-x2.1**（初代 Xbox）的非官方 Windows 原生移植。这个包只含程序，不含游戏数据；需要你自己的 vb-zh-x2.1 镜像。

## 1.1.5-t

- **启动器下拉框支持滚动**：列表按窗口剩余高度显示可见行数，底部空间不够时向上展开；右侧有滚动条，鼠标滚轮与 Up / Down 都能滚。修好底部列表（如手柄按钮重映射、较长的下拉框）越出窗口后看不见的问题。
- **游戏内「退出游戏」加退出兜底**：`request_exit` 置位后，新增的 watchdog 线程在 3 秒内还没退出就强制 `TerminateProcess`，针对 Proton/Wine 可能把 D3D 或音频线程卡在驱动调用里的情况；同时去掉终止前多余的 `fflush(stdout)`。Windows 正常退出路径不变。
- 版本号改为 `1.1.5-t`（2026-10-26 起接手的发版都带 `-t`）。

**下载**：`ngb-hurricane-pc-1.1.5-t-win32.zip`，2,763,084 字节，SHA-256 `35a54112833629e79068ad258cfb304a1550e68bf75cc3abff14b94e1266b262`。
调试符号：`ngb-hurricane-pc-1.1.5-t-win32-symbols.zip`，20,533,837 字节，SHA-256 `f3acb5c9f133bdc2e5c7ea547a56394c65317fe547bc7c2fdb82edc704407f8e`。

**Steam Deck**：Windows 上 `--timeout` 的退出路径实测正常；Steam Deck（Proton）上的「退出游戏」还没有实机确认。如果仍卡，请附游戏文件夹里的 `logs\black.log`，重点看有没有 `exit menu: yes, leaving`、`exit requested: EXIT GAME on the main menu`、`exiting with code 0`。

---

A native Windows port of Ninja Gaiden Black's Hurricane Chinese translation **vb-zh-x2.1** (original Xbox): **1.1.5-t**.

**New in 1.1.5-t**: the launcher's drop-down lists now size themselves to the window, open upward near the bottom and scroll (scrollbar, mouse wheel, Up/Down), so long lists no longer run off-screen; the in-game EXIT GAME path has a watchdog that terminates the process three seconds after the exit request if Proton/Wine leaves a D3D or audio thread stuck in a driver call. Windows exit behaviour is unchanged. Steam Deck still needs a player's test: if it still hangs, send `logs\black.log`.

The zip is 2,763,084 bytes, SHA-256 `35a54112833629e79068ad258cfb304a1550e68bf75cc3abff14b94e1266b262`. Debug symbols are in the separate `-symbols.zip`.
