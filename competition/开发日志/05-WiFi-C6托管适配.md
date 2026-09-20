# WiFi 适配（板载 ESP32-C6 · SDIO · ESP-Hosted）

| 元信息 | 内容 |
| --- | --- |
| 适配对象 | ESP32-P4X-Function-EV-Board，板载 ESP32-C6-MINI-1 协处理器；配置 `configs/smart_home`（实板验证用本地未跟踪目录 `configs/smart_home_local`） |
| 协议与总线 | SDIO（P4 为 Host，C6 为 Slave；实测 1-bit、请求 400 kHz 按分频打印 384 kHz，Function 1 块大小 512 B）承载 ESP-Hosted 控制面 RPC 与 WLAN 数据面 |
| 当前真机状态 | 手机热点实测已通过：SDIO 枚举、ESP-Hosted 控制面（WifiInit/WifiSetConfig/WifiConnect/Event_StaConnected）、`wlan0` 注册、carrier、DHCP、IPv4/网关、DNS、`api.deepseek.com:443` TCP 建连。**2026-09-20 更新**：cAGENT TLS 路径真机全通（DeepSeek 握手/请求/响应），并在米家 Agent 工具控制闭环中稳定运行（出处见附录 6） |
| 关键代码入口 | `board/esp32p4/esp32p4-function-ev-board/src/esp32p4_hosted.c` 的 `board_esp_hosted_initialize()`（由 `src/esp32p4_bringup.c` 调用）；芯片层 `chips/esp32p4/common/espressif/esp_hosted_{sdio,transport,wlan}.c`（均已核实存在于仓库） |

## 一、适配背景与目标

ESP32-P4 不带原生 Wi-Fi/Bluetooth 射频；Function EV Board 上的 ESP32-C6-MINI-1 是官方配置的 Wi-Fi 协处理器，官方推荐 P4 作 Host、C6 作 Slave，经板上已布线的 SDIO 连接并使用 ESP-Hosted。官方 ESP-IDF 方案依赖 `esp_wifi_remote`/`esp_hosted` 组件、FreeRTOS 与 LwIP，不能通过 `CONFIG_ESPRESSIF_WIFI=y` 直接复用到 P4 NuttX（该符号只适用于具备原生 Wi-Fi 的 ESP32-C3/C6）。

因此本项目首轮实现保持 P4 私有：保留官方 C6 Slave 固件与 P4↔C6 的 SDIO/ESP-Hosted 协议，在 `chips/esp32p4` 与板级目录实现传输、协议和 `net_driver_s` 适配；在经过至少两个板级实现验证前，不新增通用 `drivers/wireless/esp_hosted` 驱动。

目标是在不改变 SC2336 摄像头、DSI 显示和板载 Ethernet 的前提下，向 NuttX 注册标准 WLAN 接口 `wlan0`，完成：

```text
扫描 AP → 配置凭据 → 关联 → DHCP → DNS → TLS → Smart Home 模型请求
```

目标是网络栈中的标准 WLAN 接口，而不是在应用中直接向 C6 发送 AT 命令。首期实板验收目标为 WPA2-PSK；WPA3、AP 模式、断线自动恢复在未验证前不宣称支持。备选的板载 Ethernet 接入另见本系列以太网文档。

板级接线（P4 侧固定）：SDMMC slot 1，CLK=GPIO18、CMD=GPIO19、D0=GPIO14、D1=GPIO15、D2=GPIO16、D3=GPIO17，GPIO54 复位 C6；C6 侧对应 CLK=19、CMD=18、D0~3=20~23，不得把 C6 GPIO 编号误写成 P4 GPIO 编号。

## 二、适配流程

按实际推进顺序，每个阶段独立验证后再进入下一阶段：

