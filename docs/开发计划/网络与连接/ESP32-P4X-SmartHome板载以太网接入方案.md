# ESP32-P4X Smart Home 板载以太网接入方案

> 状态：方案阶段，尚未在“Smart Home + 摄像头 + 以太网”组合固件上完成实板验证。
>
> 适用对象：ESP32-P4 Function EV Board、板载 RJ45、Smart Home LVGL Demo。

## 1. 目标

为 P4X 的 `smart_home` 配置接入板载以太网，使应用在启动时使用
`eth0` 完成链路拉起、DHCP、DNS 解析和云端模型请求。该阶段以有线网络作为
摄像头预览、运动检测和模型 Tool 联调的网络底座，不依赖板载 ESP32-C6 的
Wi-Fi 协处理器。

完成后的路径如下：

```mermaid
flowchart LR
  RJ45[RJ45 网线] --> PHY[外置 Ethernet PHY]
  PHY --> EMAC[ESP32-P4 内部 EMAC]
  EMAC --> ETH0[ NuttX eth0 ]
  ETH0 --> DHCP[DHCP / DNS]
  DHCP --> APP[smart_home 网络模块]
  APP --> LLM[云端模型服务]
  Camera[/dev/video0] --> Vision[本地视觉服务]
  Vision --> APP
```

本期验收的是 P4X 板端到云端模型的 IPv4 网络闭环。摄像头数据不通过网络传输，
视觉 Tool 只向模型返回受限的结构化结果。

## 2. 现有基础与结论

项目已有 EMAC 到 `eth0` 的驱动和板级装配，无需为 Smart Home 新增以太网
硬件驱动：

| 层级 | 现有实现 | 在本方案中的作用 |
| --- | --- | --- |
| 芯片层 | `chips/esp32p4/common/espressif/esp_emac.c` | 初始化 ESP-IDF Ethernet MAC/PHY 并注册 NuttX 网卡 |
| 板级层 | `src/esp32p4_ethernet.c` 的 `board_emac_init()` | 先初始化高精度定时器，再调用 `esp_emac_init()` |
| bring-up | `src/esp32p4_bringup.c` | 在 `CONFIG_ESPRESSIF_EMAC` 下调用 `board_emac_init()` |
| 配置基线 | `configs/webpanel/defconfig` | 已包含 EMAC、PHY、DHCP、DNS 和 NSH 网络诊断配置 |
| 应用层 | `demos/smart_home/src/net/smart_home_network.c` | 非 Wi-Fi 平台已选择 `eth0`，并依次调用 `netlib_ifup()`、`netlib_obtain_ipv4addr()` 与 DNS 探测 |

当前 `configs/smart_home/defconfig` 仍是离线 UI 配置：
`CONFIG_SMART_HOME_DEMO_OFFLINE_UI=y` 会编译
`smart_home_network_offline.c`，不会执行上述 `eth0` 初始化流程。因此只打开
`CONFIG_ESPRESSIF_EMAC` 还不足以使 Smart Home 联网。

ESP32-P4 不具备片内 Wi-Fi；本期不启用 `wlan0`、WAPI 或 `CONFIG_ESPRESSIF_WIFI`。
后者在当前 NuttX Kconfig 中依赖 ESP32-C3/C6，不适用于 P4。板载 C6 的
ESP-Hosted 接入应作为独立后续任务。

## 3. 范围和边界

本期包含：

- P4X `smart_home` 配置启用内部 EMAC、IPv4、TCP/UDP、DHCP、DNS 和 TLS；
- 启动时自动将 `eth0` 拉起并获取 DHCP 地址；
- 保留 NSH 的 `ifconfig`、`ifup`/`ifdown`、`renew`、`ping` 用于排障；
- Smart Home UI 显示已有的网络状态，模型请求使用实际网络状态；
- 在资源分区加载模型 endpoint 和密钥配置后验证一次云端 Tool 调用。

本期不包含：

- ESP32-C6、ESP-Hosted、Wi-Fi、WAPI 或 AP 配网；
- 摄像头预览、JPEG 编码、图像上传或云端视觉模型；
- 网络断开后的复杂重连策略、mDNS 服务、Web 面板或 Node gateway；
- 更改 `esp_emac.c`、PHY 寄存器参数或 RJ45 硬件设计。只有 EMAC 基础验收失败时，
  才以首个实际错误为依据进入这些层次。

## 4. 设计

### 4.1 启动和所有权

`esp_bringup()` 已按以下顺序执行：

