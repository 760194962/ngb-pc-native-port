# 09 联网：用自己的 Insignia 账号进排行榜（2026-10-23）

目标（用户原话）：「我只希望我的游戏能以自己的 signia 服务器账号进入排行榜，查看排名」。不做多人对战。

结论：**已实现并由用户亲测成功**（2026-10-23，`run\user_online`）：游戏里的排行榜列出用户自己的玩家代号，登录
Insignia 后显示真实的中忍榜单，用户的名次也在其中。

## 怎么打开

默认关闭，不打开时游戏和以前完全一样（看不到网络，排行榜报无法连接）。需要玩家自己已经在 xemu 里注册过 Insignia
的那一套文件。启动器的「联网」页（下文「界面」）写的就是这几行：

```
# black.ini（路径可以有空格，UTF-8）
online = 1
online_eeprom = E:\games\emu\xemu\xemu\Component\4- Eemprom\eeprom.bin
online_hdd = E:\games\emu\xemu\xemu\Component\3- Xbox HDD\xbox_hdd.qcow2
online_dns_mode = insignia     # 默认：所有 DNS 查询转给 online_dns；insigniadns：转给 online_insigniadns
# online_insigniadns = 192.168.2.8   （insigniadns 时：开着 InsigniaDNS 的那台电脑）
# online_dns = 46.101.64.175   （可选：Insignia 的 DNS）
```

命令行：
- `--online-eeprom <文件> --online-hdd <文件>`：同时打开联网；`--offline`：关闭。
- `--online-dns-mode insignia|insigniadns|console`：DNS 查询方式；`--online-insigniadns <IP>`：InsigniaDNS 的地址。
- `--online-check <输出文件>`：检查两个文件后退出，给启动器用。

注意：

- 两个文件只读，不写回；硬盘镜像可以是 xemu 的 qcow2（未压缩簇）或原始镜像。
- 同一台「主机」不能同时登录两次：启动时发现 `xemu.exe` 在运行就不开联网（日志 `online: off (xemu is running…)`）。
- DNS：默认 `online_dns_mode = insignia`，游戏发出的所有 DNS 查询（不论问哪个地址）都转给 `online_dns`，玩家不用开
  InsigniaDNS。`insigniadns` 时所有查询都转给 `online_insigniadns`（开着 InsigniaDNS.exe 的电脑），地址不是 IPv4 时改用
  Insignia 的并提示。`console`（第一版启动器写的，界面已不再提供）和 xemu 一样：主机网络设定里是手动 DNS 时（注册时常见：
  主 DNS 是跑 InsigniaDNS 的那台电脑，次 DNS 1.1.1.1），游戏就去问这些地址；只有发往 DHCP 给的 10.0.2.3 的才转发。
  `console` 的毛病是地址写死在主机设定里：换了电脑或路由器重新分配地址，那台 InsigniaDNS 就找不到了，只能回 xemu 改。
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

## 界面（2026-10-23 第二步）

用户：「让用户可以在界面上操作联网开关，网络必要的设置和登录等等必要信息」。方案经用户确认：
- DNS 默认「直接用 Insignia DNS」；
- 不自动读 xemu.toml，只用「选择文件」；
- 游戏里提示失败原因与「已连接」，Ⓐ 也当取消；
- 显示玩家代号。

| 部分 | 文件 | 说明 |
|---|---|---|
| 启动器「联网」页 | `launcher/src/main.cpp`、`strings.*` | 开关；EEPROM / 硬盘镜像选文件；「主机」行显示检查结果（身份有效、已注册、账号）；DNS 卡：主机设定（自动 / 手动及地址）、查询方式（直接用 Insignia DNS / 用 InsigniaDNS，后者多一行地址框与「找本机」；最初是「按主机设定」，见下文「InsigniaDNS 地址」）、「检查网络」；登录说明与 Insignia 官网链接；两个「?」帮助；保存 / 放弃 / 恢复默认与其他设定页相同 |
| 检查 | `net/online.cpp` `online_check` | 启动器以 `--online-check` 调游戏程序（隐藏窗口），复用同一套解密与 qcow2 读取；输出只有：EEPROM ok / invalid / unreadable、硬盘 ok / 原因、registered、account（扇区 10–31 偏移 24 的玩家代号）、dns auto / manual 与两个地址、xemu 是否在运行。不含密钥、序列号、MAC |
| 主机 DNS 设定 | 同上 | 配置区第 8 扇区：偏移 76 的标志位 8 = 手动 DNS，96 / 100 两个 DNS（用户的盘：标志 8、192.168.2.8 / 1.1.1.1，与 InsigniaDNS 窗口里的主机设定一致）。标志位 8 的含义只由这一块盘推断，还没有拿自动 DNS 的盘对照过 |
| 检查网络 | `launcher/src/main.cpp` `net_check` | 在单独线程里按当前查询方式向那台 DNS 问 `AS.XBOXLIVE.COM`，3 秒超时：答出地址＝正常；198.18.0.0/15＝代理 TUN；无回应＝网络或 InsigniaDNS |
| 游戏里的提示 | `d3d/perfhud.cpp` `notice`、`net/vnet.cpp`、`ngb/d3d_ngb.cpp` | 画面右下角一行字，8 秒，每种一局一次：DNS 回答 198.18.x.x、DNS 4 秒无回应、安全网关（3074）第一次回包＝「已连接 Insignia」、`online = 1` 但没开成（xemu 在运行、文件无效…）在进排行榜时提示。与性能叠加层共用绘制，没有提示时每帧只多一次原子读 |
| Ⓐ 当取消 | `ngb_livedash.cpp` | 排行榜三个对话框处理函数 FUN_002FFAB0 / FUN_003023B0 / FUN_00302560（读 0xD219A8 + 端口×0x78 的按键：0x110 Ⓐ、0x220 Ⓑ）里，Ⓐ 去仪表板的 6 段代码（push 0x40 / push 5 / call 音效 → XLaunchNewImageA(NULL)，原因 6 网络设定、7 新账号、8 修复账号）开头改为 jmp 到同一框的 Ⓑ 代码；只有 Ⓐ 的那个框（case 4）跳到各框共用的关闭代码。改之前核对 6 处字节，不符就不改 |