1. **接入方案选型**（`docs/开发计划/网络与连接/ESP32-P4X-SmartHome板载C6-WiFi接入方案.md`）：确定 P4 Host / C6 Slave / SDIO / ESP-Hosted 路线与 P4 私有分层（芯片控制器 → 芯片私有传输 → 板级装配 → 应用），规划 W0（官方基线）～W4（与摄像头、Ethernet 共存）分阶段验收纪律。
2. **SDIO 基础枚举**（2026-09-13 记录）：实现 CMD0 → CMD5 → CMD3 → CMD7 → CMD52 完整枚举、Function 1 启用、512 B 块大小配置与受限 CMD53 访问；实板一次启动走通全序列并经 CMD53 读到 `INT_RAW=0x00484000`。此阶段不注册 `wlan0`、不进入 DHCP。
3. **ESP-Hosted 控制面**（2026-09-14 记录）：建立持续收包路径与序列号/消息类型匹配的同步 RPC；实板依次验收 WifiInit → 设置 STA 模式 → WifiStart → `STA_START` 事件 → `wlan0` 注册（含 GetMAC）。STA 配置与关联 RPC（`WifiSetStorage(RAM)` → `WifiSetConfig` → `WifiConnect`）初期在 SetConfig 处超时，经 2026-09-16 两轮排查修复后实板收到成功响应与 `Event_StaConnected`（775）。
4. **WLAN 数据面**：将 Function 1 的 WLAN 负载接收回调转换为 NuttX `netdev_lowerhalf_s`/`netpkt` 输入，待发帧经 ESP-Hosted 封装后由 CMD53 发送；carrier 仅由 `STA_CONNECTED`/`STA_DISCONNECTED` 事件驱动。2026-09-18 手机热点复测通过 DHCP（`tx_dhcp=2/rx_dhcp=2`）、IPv4/网关、DNS 与模型端 TCP 建连。
5. **传输层缺陷修复（HTTP 发送阻塞）**：TLS 请求体产生的大以太网帧因 CMD53 字节模式 512 B 限制无法发出，日志停在 `phase=http write-body`；修复为整 512 B 块模式 + 尾部字节模式分段，并启用 `CONFIG_NET_TCP_WRITE_BUFFERS`。主机侧回归已通过，真机验收在该文档中标注为待执行。
6. **TLS/模型调用（2026-09-20 已闭环）**：手机热点会话在 TCP 建连、TLS 上下文建立后串口异常中止（`FATAL: read zero bytes from port`）；HTTP 修复文档的现象描述表明后续会话已完成 TLS 握手并推进到 HTTP body 写入阶段。**2026-09-20 米家链路七层排障闭环中，cAGENT TLS 握手/请求/响应在真机全通（DeepSeek）**，见 `docs/开发日志/应用/2026-09-20-米家链路七层排障闭环.md`（附录 6）。

## 三、关键代码与配置

### 3.1 代码位置（均已用 `find`/`ls` 核实存在）

| 层级 | 路径（仓库相对） | 职责 |
| --- | --- | --- |
| 板级 | `board/esp32p4/esp32p4-function-ev-board/src/esp32p4_hosted.c`、`include/board.h` | P4 引脚表、GPIO54 复位与 1000 ms 启动等待、`board_esp_hosted_initialize()` 装配 |
| 芯片控制器 | `chips/esp32p4/common/espressif/esp_hosted_sdio.c`、`chips/esp32p4/include/esp_hosted_sdio.h` | GPIO Matrix 路由、SDMMC 时钟/复位、CMD52/CMD53 原语、错误快照与失败回收 |
| 芯片传输 | `chips/esp32p4/common/espressif/esp_hosted_transport.c`、`chips/esp32p4/include/esp_hosted_transport.h` | SDIO 枚举、ESP-Hosted 包收发、RPC 请求/响应匹配、STA 配置与连接 RPC、WLAN 收发回调 |
| 芯片 WLAN 适配 | `chips/esp32p4/common/espressif/esp_hosted_wlan.c`、`chips/esp32p4/include/esp_hosted_wlan.h` | `netdev_lowerhalf_s`、接收队列、以太帧发送、`wlan0` 注册与 carrier 状态 |
| 主机回归 | `scripts/test_hosted_rpc.py` | 抽取生产 C 函数，宿主编译器 + UBSan 执行，独立 protobuf 解码验证 RPC 编码 |
| 主机回归 | `scripts/test_openvela_http_write.py` | 提取生产 HTTP 写入与 poll 等待函数，故障注入验证分块、短写、截止时间等 |

### 3.2 defconfig

`configs/smart_home/defconfig`（受跟踪，已逐字核实存在的关键项标 ★）：

