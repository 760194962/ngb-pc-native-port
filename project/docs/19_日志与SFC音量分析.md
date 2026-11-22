# 1037U 日志与 SFC 音量分析

日期：2026-11-21 / 2026-11-22。证据来源：`E:\hayabusa\Downloads\1037u+37.2启动器的15个log`、`black.NinjaFCJ.log`。

## 一、15 个日志先纠正三件事

1. 日志头全是 **2.2.37.3**，不是 2.2.37.2。文件夹名写「37.2 启动器」，实际运行的是 2.2.37.3。
2. 15 个目录里只有 **9 次不同的运行**。10–15 的 `black.log` 与 9 完全一样，SHA-256 相同，是同一个日志复制了 6 份。
3. 路径是 `Z:\userdata\...`，是 Wine / Proton 环境。`KernelVblank` 可能初始化失败；日志里 60Hz 独佔也显示 `DwmFlush`。

## 二、逐个结果

| 日志 | 设置 | 结果 | 关键点 |
|---|---|---|---|
| 1 | 窗口，vsync 关，scale 1 | 8.3 秒崩溃 | 空指针读 0x8，地址 0x30ad6b；之前刚读 demo.afs |
| 2 | 窗口 1280×720，vsync 关 | 23.1 秒崩溃 | 空指针，地址 0x2f8900；之前 system.dat 分配提示被拒 |
| 3 | 同上，删了 P4/P5 内容 | 11.7 秒崩溃 | 同一个 0x2f8900 |
| 4 | 窗口 1280×720 | 7.1 秒崩溃 | 空指针 0x30b28e；之前 instinfo.ins 缺失 |
| 5 | 窗口，正常读档 | 135.2 秒崩溃 | 空指针 0x190df8；死亡后重读档、移动就崩 |
| 6 | 窗口，vsync 关 | 122.3 秒正常退出 | 没有崩溃 |
| 7 | 窗口 853×480 | 13.2 秒崩溃 | 又是 0x2f8900 |
| 8 | 窗口 853×480 | 42.4 秒正常退出 | 没有崩溃 |
| 9 | 独佔 60Hz，vsync 开 | 138.0 秒正常退出 | 正常运行 |
| 10–15 | 和 9 完全相同 | 同一份日志 | 不是 6 次独立测试 |

## 三、崩溃分类

### 1、2、3、7：同一类

都是空指针，2、3、7 都在 0x2f8900。日志 3 明确写了「删除 P4 和 P5 文件夹内容」，之后马上崩。日志 2、4、7 也有 P4/P5、instinfo.ins、system.dat 相关的缺失或异常。

判断：像 **存档 / P4 / P5 / 安装数据被删或损坏后，游戏代码拿到空指针**。

### 4：安装信息缺失

`instinfo.ins` 缺失后，空指针 0x30b28e。也可能是 P4/P5 被删的后果。

### 1：影片 / demo 路径

刚读 `demo.afs`，之前有视频退出，空指针在 0x30ad6b。更像影片 / demo 数据路径的问题。

### 5：读档后的游戏状态

前面正常玩了 135 秒，死亡、重读档、刚移动就崩在 0x190df8。更像读档后的游戏状态 / 绘制数据问题，和前面「标题菜单就崩」不是同一类。

### 6、8、9–15：正常

同样是窗口、vsync 关、scale 1，6 和 8 都正常。说明设置本身不是必崩。

## 四、日志结论

1. 早期崩溃重点不是画面设置，而是：
   - P4/P5 内容被删；
   - instinfo.ins 缺失；
   - system.dat 异常；
   - 存档 / 读档状态损坏；
   - 影片 / demo 数据路径。
2. 日志 5 是另一个问题：读档后死亡再读档，游戏代码空指针。
3. 10–15 是同一份正常日志，不能当成 7 次独立验证。

建议：先恢复 P4/P5 和 savedata 的完整备份，不要删 P4/P5 内容，再用同一个存档重测。若恢复后仍在 0x2f8900 崩，就不是设置问题，要看存档或游戏数据。

## 五、SFC 音量小：日志证据

`black.NinjaFCJ.log`：

```text
CreateSoundBuffer(flags 0x100, 25600 bytes, tag 0x1 2ch 32000Hz 16bit align 4 avg 128000, 0 mix bins)
Voice_SetVolume(0A3B76E8, -443)
mixer: 1 buffers, 0 streams playing, peak 0.06–0.11
```

关键：

- 只有一个 2D 音频缓冲；
- 0 个 mix bin；
- 音量 -443，约 -4.43 dB；
- 2D voice 默认 headroom 600，即 -6 dB；
- mix bin 默认 headroom 1，即再 -6 dB；
- 总衰减约 -16.4 dB。

主游戏黑之章启动时会调用 `SetMixBinHeadroom`，把 0–10 号 bin 的 headroom 设成 0，所以主游戏只吃 voice 的 -6 dB。SFC 程序没有做这一步，因此比主游戏多衰减 6 dB。

所以原因不是 SFC 模拟器音量设得太低，而是：

> SFC 程序的 2D 音频缓冲没有把 mix bin headroom 设成 0，HLE 按默认值又压了 6 dB；主游戏会设 0，SFC 不会。

## 六、SFC 音量的彻底方案

不是简单加 6 dB，而是把「附带程序的音量基准」单独建模。

1. 用 xemu 或原机录一段 SFC 忍龙的同一段音乐 / 音效，和 HLE 输出做电平对比，先确认原机上 SFC 是不是也比主游戏小。
2. 在 HLE 里按程序区分音频配置：主程序、NinjaFCJ、NinjaFCU、NinjaTrilogy、NinjaTomoe 各自有独立的 headroom / master gain。
3. 补完 DirectSound 的 headroom 语义：voice headroom、mix bin headroom、primary buffer / master volume 分别怎么作用。
4. 给附带程序明确的音量基准：SFC 默认 +6 dB，或启动器里加「SFC 音量」滑条，只对 SFC / 街机程序生效。

不要全局把 bin headroom 都改成 0，否则主游戏会多 6 dB，音量平衡会变。

## 七、相关文档

- 呈现 / vblank / kernel vblank 的问答：`16_呈现节拍问答.md` 第 72–77 节。
- 呈现节拍修复方案：`17_呈现节拍修复与重构方案.md`。
