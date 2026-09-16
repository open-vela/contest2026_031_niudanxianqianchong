# ESP32-P4X 板载 C6 ESP-Hosted 控制面与 WLAN 数据面开发记录

记录日期：2026-09-14
适用对象：ESP32-P4X-Function-EV-Board、板载 ESP32-C6-MINI-1、`configs/smart_home`。

本文衔接[SDIO 枚举阶段记录](ESP32-P4X-C6-SDIO基础枚举阶段记录.md)，记录从 C6 的
Function 1 可访问，到 ESP-Hosted 控制 RPC、STA 启动事件和 NuttX `wlan0` 数据面实现
期间遇到的问题。本文只陈述已验证的事实；`wlan0` 的注册及管理状态切换、
STA 配置和 AP 关联均已实板验证，但尚未取得 IP 或验证业务网络收发。

## 阶段结果

已在实板上确认以下控制链路：

```text
C6 reset/启动等待
  → CMD0 → CMD5 → CMD3 → CMD7 → CMD52(CCCR)
  → Function 1 + CMD53
  → ESP-Hosted 初始化握手与持续收包
  → WifiInit → 设置 STA 模式 → WifiStart → STA_START 事件
  → WifiSetStorage(RAM) → WifiSetConfig → WifiConnect → STA_CONNECTED 事件
```

控制路径表明 P4 与运行 ESP-Hosted Slave 固件的 C6 可以交换连续协议包，且 C6 已进入
STA 启动状态；后续实板还收到 `WifiSetConfig`、`WifiConnect` 的成功响应和
`Event_StaConnected`（775）。它不等于 NuttX 已获得可用 IPv4 或业务网络连通性。

本阶段还新增了 P4 私有的 WLAN 下半部：将 ESP-Hosted Function 1 的 WLAN 负载接收
回调转换为 NuttX `netdev_lowerhalf_s`/`netpkt` 输入，将 NuttX 待发帧通过 ESP-Hosted
WLAN 包封装后经 CMD53 发送。注册成功时设备名称应为网络接口 `wlan0`，不是 `/dev/wlan0`
字符设备。

## 分层调整

| 层级 | 文件 | 本阶段职责 |
| --- | --- | --- |
| 板级 | `board/.../src/esp32p4_hosted.c` | P4X 的 GPIO54 复位、C6 启动等待、组装顺序；在确认 `STA_START` 后创建 WLAN 适配器。 |
| 芯片传输 | `chips/esp32p4/common/espressif/esp_hosted_transport.c` | ESP-Hosted 包收发、RPC 请求/响应匹配、控制事件、Function 1 的 CMD53 收发及 WLAN 回调。 |
| 芯片 WLAN 适配 | `chips/esp32p4/common/espressif/esp_hosted_wlan.c` | `netdev_lowerhalf_s`、接收队列、NuttX 网络栈投递、以太帧发送和 `wlan0` 注册。 |
| 构建配置 | P4 芯片 Kconfig、Make.defs、CMakeLists 与 `configs/smart_home/defconfig` | 选择驱动依赖、编译 WLAN 适配器、配置 IOB 缓冲和网络延后初始化。 |

板级文件不再实现 CMD0/CMD52/CMD53 时序或 ESP-Hosted 包格式；这些属于 P4 控制器及协议层。
SSID、密码、DHCP 和 Smart Home 业务也不放入此处。

## 问题、原因与处理

### 1. C6 在 CMD5、CMD3 或 CCCR 前超时

早期日志出现过 `CMD5 response-timeout`、`CMD3_rca result=-110` 和 CMD52 读取 CCCR
超时。CMD0 成功只表示 Host 命令状态机完成，不表示 C6 SDIO Slave 已对后续命令就绪。
当 `raw=0x00000100` 时，控制器报告的是响应超时，不能误判为单纯的软件轮询不足。

处理方式：补齐 SDIO 标准枚举次序 `CMD0 → CMD5 → CMD3 → CMD7 → CMD52`，使用 CMD3
返回的 RCA 发 CMD7；每条命令记录完成耗时、响应和错误寄存器快照；C6 reset 保持 10 ms，
释放后改为任务上下文等待 1000 ms 再探测。实板随后连续完成 CMD5、CMD3、CMD7 与 CCCR
读取，说明启动等待和命令序列均为必要条件。仍需进行重复复位和断电重启稳定性验证。

### 2. 板级文件承担 SDIO/协议实现，产生架构越界