```ini
CONFIG_ESP32P4_FUNCTION_EV_BOARD_ESP_HOSTED=y        # ★ 板级 C6/ESP-Hosted 开关
CONFIG_ESPRESSIF_HOSTED_SDIO=y                        # 经芯片层 Kconfig select 生效
CONFIG_NETDEV_LATEINIT=y                              # ★ 允许 wlan0 延后到板级 bring-up 注册
CONFIG_NET_ETH_PKTSIZE=1514                           # ★
CONFIG_IOB_BUFSIZE=256                                # ★
CONFIG_NET_TCP_WRITE_BUFFERS=y                        # ★ HTTP 阻塞修复引入
CONFIG_NET_SEND_BUFSIZE=8192                          # ★
CONFIG_SYSLOG_DEFAULT_MASK=0x7f                       # ★
CONFIG_NETDB_DNSCLIENT=y                              # ★
# 方案文档记载的网络能力集：CONFIG_NET / NET_IPv4 / NET_TCP / NET_UDP / NETUTILS_DHCPC
# 关闭 CONFIG_NSH_NETINIT，避免启动时向 wlan0 写入默认静态示例地址
```

注：IOB 预算按阶段演进——控制面阶段记载 `CONFIG_IOB_NBUFFERS=48`、`CONFIG_IOB_NCHAINS=8`，HTTP 修复时为 96/16；当前仓库 `configs/smart_home/defconfig` 已继续调整为 `CONFIG_IOB_NBUFFERS=256`、`CONFIG_IOB_NCHAINS=32`（文件中另残留一行重复的 `CONFIG_IOB_NCHAINS=16`，以 olddefconfig 实际结果为准），属后续调参，与本证据链各时点记录不冲突。

一次性实板凭据使用受 `.gitignore` 排除的 `configs/smart_home_local/defconfig`（本地已核实存在，4 行）：`#include "../smart_home/defconfig"`、关闭 `CONFIG_SMART_HOME_DEMO_OFFLINE_UI`、写入 SSID/PASSWORD（本文档不记录凭据内容）。`build.sh` 检测到 `#include` 后跳过 `savedefconfig` 回写，正式 defconfig 不含凭据；长期凭据目标为 `/data/smart_home/secrets.json`。

### 3.3 验证命令（摘自源文档）

```bash
# 宿主端 RPC/HTTP 回归（openvela 根目录）
source myenv/bin/activate
python3 contest2026_031_niudanxianqianchong/scripts/test_hosted_rpc.py
python scripts/test_openvela_http_write.py     # 项目目录内执行
git diff --check

# 构建（本地凭据配置）
./build.sh contest2026_031_niudanxianqianchong/board/esp32p4/esp32p4-function-ev-board/configs/smart_home_local -j2

# 构建后核对配置（避免输出本地凭据）
rg '^(CONFIG_NET_TCP_WRITE_BUFFERS|CONFIG_NET_SEND_BUFSIZE|CONFIG_IOB_NBUFFERS|CONFIG_IOB_NCHAINS|CONFIG_SYSLOG_DEFAULT_MASK)=' nuttx/.config

# 实板 NSH 验收（方案 W2 通过条件）
nsh> ifconfig        # wlan0 出现且 link 状态正确
nsh> ifup wlan0
nsh> renew wlan0
nsh> ifconfig wlan0  # 通过条件：非零 IPv4、网关和 DNS
```

## 四、适配证据（真机验收）

以下日志均为源文档逐字摘录，出处以仓库相对路径注明。

### 4.1 SDIO 枚举成功（2026-09-13，一次完整启动）

出处：`docs/开发日志/ESP32-P4X-C6-SDIO基础枚举阶段记录.md`

