# 09 联网：用自己的 Insignia 账号进排行榜（2026-10-23）

目标（用户原话）：「我只希望我的游戏能以自己的 signia 服务器账号进入排行榜，查看排名」。不做多人对战。

结论：**已实现并由用户亲测成功**（2026-10-23，`run\user_online`）：游戏里的排行榜列出用户自己的玩家代号，登录
Insignia 后显示真实的中忍榜单，用户的名次也在其中。

## 怎么打开

默认关闭，不打开时游戏和以前完全一样（看不到网络，排行榜报无法连接）。需要玩家自己已经在 xemu 里注册过 Insignia
的那一套文件：

```
# black.ini（路径可以有空格）
online = 1
online_eeprom = E:\games\emu\xemu\xemu\Component\4- Eemprom\eeprom.bin
online_hdd = E:\games\emu\xemu\xemu\Component\3- Xbox HDD\xbox_hdd.qcow2
# online_dns = 46.101.64.175   （可选：游戏用 DHCP 给的 DNS 时转发到哪里）
```

命令行：`--online-eeprom <文件> --online-hdd <文件>`（同时打开联网），`--offline` 关闭。

注意：

- 两个文件只读，不写回；硬盘镜像可以是 xemu 的 qcow2（未压缩簇）或原始镜像。
- 同一台「主机」不能同时登录两次：启动时发现 `xemu.exe` 在运行就不开联网（日志 `online: off (xemu is running…)`）。
- 主机网络设定里如果是手动 DNS（注册时常见：主 DNS 是跑 InsigniaDNS 的那台电脑，次 DNS 1.1.1.1），游戏就去问这些地址；
  所以要么在那台电脑上开着 InsigniaDNS.exe，要么那台电脑的地址恰好就是本机。只有主机用自动 DNS 时才转发到 `online_dns`。
- 代理软件的 TUN / fake-ip 模式会把所有 DNS 截走、回 198.18.x.x 的假地址，Insignia 连不上（下文「排查记录」）。测试时
  关闭 TUN，或把 `+.xboxlive.com` 加进 fake-ip 过滤、`46.101.64.175` 与 Insignia 服务器直连。

## 做法

游戏自带 XONLINE + XNET（完整的 TCP/IP 栈、Kerberos、安全网关），XNET 的网卡驱动直接写 nForce 网卡寄存器。所以不去
模拟服务、只模拟硬件和内核：游戏自己的联网代码原样运行，和真机 / xemu 一样跟 Insignia 对话。

| 部分 | 文件 | 说明 |
|---|---|---|
| 主机身份 | `net/online.cpp` | EEPROM 安全区按 xemu `eeprom_generation.c` 的分版本 SHA-1 状态 + RC4 解密（用户的是 1.0 版），得到 `XboxHDKey`；出厂区的序列号、MAC、在线密钥经 `ExQueryNonVolatileSetting(0x100/0x101/0x102)` 给游戏；qcow2 读出硬盘前 512 KB 配置区 |
| 账户扇区 | `kernel/k_file.cpp` | `\Device\Harddisk0\Partition0` 第 8 扇区起（网络设定、机器账户、Live 账户）从注册过的硬盘读；游戏写回的内容只留在内存，不改任何文件 |
| 加密导出 | `kernel/k_crypto.cpp` | XcRC4Key/Crypt（Kerberos）、XcModExp（Montgomery，安全网关 DH）、XcDESKeyParity / XcKeyTable / XcBlockCryptCBC（DES、3DES-CBC）、XcVerifyPKCS1Signature；开联网时先做已知答案自检（DES FIPS 81、RC4、模幂） |
| 网卡 | `net/nvnet.cpp` | 0xFEF00000 保留为不可访问页，寄存器访问触发异常后解码指令（mov / test / or / and / ALU / movzx）在网卡模型上执行；模型按 xemu `nvnet.c`（LGPL 2+）：描述符环、发送踢动、接收、电平中断。中断在网络线程上以中断 IRQL 调用驱动的 ISR（持有派发锁，不会和驱动的 DPC 并行）。`PhyGetLinkState` 报链路已连接 100M 全双工 |
| 本机网络 | `net/vnet.cpp` | 仿 xemu 用户态网络：主机 10.0.2.15、网关 10.0.2.2（ARP、DHCP）、10.0.2.3 DNS；UDP 经本机套接字 NAT（3074 端口能用就用同一个），IP 分片重组 / 拆分；不记录任何负载 |
| 其余内核 | `k_dpc.cpp` `k_sync.cpp` `k_memory.cpp` `input/xinput.cpp` | `KeSetTimerEx`（驱动 200 ms 周期 DPC）、事件句柄转 KEVENT（XOnlineLogon 的完成事件）、`MmQueryStatistics`（随机种子）、XPP 0x3DA598（问记忆卡：没有）、XNET 的 `MmGetPhysicalAddress` 对非连续内存给「地址 | 0x80000000」 |
| 开关 | `d3d/dispatch.cpp` | 开联网时不再用「XNetStartup 返回网络断开」的替身 |