```text
board_emac_init()
  → esp_hr_timer_init()
  → esp_emac_init()
  → NuttX 注册 eth0
  → DSI framebuffer、触摸、摄像头等后续板级初始化
```

Smart Home 不直接调用 `esp_emac_init()`，也不管理 PHY。应用只通过网络公共接口
操作已经注册的 `eth0`：

```text
smart_home_main()
  → smart_home_network_init()
  → netlib_ifup("eth0")
  → netlib_obtain_ipv4addr("eth0")
  → getaddrinfo(模型服务域名)
  → cAGENT TLS / HTTP 请求
```

网络初始化失败时，应用应保留 LVGL 本地 UI 和摄像头本地功能，并把
`ifup`、DHCP 或 DNS 的失败状态显示在设置页；不得将无 IP 或 DNS 失败伪报为
模型可用。

### 4.2 DHCP 策略

选择由 `smart_home_network.c` 在应用启动时显式执行 DHCP，而不启用
`NETINIT_THREAD` / `NETINIT_DHCPC` 的第二套自动 DHCP 流程。这样 `eth0` 的
状态、日志和 UI 状态由 Smart Home 的单一路径维护，也避免两个启动组件并发申请
租约。

NSH 的 `renew eth0` 只是人工排障命令，不参与应用的正常启动流程。

### 4.3 Wi-Fi 源码边界

去掉 `SMART_HOME_DEMO_OFFLINE_UI` 后，`demos/smart_home/CMakeLists.txt` 当前会
同时加入 `smart_home_network.c` 与 `smart_home_wifi.c`。P4 使用以太网却不具备
WAPI/`wlan0`，因此应调整构建条件：

- `smart_home_network.c` 始终在联网配置中编译；
- `smart_home_wifi.c` 只在明确具备 Wi-Fi 的配置中编译；
- `smart_home_network.c` 对 `smart_home_wifi.h`、
  `smart_home_network_init_wifi()` 的引用以相同配置条件包裹；
- P4 分支仅保留 `eth0` 路径。

这保持应用层与硬件能力一致，避免为了编译 P4 Ethernet 而启用不适用的 WAPI。

## 5. 配置方案

以 `configs/smart_home/defconfig` 为目标，参考 `configs/webpanel/defconfig`，
通过 `menuconfig` / `savedefconfig` 生成最终配置。以下是需要的最小能力集合；
具体的派生符号以最终 `nuttx/.config` 为准。

```ini
# 板载 EMAC 与常用诊断
CONFIG_ESPRESSIF_EMAC=y
CONFIG_ESPRESSIF_ETH_DMA_BUFFER_SIZE=512
CONFIG_NETDEV_LATEINIT=y
CONFIG_NETDEV_PHY_IOCTL=y

# IPv4 网络与模型 HTTP 所需传输
CONFIG_NET=y
CONFIG_NET_IPv4=y
CONFIG_NET_TCP=y
CONFIG_NET_UDP=y
CONFIG_NET_ETH_PKTSIZE=1514
CONFIG_NETDB_DNSCLIENT=y
CONFIG_NETUTILS_DHCPC=y

# NSH 人工诊断命令
CONFIG_FS_PROCFS=y
# CONFIG_FS_PROCFS_EXCLUDE_NET is not set
# CONFIG_NSH_DISABLE_IFCONFIG is not set
# CONFIG_NSH_DISABLE_IFUPDOWN is not set
CONFIG_SYSTEM_DHCPC_RENEW=y
CONFIG_SYSTEM_PING=y

# 使用实际网络模块和 TLS；关闭离线替身
# CONFIG_SMART_HOME_DEMO_OFFLINE_UI is not set
```

保留 Smart Home 已有的 DSI framebuffer、GT911、PSRAM、LittleFS、LVGL、
SC2336 摄像头开关。摄像头的三项板级配置与 EMAC 相互独立：两者分别注册
`/dev/video0` 与 `eth0`。

不要从 `webpanel` 整份复制配置。它包含 Python、WebSocket、mDNS、Web 面板、
SmartFS 和不同的存储布局，会额外消耗 Flash、PSRAM 与任务栈，且会破坏
Smart Home 的 `/data` LittleFS 资源分区布局。

## 6. 修改清单