```text
INFO: ESP-Hosted C6 boot wait complete; probing SDIO
INFO: ESP-Hosted SDIO: cmd=0 arg=0x00000000 raw=0x00000004 elapsed_us=1110
INFO: ESP-Hosted SDIO: cmd=5 arg=0x00000000 raw=0x00000004 elapsed_us=900
INFO: ESP-Hosted SDIO: cmd=5 arg=0x00ff8000 raw=0x00000004 elapsed_us=900
INFO: ESP-Hosted SDIO: cmd=5 arg=0x00ff8000 raw=0x00000004 elapsed_us=890
INFO: ESP-Hosted C6 probe: stage=cmd5 R4=0xa0ffff00
INFO: ESP-Hosted SDIO: cmd=3 arg=0x00000000 raw=0x00000004 elapsed_us=890
INFO: ESP-Hosted C6 probe: stage=cmd3_rca R6=0x00010000
INFO: ESP-Hosted SDIO: cmd=7 arg=0x00010000 raw=0x00000004 elapsed_us=900
INFO: ESP-Hosted C6 probe: stage=cmd7_select R1=0x00001e00
INFO: ESP-Hosted SDIO: cmd=52 arg=0x00000000 raw=0x00000004 elapsed_us=930
INFO: ESP-Hosted C6 SDIO: slot=1 width=1 clock=384kHz revision=0x5342270a hardware=0x03c44c83
INFO: ESP-Hosted C6 probe: CMD5 R4=0xa0ffff00 RCA=0x0001 CCCR=0x32
INFO: ESP-Hosted Function 1 ready: io_ready=0x02 block_size=512
INFO: ESP-Hosted C6 CMD53 probe: function=1 address=0x050 int_raw=0x00484000
```

`io_ready=0x02` 表明从机已报告 Function 1 就绪；CMD53 读到 `INT_RAW` 四字节数据验证了命令响应与 PIO 数据相位。边界：这是单次启动日志而非完整串口归档；10 次 RESET、5 次断电重启与失败后恢复未做，不能宣称稳定性验收通过。

### 4.2 WifiSetConfig 修复后的控制面成功响应与关联事件（2026-09-16）

出处：`docs/开发日志/应用/2026-09-16-C6-WifiSetConfig超时排查与修复.md`

```text
INFO: ESP-Hosted C6 RPC: storage=RAM remote_result=0
INFO: ESP-Hosted C6 RPC: set_sta_config remote_result=0
INFO: ESP-Hosted C6 RPC: wifi_connect remote_result=0
```

```text
INFO: ESP-Hosted C6 RPC: WifiSetConfig sent uid=6 bytes=68
INFO: ESP-Hosted C6 RPC: WifiSetConfig response uid=6 result=0
INFO: ESP-Hosted C6 RPC: WifiConnect response uid=7 result=0
INFO: ESP-Hosted C6 RX: event=775
```

协议定义中 775 为 `Event_StaConnected`，证明 C6 已关联 AP；尚不证明 P4 网络接口已打开 carrier、取得 DHCP 地址或能收发业务流量。同轮固件 `nuttx.bin` 877416 字节，SHA-256 `e3c0403176dec8246032aacada75195b74f5bb1e5e1afb83490ace2858cc1de5`。

### 4.3 DHCP / DNS / TCP 证据（2026-09-18 手机热点复测）

出处：`docs/开发日志/ESP32-P4X-C6-ESP-Hosted控制面与WLAN数据面开发记录.md`

```text
WifiSetStorage response result=0
WifiSetConfig response result=0
WifiConnect response result=0
STA connected; wlan0 carrier on

DHCP data: stage=complete tx_frames=2 tx_dhcp=2 tx_errors=0
                           rx_frames=2 rx_dhcp=2 rx_dropped=0
Network: DNS verify OK
Network init-end: ip=<allocated> gateway=<gateway> online=1

phase=dns resolved host=api.deepseek.com
phase=tcp connected host=api.deepseek.com port=443
```

该组证据确认：C6 接受 STA 配置并关联；P4/C6 数据面能够收发 DHCP 报文；NuttX 取得 IPv4、默认网关并通过 DNS 解析；cAGENT 已把模型服务 TCP 三次握手走通。足以排除"ESP-Hosted WLAN 完全无法收发数据"这一结论。

同一次会话在 TCP 建连、TLS 上下文分配完成后终端报告（同上出处）：

```text
FATAL: read zero bytes from port
term_exitfunc: reset failed for dev UNKNOWN: Input/output Error
```

该输出发生在本地串口/终端会话终止路径，无 mbedTLS 握手完成、HTTP 状态或模型响应证据，不能直接定性为 TLS 失败，也不能宣称模型调用成功。