## 证据

- r435–r437：进排行榜后 NIC 初始化（`[net] NIC: first register write`）、读第 8/9/11/12 扇区、3DES 解密机器账户，
  排行榜列出用户的玩家代号（r436 截图 `r40.png`）。
- r438：DHCP 正常；登录去问主机设定里的手动 DNS 192.168.2.8 / 1.1.1.1，失败后游戏提示「无法连接 XBOX LIVE」。
- r439：查到 Clash Meta TUN（fake-ip）把发往 46.101.64.175 的查询也截走（本机 `nslookup example.com 46.101.64.175`
  回 198.18.1.0）。
- `run\user_online`（用户本人，TUN 关、InsigniaDNS 开）：`AS.XBOXLIVE.COM -> 49.13.57.101`，但第一个 Kerberos 包 462 字节
  全是 0：发送缓冲在 64 MB 以下，`& 0x7FFFFFFF` 的「物理地址」被网卡模型当成连续内存。修正（XNET 调用时置最高位）后：
  Kerberos 通过，与安全网关 51.68.122.14:3074 建立连接，排行榜显示真实榜单与用户名次（用户截图）。

## 性能

只有 `online = 1` 时才装异常处理与网卡；网络线程只在进入排行榜、驱动第一次写寄存器后才启动，且等待事件，不空转。
关闭时的额外开销只有 `NtCreateEvent` / `NtClose` / `KeSetEvent` 各多一次表查找。

铁律复核（集显 Iris Xe，第 1 章 bench，1920×1080）：

| 运行 | 游戏速度 | 超时帧 | 平均帧时间 |
|---|---|---|---|
| r444 上次提交（不含联网） | 97.7% | 0.2% | 8.31 ms |
| r443 新版，联网关闭 | 97.7% | 0.2% | 8.42 ms |
| r441 上次提交 | 97.6% | 0.2% | 8.50 ms |
| r442 新版，联网打开（未进排行榜） | 97.7% | 0.2% | 8.52 ms |

（r440 新版联网关闭 91.1%：那次窗口开在另一块 1920×1200 的显示器上，后台缓冲不同，不作比较，r443 重测。）

## 未完成 / 已知问题

- 启动器还没有联网的开关和选文件界面，目前只能改 black.ini 或用命令行。
- 用户测试时切到后台后画面变黑（游戏仍在出帧、能操作；那次是强制集显的测试版，窗口在另一块显示器上）；尚未查明，用户
  说先不管。
- `XePublicKeyData` 为空：需要它的签名检查都判为失败（登录与排行榜没有用到）。
- 「无法连接 XBOX LIVE」框里按 Ⓐ 会去找联网仪表板，移植版会重新启动本篇；应按 Ⓑ。
- 只测了 UDP；TCP 从没出现在线上（服务都走安全网关的 UDP 隧道），出现时日志会记 `IP protocol 6 … not relayed`。
