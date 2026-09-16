# C6 WifiSetConfig 超时排查与修复

## 结论与验证边界

第一轮修复了回调清理和最大凭据编码容量问题，并增加 `WifiSetStorage(RAM)`。
用户随后实板验证：RAM 设置成功，两次仍在 SetConfig 后发生相同 RTO；
因此 RAM 措施没有解决这次故障，不能把配置持久化认定为根因。

第二轮发现明确的旧版协议兼容缺陷：SetConfig 省略 `threshold` 与 `pmf_cfg`
嵌套消息，而部分旧版 C6 处理器会不判空直接解引用。现已按官方主机行为补齐
默认消息，新增回归用例先失败、修复后通过。实板随后以 68 字节 SetConfig 包收到
成功响应，并收到 `Event_StaConnected`（775）；这验证了控制面配置和 AP 关联。
板载 C6 的确切固件版本和此前是否确实在该处崩溃，仍未取得直接证据。

## 日志说明了什么

最新启动日志显示：SDIO 枚举、WifiInit、SetWifiMode、WifiStart、STA_START、
GetMAC 和 `wlan0` 注册均成功。发送 WifiSetConfig 后，读取 Function 1 地址
`0x50` 的 CMD53 超时；完整复位重试后，在相同阶段再次失败。
修复前的日志包含 `storage=RAM remote_result=0`，但没有 SetConfig 响应；
修复后的实板日志则包含 SetConfig、WifiConnect 的成功响应以及关联事件。

- 命令参数 `0x1400a004` 对应 Function 1、地址 `0x50`、字节模式读取 4 字节。
- 中断状态 `raw=0x100`、`rto=1 dto=0` 是命令响应超时，不是数据超时。
- 这证明 P4 没有收到该命令的响应，不能单凭它断言 C6 崩溃、密码错误或硬件损坏。
- 第二次能够重新注册 `wlan0`，说明旧的重复注册问题与此次重复出现的总线超时要分开处理。
- 两轮失败清理会注销 `wlan0`，所以进入 NSH 后 `ifup wlan0` 失败、
  `/proc/net/wlan0` 不存在与该清理流程一致；提示中的 “is procfs mounted?”
  只是通用报错，不足以证明 procfs 未挂载。

修复后的实板关键日志为：

```text
INFO: ESP-Hosted C6 RPC: WifiSetConfig sent uid=6 bytes=68
INFO: ESP-Hosted C6 RPC: WifiSetConfig response uid=6 result=0
INFO: ESP-Hosted C6 RPC: WifiConnect response uid=7 result=0
INFO: ESP-Hosted C6 RX: event=775
```

协议定义中 775 是 `Event_StaConnected`。这证明 C6 已关联 AP；尚不证明 P4
网络接口已打开 carrier、取得 DHCP 地址或能收发业务流量。

## 协议核对及版本判断修正

第一轮按项目参考的 ESP-Hosted 2.9.1 对应提交
`e1d75268e963829fc51fb78a987e5494cd6d3e29` 核对：

1. WifiSetConfig 的请求/响应编号为 284/540；STA 配置的嵌套字段与当前编码一致。
2. 该版本对缺省的 `threshold`、`pmf_cfg` 指针有判空，但这不能排除旧版固件问题。
3. 字节串复制宏会检查长度和数据指针，缺省 SAE 字段也不能直接作为崩溃证据。