### 4.4 证据分级：枚举成功 ≠ 数据有效 ≠ 业务达标

| 层级 | 证据 | 当前结论 |
| --- | --- | --- |
| L1 总线/枚举 | 4.1 日志：CMD0/5/3/7/52、CCCR=0x32、F1 ready、CMD53 PIO 读 | 单次通过；重复复位/断电稳定性未验证 |
| L2 控制面 RPC | WifiInit、STA 模式、WifiStart、`STA_START`、Storage/SetConfig/Connect 响应 result=0、事件 775 | 已验证配置下发与 AP 关联；不等于已取得 IP |
| L3 数据面 | carrier on、DHCP `tx_dhcp=2/rx_dhcp=2`、无错误/丢弃、IPv4/网关、DNS verify OK | 手机热点实测通过；某 AP 存在 `rx_dhcp=0` 边界（见 5.6） |
| L4 传输层业务 | `phase=tcp connected host=api.deepseek.com port=443` | TCP 443 建连已实测。TLS 握手：HTTP 修复文档陈述"真机已完成 DNS、TCP 和 TLS 握手"并曾停在 `phase=http write-body`，但源文档未附握手逐字日志，方案文档（2026-09-18 状态）亦未将其标为已验收。HTTP 发送与模型响应：修复后真机复测记录（`5312/5312`、`write-complete`、HTTP 状态码、UI 回复）在所列源文档中缺失 |

## 五、遇到的问题与解决

每条按 现象 → 定位 → 根因 → 修复 → 复测 组织。

### 5.1 SDIO 枚举在 CMD5/CMD3 停滞

现象：实板曾分别停在 CMD5、CMD3，CMD5 失败快照 `raw=0x100 status=0x176`，另有 `CMD3_rca result=-110`。定位：控制器报告硬件响应超时（命令状态机等待响应起始位），不是软件轮询超时；CMD0 成功只表示 Host 命令状态机完成，不证明从机在线。根因：枚举序列早期从 CMD5 直接跳 CMD52，缺 CMD3（取 RCA）/CMD7（选卡）；且释放 C6 复位后仅忙等 100 ms，启动裕量不足。修复：补齐 `CMD0 → CMD5 → CMD3 → CMD7 → CMD52` 完整次序并检查 R6/R1/R5 错误位；复位保持 10 ms 脉冲，释放后任务休眠 1000 ms 再探测；保留清中断前寄存器快照。复测：一次完整枚举成功（4.1 日录）。边界：固定等待不代表已检测 C6 就绪，历史故障是否全部由此引起未证明。

### 5.2 分层越界与构建集成问题（合并条目）

现象与处理：早期 `esp32p4_hosted.c` 同时持有引脚、复位时序与 SDIO 命令处理——拆分为板级仅保留引脚表/复位/装配，命令时序入 `esp_hosted_sdio.c`，协议入 `esp_hosted_transport.c`；新增传输层后链接报 undefined reference——每新增芯片源文件同步维护 Make.defs/CMakeLists.txt 与 Kconfig 依赖链；`netpkt_queue_t` 报 incomplete type——`CONFIG_IOB_NCHAINS=0` 使队列结构不定义，在 defconfig 配置 IOB 缓冲与链数；`ESPRESSIF_HOSTED_WLAN` 直接 `select DRIVERS_IEEE80211` 报 direct dependencies 不满足——先 `select DRIVERS_WIRELESS` 再 `select DRIVERS_IEEE80211`；`CONFIG_NET=y` 后链接缺 `riscv_netinitialize`——C6 须先复位/枚举/协议启动才能注册 `wlan0`，加入标准选项 `CONFIG_NETDEV_LATEINIT=y`，未添加空伪实现隐藏顺序错误。其中 LATEINIT 修正当时尚待完整编译与实板验证（后续阶段日志表明已随数据面构建通过）。

### 5.3 NSH 伪静态地址与 `ifconfig wlan0 up` 写入广播地址