| 文件 | 动作 | 内容 |
| --- | --- | --- |
| `board/.../configs/smart_home/defconfig` | 修改 | 合入最小 EMAC、IPv4、DHCP、DNS、NSH 网络诊断配置，关闭离线 UI。 |
| `demos/smart_home/CMakeLists.txt` | 修改 | 将 Wi-Fi 辅助源码限制在 Wi-Fi 平台；P4 Ethernet 仅编译通用网络模块。 |
| `demos/smart_home/src/net/smart_home_network.c` | 修改 | 用配置条件隔离 Wi-Fi 专有包含和函数，保持 P4 `eth0` 路径。 |
| `demos/smart_home/Kconfig` | 视实现修改 | 若现有芯片宏不足以表达“是否编译 Wi-Fi 辅助层”，增加一个有明确依赖的内部配置符号。 |
| `board/.../src/esp32p4_ethernet.c` | 不修改 | 已由 `CONFIG_ESPRESSIF_EMAC` 接入。仅在 EMAC 实板失败时再定位。 |
| `chips/esp32p4/common/espressif/esp_emac.c` | 不修改 | 复用现有芯片层驱动；应用联网不是修改 EMAC 的理由。 |

## 7. 实施和验收步骤

### P1：独立 EMAC 基线

1. 从 `smart_home` 配置派生最小 EMAC 网络配置，编译并烧录。
2. 接入 DHCP 网络的 RJ45 网线，观察启动日志中 `board_emac_init()` 是否成功。
3. 在 NSH 依次执行：

```text
nsh> ifconfig
nsh> ifup eth0
nsh> renew eth0
nsh> ifconfig eth0
nsh> ping <网关IPv4地址>
nsh> ping <公网IPv4地址>
```

验收：`eth0` 获得非零 IPv4、网关和 DNS；网关与公网 IP 连通。

### P2：DNS 与 TLS 基线

1. 在 `smart_home` 非离线配置启动前，确认 LittleFS 中存在
   `/data/res/skills/` 和模型 endpoint 配置。
2. 通过 `smart_home` 的网络日志确认：

```text
[smart_home_net] init-end if=eth0 ip=<IPv4> gateway=<IPv4> ... online=1
```

3. 在不打印密钥的前提下，发起一次只读 Tool 请求，例如查询家庭状态或天气。

验收：DNS 探测成功，TLS/HTTP 请求获得模型响应，UI 不因网络初始化而阻塞。

### P3：摄像头共存

1. 保持网线连接，确认 `/dev/video0` 存在。
2. 运行 `video_test 300`，复核连续帧与 30fps 验收仍通过。
3. 启动 Smart Home，复核 `eth0` 地址仍存在，并执行一次本地 Tool 或模型请求。

验收：网络、DSI/LVGL、摄像头三者可共同启动；任一失败日志能够明确归属到
EMAC、DHCP/DNS、显示或 CSI，而非笼统报告“Smart Home 失败”。

## 8. 失败定位

| 现象 | 优先检查 | 不应直接采取的动作 |
| --- | --- | --- |
| 启动没有 `eth0` | `CONFIG_ESPRESSIF_EMAC`、`board_emac_init()` 返回值、网线与 PHY 链路 | 打开 WAPI 或修改摄像头驱动 |
| `ifup` 命令缺失 | `FS_PROCFS`、`NSH_DISABLE_IFUPDOWN` | 仅增加 DHCP 库 |
| `renew` 命令缺失 | `SYSTEM_DHCPC_RENEW` 与 UDP/DHCPC 依赖 | 手工写静态地址掩盖配置问题 |
| `eth0` 无 IPv4 | DHCP 服务器、交换机 VLAN、网线、`netlib_obtain_ipv4addr()` 日志 | 把问题归因于模型 API Key |
| 有 IPv4 但 `online=0` | 默认路由、DNS server、域名解析、TLS 时间/证书 | 重复初始化 EMAC |
| 网络正常但模型失败 | `/data` 的 endpoint/密钥、TLS、HTTP 状态码 | 修改 PHY 或 CSI |
| 摄像头开启后网络异常 | PSRAM/堆余量、任务栈和 DMA 资源 | 先修改 EMAC 寄存器参数 |

## 9. 提交与证据

配置、跨平台源码条件和文档作为一个可构建的功能单元提交。提交前至少需要：

1. `smart_home` 配置编译通过；
2. 最终 `.config` 中确认 EMAC、DHCP、DNS、非离线 Smart Home 和 SC2336 配置；
3. 实板日志证明 `eth0` 获得 IPv4，且 DNS 探测成功；
4. 如声明云端模型可用，还需要一条脱敏的实际模型 Tool 调用日志；
5. 如同时启用摄像头，需要附 `video_test 300` 的复测结果。

未通过上述验证的探索性变更保留在工作区继续定位，不拆分为“尝试修复”提交。
