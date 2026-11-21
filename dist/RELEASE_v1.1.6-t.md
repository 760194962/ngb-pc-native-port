**NINJA GAIDEN BLACK [飓风版][中英] PC 原生移植 v1.1.6-t**　PC 移植：b站 哈雅布撒（QQ：2682557218）

《忍者龙剑传 黑之章》飓风版中文汉化 **vb-zh-x2.1**（初代 Xbox）的非官方 Windows 原生移植。这个包只含程序，不含游戏数据；需要你自己的 vb-zh-x2.1 镜像。

## 1.1.6-t

- **修好第三关捡完地图后的天窗实时过场整屏红棕色、人物没有立体感**：这段过场游戏设置的固定功能线性雾范围是 `start 5 / end -1.2`（反向、无效），雾色 `0xff6e3c1e`。D3D9 固定功能雾由显卡驱动计算：Intel 驱动会把整个场景饱和成雾色，NVIDIA 驱动不会，所以同一段游戏在集显上红、独显正常。现在线性雾 `end <= start` 时关闭主机雾，正常范围的雾不受影响；Intel 集显实测过场恢复正常。
- 包含 1.1.5-t 的启动器下拉框滚动/向上展开，以及游戏内「退出游戏」的 3 秒 watchdog。

**下载**：`ngb-hurricane-pc-1.1.6-t-win32.zip`，2,764,127 字节，SHA-256 `0137e06c4bd2bcd73c84a84a03bc9c84799dce8579a83506249e219ade3e6f41`。
调试符号：`ngb-hurricane-pc-1.1.6-t-win32-symbols.zip`，19,715,621 字节，SHA-256 `4d23c9bcaee561745be34ce705e2270e35cc1a5b2617291c6e53ee64f297db79`。

**Steam Deck**：「退出游戏」的 watchdog 仍待 Proton 实机确认；如果仍卡，请附游戏文件夹里的 `logs\black.log`。

---

A native Windows port of Ninja Gaiden Black's Hurricane Chinese translation **vb-zh-x2.1** (original Xbox): **1.1.6-t**.

**New in 1.1.6-t**: fixed the flat red-brown real-time cutscene after the Stage 3 map pickup. That cutscene sets a reversed, invalid fixed-function linear fog range (`start 5 / end -1.2`, colour `0xff6e3c1e`); D3D9 fixed-function fog is driver code, so Intel saturated the whole scene to the fog colour while NVIDIA did not. Host fog is now disabled when a linear range has `end <= start`; valid fog is unchanged, and the Intel iGPU path is confirmed fixed. This build also includes 1.1.5-t's launcher drop-down scrolling/upward opening and the in-game EXIT GAME watchdog.

The zip is 2,764,127 bytes, SHA-256 `0137e06c4bd2bcd73c84a84a03bc9c84799dce8579a83506249e219ade3e6f41`. Debug symbols are in the separate `-symbols.zip`.