现象：首次 `ifconfig` 显示 `10.0.0.2/10.0.0.1`，执行 `ifconfig wlan0 up` 后地址变为 `255.255.255.255`、网关 `255.255.255.1`。定位：前者来自 NSH `CONFIG_NSH_NETINIT` 为 `wlan0` 写入的默认静态示例地址，非 C6 上报也非 DHCP 租约；后者因 NSH `ifconfig` 不接受 Linux 风格 `up` 子命令，第三参数被当 IPv4 地址，`inet_addr("up")` 返回 `INADDR_NONE`。修复：关闭 `CONFIG_NSH_NETINIT`；接口启停使用 `ifup`/`ifdown`，`ifconfig wlan0` 仅查看。复测：关闭后初始地址保持 `0.0.0.0`。

### 5.4 `ifup wlan0` 后 `/proc/net/wlan0` 返回 `-ENOMEM`

现象：`ifup wlan0` 后第二次读取报 `Could not open /proc/net/wlan0`、`Out of memory`。定位：非 procfs 未挂载、非真实堆耗尽——故障快照 Kmem 总量 431184 B、空闲 426688 B、最大连续空闲块 424952 B，重复 `ifup` 不增加 Kmem 使用。根因：NuttX `net/procfs/net_procfs.c` 设备节点查找路径中 `strdup(relpath)` 展开为从 Kmem 分配的 `nx_strdup()`，原代码却调用 `lib_free(copy)`；`CONFIG_BUILD_FLAT=y` 下 `lib_free()` 展开为操作 Umem 的普通 `free()`，同一指针跨堆释放破坏分配器状态。修复：`netprocfs_open()`/`netprocfs_stat()` 两处 `lib_free(copy)` 改为 `kmm_free(copy)`，释放域与分配域一致。复测：`ifconfig → ifup → ifconfig → ifdown → ifconfig` 设备状态正确 `DOWN → UP → DOWN` 切换，三次读取均成功，Kmem 无增长。

### 5.5 WifiSetConfig 超时（两轮排查）

**第一轮（RAM 持久化假设，被证伪）**

现象：SDIO 枚举、WifiInit、SetWifiMode、WifiStart、`STA_START`、GetMAC、`wlan0` 注册均成功，但发送 WifiSetConfig 后读 Function 1 地址 `0x50` 的 CMD53 超时，复位重试后同阶段再失败。定位：命令参数 `0x1400a004` 对应 Function 1、地址 `0x50`、字节模式读 4 字节；`raw=0x100`、`rto=1 dto=0` 是命令响应超时而非数据超时，只证明 P4 未收到响应，不能单凭它断言 C6 崩溃、密码错误或硬件损坏。第一轮修复：新增 `WifiSetStorage(RAM)`（请求 313/响应 569）隔离 NVS/Flash 持久化路径；接收线程停止后仍可安全解除回调（`rpc_lock` 保护）；凭据编码缓冲 96 → 104 字节（32 B SSID + 64 B 密码 + protobuf 字段头）。复测：RAM 设置成功但两次仍在 SetConfig 后相同 RTO——RAM 措施没有解决本次故障，不能把配置持久化认定为根因。

**第二轮（旧版协议缺 threshold/pmf_cfg 嵌套消息）**

定位与根因：对照官方 ESP-Hosted 源码，项目参考的 2.9.1 提交（`e1d7526`）对缺省 `threshold`/`pmf_cfg` 有判空；但历史提交 `65ba6934` 的 `req_wifi_set_config()` 直接执行 `p_c_sta->threshold->rssi` 与 `p_c_sta->pmf_cfg->capable`，无判空，官方 2.9.1 主机发送端也始终分配这两个对象。SetConfig 省略嵌套消息虽符合 protobuf 语法，却不满足旧版从机处理器的实际前提。修复：新增 `esp_hosted_transport_append_empty_message()`，在 STA 配置中显式发送默认嵌套消息（字段 9 `threshold`：`4a 00`；字段 10 `pmf_cfg`：`52 00`）；12 B SSID + 10 B 密码的请求由 64 字节变为 68 字节，最大凭据 STA 消息 104 字节仍在缓冲容量内。验证：新增回归用例修复前报 `STA threshold submessage is missing`、修复后通过；`scripts/test_hosted_rpc.py`（UBSan）通过；使用官方历史提交 `esp_hosted_rpc.pb-c.c/.h` 与本地 protobuf-c 1.4.1 实际解码三组配置，修复后两个指针非 NULL、成员为默认值，移除末尾四字节的负对照两指针均为 NULL。真机复测：见 4.2，SetConfig/WifiConnect 成功响应 + 事件 775，超时不再出现。限定：板载 C6 的确切固件版本无直接证据（日志 `firmware=0x00000000` 是本地解析器清零后未匹配的结果，不能从中推断版本），此前是否确实在该处崩溃亦未取得直接证据；仍需多次冷启动及复位验证。