早期 `esp32p4_hosted.c` 同时持有本板引脚、复位时序和 SDIO 命令/寄存器处理。它会把只与
P4 SDMMC 控制器和 C6 协议有关的逻辑锁死在一块板中，且难以独立诊断或复用。

处理方式：板级仅保留引脚表、C6 reset 和 `board_esp_hosted_initialize()` 装配；P4 SDMMC
命令、CMD53 原语移入 `esp_hosted_sdio.c`，枚举和 ESP-Hosted 包解析移入
`esp_hosted_transport.c`。这使 SDIO 超时、协议错误和板级复位问题可以从日志中区分。

### 3. 新增传输层后出现未定义符号

`esp_hosted_transport_initialize` 曾在板级对象中被引用，但新源文件没有同时进入 NuttX 的
Make 与 CMake 源文件列表，最终在链接阶段报 undefined reference。

处理方式：每新增芯片源文件都同步更新 `chips/esp32p4/common/espressif/Make.defs` 和
`CMakeLists.txt`，并让 Kconfig 的依赖关系与源文件选择一致。此类错误属于构建清单遗漏，
不能通过在板级增加临时桩函数掩盖。

### 4. ESP-Hosted 控制 RPC 需要持续收包和请求匹配

只完成 CMD53 单次读写时，C6 的 RPC 响应与异步事件可能留在 Function 1 FIFO；控制请求会
因没有持续接收路径而超时，或把不相关事件误当作响应。

处理方式：传输层建立持续收包路径，以序列号和消息类型匹配同步 RPC；异步 Wi-Fi 事件单独
分发。实板已依次验收 WifiInit、设置 STA 模式、WifiStart 及 `STA_START` 事件。该验收只
覆盖控制面，不覆盖 AP 关联、断线、重连和 DHCP。

### 5. `netpkt_queue_t rx_queue` 是不完整类型

新增 `esp_hosted_wlan.c` 后，编译报：

```text
error: field 'rx_queue' has incomplete type
```

原因不是漏掉头文件。NuttX 的 `netpkt_queue_t` 依赖 IOB 队列类型；当
`CONFIG_IOB_NCHAINS=0` 时，该队列结构不定义。原 Smart Home 配置未给网络数据面预留
IOB 链和帧缓存。

处理方式：在 `configs/smart_home/defconfig` 配置 `CONFIG_IOB_NBUFFERS=48`、
`CONFIG_IOB_BUFSIZE=256`、`CONFIG_IOB_NCHAINS=8` 以及 `CONFIG_NET_ETH_PKTSIZE=1514`。
其中 48 个 256 字节 IOB 能覆盖 4 个完整以太帧的最小接收预算；实际吞吐、丢包和内存占用
仍需实板压测后再调整。

### 6. `DRIVERS_IEEE80211` 的 Kconfig 直接依赖未满足

启用 `ESPRESSIF_HOSTED_WLAN` 后，`olddefconfig` 提示：

```text
DRIVERS_IEEE80211 ... direct dependencies DRIVERS_WIRELESS with value n
```

原因是 WLAN 选项直接 `select DRIVERS_IEEE80211`，却没有先选择它的父依赖
`DRIVERS_WIRELESS`。Kconfig 不允许由 `select` 跨越未满足的直接依赖。

处理方式：在 `ESPRESSIF_HOSTED_WLAN` 的芯片层 Kconfig 中先 `select DRIVERS_WIRELESS`，
再 `select DRIVERS_IEEE80211`，同时保留 `NET`、`NETDEVICES` 和 `NET_ETHERNET` 依赖。

### 7. 链接阶段缺少 `riscv_netinitialize`

IOB 和无线依赖补齐后，链接报：

```text
undefined reference to `riscv_netinitialize`
```

原因是 `CONFIG_NET=y` 使 RISC-V 通用启动代码调用 `riscv_netinitialize()`；该符号只适用于
需要在架构早期注册网络设备的平台。ESP-Hosted 的 C6 必须先复位、枚举、完成协议启动，
之后才由板级 bring-up 注册 `wlan0`，不具备早期初始化条件。

处理方式：在 Smart Home 配置加入 NuttX 标准选项 `CONFIG_NETDEV_LATEINIT=y`。它使 RISC-V
启动代码不要求早期 `riscv_netinitialize()`，允许网络设备在板级 bring-up 延后注册。没有
新增空的 `riscv_netinitialize()` 伪实现，以免隐藏真实的初始化顺序错误。

该修正已通过 `git diff --check` 和配置文本检查；**尚待用户重新执行完整编译及实板启动验证**。

### 8. `ifconfig wlan0 up` 写入广播地址，且启动时出现伪静态地址