来源：[版本页面](https://components.espressif.com/components/espressif/esp_hosted/versions/2.9.1)、
[协议定义](https://github.com/espressif/esp-hosted-mcu/blob/e1d75268e963829fc51fb78a987e5494cd6d3e29/common/proto/esp_hosted_rpc.proto)、
[C6 请求处理](https://github.com/espressif/esp-hosted-mcu/blob/e1d75268e963829fc51fb78a987e5494cd6d3e29/slave/main/slave_control.c)、
[复制宏](https://github.com/espressif/esp-hosted-mcu/blob/e1d75268e963829fc51fb78a987e5494cd6d3e29/slave/main/slave_control.h)。

重要修正：2.9.1 是项目方案引用的版本，不是此次日志证实的板载版本。
日志只显示 `firmware=0x00000000`；本地解析器会先将结果清零，未解析到匹配字段时
也会保持零，不能从中推断具体版本。此前按 2.9.1 排除嵌套空指针风险的判断过强。

对照官方历史提交 `65ba69343d13a58a8a8cd8b83774de28d4b15872`，
`req_wifi_set_config()` 直接执行 `p_c_sta->threshold->rssi` 和
`p_c_sta->pmf_cfg->capable`，没有对应判空；官方 2.9.1 主机发送端也始终分配这两个
对象。省略消息虽符合 protobuf 语法，却不满足这些旧版处理器的实际前提。
来源：[旧版从机处理器](https://github.com/espressif/esp-hosted-mcu/blob/65ba69343d13a58a8a8cd8b83774de28d4b15872/slave/main/slave_control.c#L800)、
[官方主机编码](https://github.com/espressif/esp-hosted-mcu/blob/e1d75268e963829fc51fb78a987e5494cd6d3e29/host/drivers/rpc/core/rpc_req.c#L223)。

## 修改内容与理由

### 1. 设置配置前显式选择 RAM 存储

新增 `esp_hosted_transport_set_wifi_storage_ram()`：请求 313、响应 569，
请求字段 1 的值为 1，即 `WIFI_STORAGE_RAM`。复用带 UID 匹配、互斥、超时和
远端结果返回的标量 RPC 路径，并接入响应解析。

板级流程现在是：

```text
WifiInit → SetWifiMode → WifiStart → STA_START → wlan0/GetMAC
         → WifiSetStorage(RAM) → WifiSetConfig → WifiConnect
```

ESP-IDF 默认 Wi-Fi 配置存储为 Flash；NVS 写入涉及 Flash/cache 行为。
本项目 P4 每次复位都会提供凭据，因此不必每次把这些配置写入 C6 NVS。
选择 RAM 用于隔离这条持久化路径；最新实板日志证实它没有消除 RTO，因此不能
继续以“改用 RAM 即可修复”为结论。
不修改 `nvs_enable`，也不擦除 C6 NVS；RAM 选择不会删除先前保存的配置。
参考：[Espressif NVS FAQ](https://docs.espressif.com/projects/esp-faq/en/latest/software-framework/storage/nvs.html)。

### 2. 接收线程停止后仍可安全解除回调

之前接收故障使 `rx_active=false`，注销回调也会收到 `-EPIPE`。
仅忽略该错误不能保证回调已经解除。

现在对仍已初始化的 transport 允许移除 NULL 回调；新增回调仍要求数据路径正常。
移除操作与回调执行使用同一把 `rpc_lock`，使 WLAN 资源释放前不再留下消费者引用。
WLAN 层不再忽略注销错误；板级清理只在全部成功后丢弃 transport 指针并允许重试。
单独记录 ready 状态，避免残留非 NULL 指针被误当作初始化成功。

### 3. 修正最大凭据编码容量

32 字节 SSID 与 64 字节密码合计 96 字节，protobuf 字段头还需要额外空间。
原 96 字节缓冲区不足，最大组合会编码失败；提高到 104 字节。
这是独立边界问题，不能解释日志中 12 字节 SSID、10 字节密码的超时。

### 4. 显式发送默认嵌套消息，兼容旧版从机

新增 `esp_hosted_transport_append_empty_message()`，在 STA 配置中发送：

```text
字段 9  threshold：4a 00
字段 10 pmf_cfg：  52 00
```

这里的“空”表示内容使用默认值，而不是对象不存在。protobuf-c 对缺省消息保留
NULL 指针，对显式的零长度消息分配默认对象，后者能满足旧版处理器的访问前提。
原 `append_bytes()` 会省略零长度字节串，不能直接用它实现这个修复；保持原函数
行为不变，避免影响其他请求及空密码编码。

本轮不改变扫描阈值、认证策略、PMF 标志、SDIO 时钟、DMA 和重试机制。
12 字节 SSID、10 字节密码的完整请求由 64 字节变为 68 字节。
最大凭据的 STA 消息为 `32 + 64 + 4（字节串头）+ 4（嵌套消息头）=104` 字节，
仍在上一轮修正的缓冲区容量之内。

## 验证记录

- `scripts/test_hosted_rpc.py`：通过。抽取实际生产 C 函数，用宿主编译器和
  UndefinedBehaviorSanitizer 编译执行，独立解码 protobuf 验证请求。
- 覆盖：RAM/STA 模式请求格式、远端失败、发送失败、等待超时、普通/空密码、
  最大长度与超长输入、RX 故障后的回调移除；本轮增加两个默认消息的存在性、
  内容、68 字节包长与空消息编码容量边界检查。
- 修复前同一新增用例报 `STA threshold submessage is missing`，修复后通过。
- 使用上述官方历史提交的 `esp_hosted_rpc.pb-c.c/.h`，链接本地 protobuf-c
  1.4.1 运行库实际解码三组配置：修复后的两个指针非 NULL、成员均为默认值；
  移除末尾四字节作为负对照时，两个指针均为 NULL。此项也通过 UBSan 测试。
- 第二轮 `smart_home_local` 固件构建通过，生成 `nuttx/nuttx.bin`，877416 字节；
  SHA-256：`e3c0403176dec8246032aacada75195b74f5bb1e5e1afb83490ace2858cc1de5`。
  配置刷新期间需恢复 HAL 锁定提交及 mbedTLS 子模块，并通过既有
  `.p4x_hal_patches.applied` 构建目标应用项目兼容补丁；随后使用已配置的
  `make -C nuttx` 完成构建，未变更依赖版本或 Wi-Fi 凭据。
- `git diff --check`：通过。
- `checkpatch.sh`：执行过，仍报告文件头路径和既有长行/空白格式问题；
  未为本次问题批量改写已有代码，不能声称全文件风格检查通过。
- 测试不模拟真实 SDIO、C6 调度、Flash 行为或并发时序；不能替代真机验收。

在工作区根目录重现：

```bash
source myenv/bin/activate
python3 contest2026_031_niudanxianqianchong/scripts/test_hosted_rpc.py
./build.sh contest2026_031_niudanxianqianchong/board/esp32p4/esp32p4-function-ev-board/configs/smart_home_local -j2 CROSSDEV=/home/arongw/openvela/prebuilts/gcc/linux-x86_64/riscv-none-elf/bin/riscv-none-elf-
```

可选的官方解码器交叉验证：将该历史提交的 `common/proto/esp_hosted_rpc.pb-c.c`
及 `.h` 下载到同一临时目录，然后运行测试脚本的 `--decoder-dir <该目录>`。
默认测试不需要下载依赖；本次交叉验证使用 `/tmp` 中的官方文件。

使用项目现有 `riscv-none-elf` 工具链；本地配置继续继承 `smart_home`，不将凭据写入
正式 defconfig。固件中包含本地配置凭据，不应公开发布镜像或 `.config`。

## 真机验收与下一步分支

本轮实板已烧录并验证下列控制面日志：

```text
INFO: ESP-Hosted C6 RPC: storage=RAM remote_result=0
INFO: ESP-Hosted C6 RPC: set_sta_config remote_result=0
INFO: ESP-Hosted C6 RPC: wifi_connect remote_result=0
```

| 结果 | 后续判断 |
| --- | --- |
| 三条均成功且出现 775 | 已验证配置、连接请求和 AP 关联；继续验收 carrier 与数据面，不等于已取得 IP。 |
| 停在 `rpc_set_storage_ram` | 检查新 RPC 的远端结果及 C6 日志，不能归因于后续配置写 Flash。 |
| RAM 成功，仍在 SetConfig 后 RTO | 持久化假设不足；采集 C6 串口崩溃/复位信息，必要时抓取 SDIO 波形，区分从机状态与主机时序。 |
| 有 `cleanup failed` | 保留原始错误与清理错误；先处理资源回收，不能强行再次初始化。 |

已确认超时不再出现，仍需多次冷启动及复位验证。下一项实现为处理
STA_CONNECTED/STA_DISCONNECTED（775/776）并驱动 `esp_hosted_wlan_set_link()` 的
carrier 状态，再接入 DHCP。当前不能把 `wlan0` 注册成功、WifiConnect 返回成功或
收到 775 等同于 Wi-Fi 已联网。

第二轮先确认相同凭据下出现 `WifiSetConfig sent uid=6 bytes=68`，避免误烧旧包。
本轮已出现 SetConfig 响应，因此该请求阶段已经通过。在没有 C6 调试串口时，
若以后出现新 RTO，可增加 P4 侧只读版本查询和限界故障诊断，但不要盲目重放
可能已被从机执行的写请求。