### 5.6 某 AP `rx_dhcp=0` 的边界结论

现象：此前接入的某 AP 路径记录到 DHCP Client 报文已发送但 `rx_dhcp=0`，始终未观察到 DHCP 应答。定位：与手机热点复测的 `rx_dhcp=2` 对照，同一 P4/C6 数据面在另一 AP 上收发正常。结论（边界）：可确认该失败路径是"该 AP 的 DHCP 应答未进入当前链路"，应检查该 AP 的 DHCP 地址池、MAC/接入策略、VLAN 或热点隔离；在未取得 AP 侧证据前，不把根因归为单一的路由器配置，也不修改 C6 SDIO/WLAN 数据面来规避，更不能据此反向认定 P4↔C6 数据面不可用。

### 5.7 HTTP 发送阻塞（CMD53 字节模式 512 B 限制）

现象：真机已完成 DNS、TCP 和 TLS 握手，HTTP 请求头 227 B、请求体 5312 B，日志停在 `phase=http write-body`，界面持续 thinking；body 切成 1024 B 仍可复现。定位：`esp_hosted_transport_send_packet_locked()` 原对所有长度调用 CMD53 字节模式，`esp_hosted_transport_transfer_once()` 拒绝超过 512 B 的字节模式事务返回 `-EINVAL`，TLS 请求体产生的大以太网帧无法发出（接收方向已有块模式与尾部分段，发送方向遗漏）；TX 配额 `available=20` 只证明查询到可用配额，不证明后续 SDIO 写入成功。另原配置未启用 `NET_TCP_WRITE_BUFFERS`，本地 `tcp_send_unbuffered.c` 在发送中等待 ACK 并受 `SO_SNDTIMEO` 限制，外层 TLS poll 超时无法覆盖停留在底层 send 内部的等待；非阻塞 fcntl 调用未检查返回值。修复：RX/TX 共用 FIFO 分段函数——整 512 B 部分块模式、尾部字节模式、每次事务最多 4096 B，地址始终为 FIFO_END 减去剩余长度；整包 TX 成功后才递增发送配额计数；启用 `CONFIG_NET_TCP_WRITE_BUFFERS=y` 与 `CONFIG_NET_SEND_BUFSIZE=8192`；检查 `F_GETFL`/`F_SETFL`/`SO_SNDTIMEO` 返回值；header/body 共用写入截止时间，WANT_READ/WRITE 用 poll 重试且指针长度不变；保留 1024 B 分块与逐块进度日志。复测：主机测试通过——`scripts/test_hosted_rpc.py` 直接编译生产发送函数覆盖 1～1536 B 全部长度，检查 CMD53 写方向、模式、长度、地址与数据指针连续性；`scripts/test_openvela_http_write.py` 验证 Content-Length 完整性、5312 B 分块、短写、WANT_READ/WRITE、截止时间、零进展等。真机验收在该文档中为待执行，标准：TLS 握手成功、body 进度 `5312/5312`、出现 `write-complete` 与实际 HTTP 状态（401/429 属鉴权/限流，不等于链路未发出请求）、HTTP 200 后 UI 显示模型回复、连续多轮无 TX `-EINVAL`、断网能超时返回 UI。主机测试不替代真实 mbedTLS/NuttX TCP/SDIO 硬件集成。

### 5.8 TLS 会话异常中止（未定性）

现象：2026-09-18 手机热点会话在 `api.deepseek.com:443` TCP 连接成功、TLS 上下文分配完成后终端输出 `FATAL: read zero bytes from port` 与 `term_exitfunc: reset failed for dev UNKNOWN: Input/output Error`。定位：该输出发生在本地串口/终端会话终止路径，日志中无握手完成、HTTP 状态或模型响应证据。结论：不能直接定性为 TLS 失败，也不能宣称模型调用成功；下一轮应保留完整复位后的启动首屏与 cAGENT TLS 日志，确认设备是否重启、串口是否断开，以及失败发生在握手、HTTP 发送还是终端传输层。HTTP 修复文档的现象描述表明后续会话已推进到 TLS 握手完成并停在 HTTP body 写入（即 5.7 所处理的问题），但 TLS 握手本身仍缺逐字日志证据。