实板首次执行 `ifconfig` 时显示 `10.0.0.2/10.0.0.1`，这组地址来自 NSH 的
`CONFIG_NSH_NETINIT`：它选择 `NETUTILS_NETINIT`，在启动时为名为 `wlan0` 的接口写入
NuttX 的默认静态示例地址。它不是 C6 上报的地址，也不是 DHCP 租约。

随后执行 `ifconfig wlan0 up` 后，地址变为 `255.255.255.255`，网关变为
`255.255.255.1`。原因是 NSH 的 `ifconfig` 不接受 Linux 风格的 `up` 子命令；第三个参数
被当作 IPv4 地址，`inet_addr("up")` 返回 `INADDR_NONE`，即全 1 地址，默认网关由该地址和
掩码计算得到。

处理方式：关闭 Smart Home 配置中的 `CONFIG_NSH_NETINIT`，避免在尚未关联 AP 时自动写入
静态示例地址。接口启停使用独立命令 `ifup wlan0`/`ifdown wlan0`；`ifconfig wlan0` 只用于
查看，设置静态地址时必须显式提供合法 IPv4 地址。DHCP 必须等后续 STA 关联和链路事件完成
后再启动，不能在本阶段替代 Wi-Fi 配网。

### 9. `ifup wlan0` 后读取 `/proc/net/wlan0` 返回 `-ENOMEM`

实板在接口初始为 `DOWN` 时可以读取 `ifconfig wlan0`；执行正确的启停命令
`ifup wlan0` 后，第二次读取出现：

```text
nsh: ifconfig: Could not open /proc/net/wlan0 (is procfs mounted?)
nsh: ifconfig: open failed: Out of memory
```

这不是 procfs 未挂载，也不是真实的内核堆耗尽。NSH 的第一行是任何 procfs 打开失败时的
通用提示；实板快照显示故障时 Kmem 总量为 431184 字节，空闲 426688 字节，最大连续空闲块
为 424952 字节。重复 `ifup` 不增加 Kmem 使用量，`ifdown` 也无法恢复错误，排除了 WLAN
接收队列和接口启停导致的容量泄漏。

根因位于 NuttX `net/procfs/net_procfs.c` 的设备节点查找路径。该文件作为内核编译单元时，
`strdup(relpath)` 展开为从 Kmem 分配的 `nx_strdup()`；但原代码随后调用 `lib_free(copy)`。
当前配置为 `CONFIG_BUILD_FLAT=y`，此时 `lib_free()` 展开为普通 `free()`，操作的是 Umem。
同一指针跨堆释放会破坏分配器状态，导致后续打开 `/proc/net/wlan0` 错误返回 `ENOMEM`，即使
两个堆的可用空间均充足。

处理方式：将 `netprocfs_open()` 与 `netprocfs_stat()` 中对应的两处 `lib_free(copy)` 改为
`kmm_free(copy)`，使释放器与 `nx_strdup()` 的分配域一致。该补丁只影响 ProcFS 网络设备
节点的临时路径副本，不改变 Wi-Fi 协议或数据面行为。

实板按 `ifconfig wlan0 → ifup wlan0 → ifconfig wlan0 → ifdown wlan0 → ifconfig wlan0` 验收后，
设备管理状态正确在 `DOWN → UP → DOWN` 间切换，三次读取均成功，Kmem 未出现增长。此时
`UP` 表示 NuttX 管理状态；C6 仍只有 `STA_START`，未收到 `STA_CONNECTED` 前不代表已关联 AP。

WLAN 适配器仍保留独立 `link_up` 状态：只有未来 C6 的 `STA_CONNECTED` 事件调用
`esp_hosted_wlan_set_link(true)` 后才允许 RX/TX 并打开 carrier；`STA_DISCONNECTED` 将关闭
carrier 并回收排队帧。接收队列释放采用短临界区摘取队首、锁外释放 `netpkt`；LPWORK 栈为
4096 字节。

## 当前验证边界