证据：
- **检查模式：** 用户的文件 → `eeprom = ok`、`registered = 1`、`account = <用户的玩家代号>`、`dns = manual 192.168.2.8 / 1.1.1.1`；错误文件 → `eeprom = invalid`、`hdd = cannot read the file`。
- **r446：** DNS 指向不可达地址 10.255.255.1，但本机 Clash TUN 开着，查询仍被截走、回 198.18.0.252。提示「DNS 被代理软件截走」出现（日志 `shown to the player`），游戏弹出「此玩家代号无法使用。按Ⓐ修复…」；探针显示这时运行的是 FUN_00302560。
- **r447：** 同一个框按 Ⓐ 后回到玩家代号列表（截图 r26），没有重启。`--rawvideo` 截到右下角的提示，中文正常。
- **r448：** `--online-dns-mode console`，经本机 InsigniaDNS（192.168.2.8）登录，排行榜显示用户自己的成绩，日志 `shown to the player: 联网：已连接 Insignia`。
- **启动器：** `--snap` 中英文截图正常。「检查网络」实测：
  - 「直接用 Insignia DNS」：本机 TUN 开着，回答 198.18.0.252，判为被截走；
  - 「按主机设定」：192.168.2.8 答 49.13.57.101，正常。局域网地址不经 TUN。
- **性能（铁律，集显第 1 章 bench）：**

  | 运行 | 游戏速度 | 超时帧 | 平均帧时间 |
  |---|---|---|---|
  | r449 新版 | 97.7% | 0.2% | 8.48 ms |
  | r450 上一版 | 97.7% | 0.2% | 8.32 ms |

  平均帧时间之差在此前 8.31–8.52 ms 的波动范围内。

待用户实测：TUN 关闭时「直接用 Insignia DNS」能否登录。这时 InsigniaDNS 可以关掉。

## InsigniaDNS 地址（2026-10-23）

用户问「换个电脑第二种就用不成了」：对，「按主机设定」问的是主机设定里写死的 192.168.2.8。改为：

- 第二种方式改名「用 InsigniaDNS」（`online_dns_mode = insigniadns`），所有查询转给启动器里填的地址
  （`online_insigniadns`），不再看主机设定。
- 启动器 DNS 卡多一行「InsigniaDNS 地址」：
  - 地址框只收数字和点，Enter 或点别处确认，Esc 取消，Ctrl+V 可粘贴；不是 IPv4 时不保存并提示格式。
  - 「找本机」：先找本机 UDP 53 端口上的程序（`GetExtendedUdpTable`），再退到有网关的 Wi-Fi / 以太网地址。代理的 TUN
    网卡不是这两类，不会被选中。
  - 第一次切到这种方式且地址为空时自动找一次。
- 用户这台电脑：InsigniaDNS 只监听 `192.168.2.8:53`（`Get-NetUDPEndpoint`），不监听 127.0.0.1，所以不能用本机回环地址。

证据：
- **启动器：** 切到「用 InsigniaDNS」后自动填上 192.168.2.8，显示「✓ 找到本机正在运行的 InsigniaDNS」。「检查网络」：
  192.168.2.8 答 49.13.57.101。输入 `1.2.3` 按 Enter，提示格式不对，原地址保留。输入 `10.0.0.5`：这台电脑 Clash TUN
  开着，回答 198.18.0.252，判为被截走。保存后 `black.ini` 为 `online_dns_mode = insigniadns`、
  `online_insigniadns = 192.168.2.8`；游戏程序读回，`--online-check` 输出 `insigniadns = 192.168.2.8`，退出时重写文件后两行仍在。
- **r453：** Clash TUN 开着（这时「直接用 Insignia DNS」的检查答 198.18.0.252），`--online-dns-mode insigniadns
  --online-insigniadns 192.168.2.8`：`DNS query AS.XBOXLIVE.COM to 192.168.2.8` → 49.13.57.101，Kerberos 88 与安全网关 3074
  都有回包，提示「联网：已连接 Insignia」，排行榜显示成绩。
- **r455：** 地址 `1.2.3`：日志 `is not an IPv4 address: DNS goes to Insignia's`，转发到 46.101.64.175，提示「InsigniaDNS 的地址无效」。
- 性能：只改了联网启动时选 DNS 地址与启动器，不在每帧的路径上；联网关闭时不执行。

## 未完成 / 已知问题

- 用户测试时切到后台后画面变黑（游戏仍在出帧、能操作；那次是强制集显的测试版，窗口在另一块显示器上）；尚未查明，用户
  说先不管。
- `XePublicKeyData` 为空：需要它的签名检查都判为失败（登录与排行榜没有用到）。
- 只测了 UDP；TCP 从没出现在线上（服务都走安全网关的 UDP 隧道），出现时日志会记 `IP protocol 6 … not relayed`。