## 六、当前验收状态与边界

| 能力 | 状态 | 证据或限制 |
| --- | --- | --- |
| SDIO 基础枚举与 Function 1 | 已实板验证 | CMD0/5/3/7/52、CCCR、CMD53 成功日志（4.1）；重复复位/断电稳定性未验证 |
| ESP-Hosted 初始化与控制 RPC | 已实板验证 | WifiInit、STA 模式、WifiStart、`STA_START`、SetStorage/SetConfig/Connect、事件 775（4.2） |
| `wlan0` 注册与 procfs/堆稳定性 | 已实板验证 | 注册显示 C6 MAC、初始地址 `0.0.0.0`；ProcFS 跨堆释放修复后 `DOWN → UP → DOWN` 多次读取正常 |
| AP 关联、carrier、DHCP、IPv4、DNS | 已在手机热点实板验证 | `tx_dhcp=2/rx_dhcp=2`、无错误/丢弃、DNS verify OK（4.3）；方案 W2 通过条件满足，其他 AP 须分别复测（5.6） |
| 模型 endpoint TCP 443 建连 | 已在手机热点实板验证 | `phase=tcp connected host=api.deepseek.com port=443`（4.3） |
| TLS 握手 | 已实板验证（2026-09-20） | 米家链路排障中验证 cAGENT TLS 路径真机稳定（DeepSeek 握手/请求/响应全通），出处：附录 6 |
| HTTP 发送（修复后）、模型调用 | 已实板验证（2026-09-20） | Agent 经 LLM→miot_device_list→miot_device_control 完成摄像机真实控制，多轮对话正常（出处：附录 6）；该闭环同时覆盖 5.7 修复后的发送路径 |

尚未验证、在取得证据前不宣称的能力：多次冷启动与复位稳定性；4-bit/高速模式、CMD53 FIFO 连续收发、数据 DMA、Function 1 中断；WPA3、SoftAP、断线自动恢复；`eth0`/`wlan0` 并存时的默认路由策略（须先在网络栈明确路由策略）；C6 固件版本与构建配置（`firmware=0x00000000` 不可推断）。测试脚本为宿主端桩验证，不模拟真实 SDIO、C6 调度、Flash 行为或并发时序，不能替代真机验收。

## 附录：原始文档索引

| 序号 | 源文档（仓库相对路径） | 主要内容 |
| --- | --- | --- |
| 1 | `docs/开发计划/网络与连接/ESP32-P4X-SmartHome板载C6-WiFi接入方案.md` | 方案选型、分层设计、配置与凭据策略、W0～W4 分阶段验收纪律、风险表（状态截至 2026-09-18） |
| 2 | `docs/开发日志/ESP32-P4X-C6-SDIO基础枚举阶段记录.md` | 2026-09-13 SDIO 枚举交付、枚举日志、验证边界 |
| 3 | `docs/开发日志/ESP32-P4X-C6-ESP-Hosted控制面与WLAN数据面开发记录.md` | 2026-09-14 控制面/`wlan0` 数据面问题清单、2026-09-18 手机热点复测（DHCP/DNS/TCP） |
| 4 | `docs/开发日志/应用/2026-09-16-C6-WifiSetConfig超时排查与修复.md` | SetConfig 超时两轮排查、旧版协议嵌套消息修复、回归验证与固件指纹 |
| 5 | `docs/开发日志/ESP32-P4X-SmartHome-HTTP发送阻塞修复.md` | HTTP 发送阻塞根因（CMD53 512 B 字节模式限制）、修复内容、主机验证与待执行真机验收标准 |
| 6 | `docs/开发日志/应用/2026-09-20-米家链路七层排障闭环.md` | 米家链路七层问题闭环（2026-09-20），含 cAGENT TLS 路径真机全通与 Agent 工具控制摄像机验证 |