| 能力 | 状态 | 证据或限制 |
| --- | --- | --- |
| SDIO 基础枚举与 Function 1 | 已实板验证 | CMD0/CMD5/CMD3/CMD7/CMD52、CCCR、CMD53 已有成功日志。 |
| ESP-Hosted 初始化与控制 RPC | 已实板验证 | WifiInit、STA 模式、WifiStart 和 `STA_START` 已验收。 |
| WLAN 下半部源代码接入 | 已实板验证 | 已完成构建、注册、管理状态切换和 Kmem 稳定性验收。 |
| `wlan0` 注册 | 已实板验证 | 显示 C6 MAC；关闭 NSH 默认静态地址初始化后，初始地址保持 `0.0.0.0`。 |
| `ifup` 后 procfs/内核堆稳定性 | 已实板验证 | 修复 ProcFS 跨堆释放后，`DOWN → UP → DOWN` 和多次 `/proc/net/wlan0` 读取正常。 |
| AP 关联 | 已实板验证 | 68 字节 SetConfig 请求、WifiConnect 响应均成功，随后收到 `Event_StaConnected`（775）。 |
| carrier、DHCP、DNS、TLS | 待实现/验收 | 当前只记录 775，尚未调用 `esp_hosted_wlan_set_link(true)` 或启动 DHCP。 |


## STA 凭据与连接 RPC（已实板验收）

新增两个传输层接口：`esp_hosted_transport_set_sta_config()` 序列化
`Req_WifiSetConfig`，将 `ESP_WIFI_STA` 的 `wifi_config.sta` 分支写入 SSID 和密码字节串；
`esp_hosted_transport_wifi_connect()` 发送空请求 `Req_WifiConnect`。它们使用既有 UID、单请求互斥、
1 秒响应超时和 `remote_result` 分离机制，分别等待 `Resp_WifiSetConfig`（540）和
`Resp_WifiConnect`（538）。

板级新增两个默认空字符串的本地 Kconfig：
`ESP32P4_FUNCTION_EV_BOARD_ESP_HOSTED_STA_SSID` 与
`ESP32P4_FUNCTION_EV_BOARD_ESP_HOSTED_STA_PASSWORD`。代码不会向日志输出凭据内容，也不在
受跟踪 `defconfig` 写入默认凭据：SSID 为空时仅注册 `wlan0` 并记录连接跳过；非空时在
`STA_START`、`wlan0` 注册后依次下发配置和连接 RPC。

实板已确认两条响应日志的 `remote_result=0`，随后收到 775。为避免项目 `build.sh` 在构建成功后
执行 `savedefconfig` 回写凭据，本地测试应使用受 `.gitignore` 排除的同级
`configs/smart_home_local/defconfig`，并以 `#include "../smart_home/defconfig"` 继承正式
配置；脚本检测到 include 后不会写回该目录。NuttX 对默认空字符串不会生成对应
`CONFIG_*` 宏，板级文件为 SSID、密码各提供 `""` 的编译期回退值，因此正式 `smart_home`
配置仍可编译且会跳过连接。该机制只服务 bring-up 验证，长期凭据仍应在存储挂载后由专用服务读取。

两条响应成功只证明 C6 接受配置及发起关联；本轮的 775 进一步证明 AP 已关联。
后续仍须处理 `Event_StaConnected`/`Event_StaDisconnected`，驱动
`esp_hosted_wlan_set_link()`，再接入 DHCP。

## 下一步与验收命令

先在本地创建不受版本控制的凭据配置，再编译：

```bash
cd /home/arongw/openvela
mkdir -p contest2026_031_niudanxianqianchong/board/esp32p4/esp32p4-function-ev-board/configs/smart_home_local
cat > contest2026_031_niudanxianqianchong/board/esp32p4/esp32p4-function-ev-board/configs/smart_home_local/defconfig <<'EOF'
#include "../smart_home/defconfig"
CONFIG_ESP32P4_FUNCTION_EV_BOARD_ESP_HOSTED_STA_SSID="<ssid>"
CONFIG_ESP32P4_FUNCTION_EV_BOARD_ESP_HOSTED_STA_PASSWORD="<password>"
EOF
source myenv/bin/activate
./build.sh contest2026_031_niudanxianqianchong/board/esp32p4/esp32p4-function-ev-board/configs/smart_home_local -j2
```

本轮烧录已验收启动日志包含 C6 控制面成功、C6 STA 启动事件、
`ESP-Hosted C6 WLAN registered: wlan0`，以及以下两条均为 `remote_result=0`：

```text
INFO: ESP-Hosted C6 RPC: set_sta_config remote_result=0
INFO: ESP-Hosted C6 RPC: wifi_connect remote_result=0
```

然后执行：

```sh
nsh> ifconfig
nsh> ifup wlan0
nsh> ifconfig wlan0
```

在当前版本，虽然已经下发 SSID/密码并收到关联事件，`wlan0` 的 carrier 仍未映射，
链路关闭属于预期。只有完成关联事件到 carrier 的映射、DHCP 获取非零 IPv4、网关和 DNS 后，
才可宣称 Wi-Fi 接入完成。
