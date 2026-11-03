# ESP32-P4X 板载 C6 蓝牙/BLE 适配方案

状态：方案阶段，尚未实现 Hosted-HCI 与实板 BLE 验收。

## 1. 目标

通过板载 ESP32-C6 提供 BLE 广播、扫描、单连接及 GATT 读写/通知能力，与现有
Wi-Fi 共用 SDIO。P4 运行 BLE Host，C6 运行 Controller 和射频。
C6 不支持经典蓝牙，本期不包含 A2DP、经典 SPP、Mesh 和 Matter/Thread。

## 2. 现状与前置条件

- 已有 `esp_hosted_sdio.c`、`esp_hosted_transport.c`、`esp_hosted_wlan.c` 和 C6 板级装配。
- 当前接收分发只处理 STA 与串行 RPC，未接入 HCI；已有 Wi-Fi 闭环不能证明 BLE 可用。
- 工作区提供 NuttX `bt_driver_s` 和 NimBLE；新增 Hosted-HCI 驱动接入该接口，
  使用已有 NimBLE Host，不移植 ESP-IDF Host 框架。
- 固定 C6 ESP-Hosted 固件的源码版本、ESP-IDF 版本、配置和镜像校验值，确认启用
  BLE Controller 与 SDIO Hosted-HCI。先核对能力、包格式及 Controller 启停协议，
  不套用最新版文档中的 RPC 编号或配置名；固件升级后重新验收 Wi-Fi。

## 3. 架构

```text
BLE 测试程序 → NimBLE → NuttX bt_driver_s → Hosted-HCI ─┐
                                                     ├→ ESP-Hosted → SDIO → C6
NuttX 网络栈 → wlan0 ─────────────────────────────────┘
```

- 复用现有 C6 复位、SDIO 初始化和收包线程，BLE 不创建第二个 SDIO 实例。
- 按已固定协议新增 HCI 分发与发送接口，映射 Command、Event、ACL 类型，校验
  长度、类型、序号及校验和；明确 HCI 类型字段的位置，避免重复添加 H4 类型字节。
- `bt_driver_s` 实现 `open/send/close`，接收通过已注册的 `receive` 回调上送。
  Host 不持有底层接收缓冲；先复制到有界队列，再由独立 worker 上送。
- SDIO/RPC 锁内不调用 BLE Host。发送统一串行化，限制队列与单轮发送量，
  避免 ACL 流量阻塞 Wi-Fi、RPC 和 HCI 命令完成事件。
- 保留 HCI 命令及 ACL credit 流控；队列满返回明确错误。关键事件丢失或链路失步时
  进入恢复流程，不静默继续运行。
- BLE 关闭仅停用 Controller 和自身队列，不复位 C6、不释放 Wi-Fi 正在使用的传输。
  必须复位 C6 时同时通知 Wi-Fi 与 BLE，清理旧队列和待完成请求，再重建两条链路。

## 4. 改动范围

以下路径相对项目根目录，新增文件名为规划名称。

| 位置 | 工作内容 |
| --- | --- |
| `chips/esp32p4/common/espressif/esp_hosted_transport.c` 及对应头文件 | HCI 接口分发、Controller 生命周期、收发与能力检查 |
| `chips/esp32p4/common/espressif/esp_hosted_bt.c` 及对应头文件 | `bt_driver_s` 适配、类型转换、队列、回调与关闭清理 |
| `board/esp32p4/esp32p4-function-ev-board/src/esp32p4_hosted.c` | 分开装配传输、WLAN、BLE；不把 BLE 注册绑定到 AP 关联成功 |
| 芯片与板级 Kconfig、Make/CMake、bring-up | 新增 Hosted-BLE 开关；不解除原生 `ESPRESSIF_BLE` 对 P4 的限制 |
| 板级 `configs/ble/defconfig`、`app/ble_probe/` | 独立 HCI/BLE 验证；启用 `DRIVERS_BLUETOOTH`、NimBLE 及所需角色 |
| C6 固件构建说明与配置 | 保存匹配的版本、配置、镜像校验值及原 Wi-Fi 固件恢复方式 |

NuttX 接口参考工作区 `nuttx/include/nuttx/wireless/bluetooth/bt_driver.h` 和
`nuttx/drivers/wireless/bluetooth/`；Host 采用 `apps/wireless/bluetooth/nimble/`。

## 5. 实施与验收

| 阶段 | 工作 | 通过条件 |
| --- | --- | --- |
| B0 | 固定 C6 固件及协议，验证 NimBLE 最小构建 | Controller 可启用；记录 HCI 类型、能力及版本；Wi-Fi 回归通过 |
| B1 | 独立 HCI 探针，先不启动 NimBLE | Reset、Read Local Version、LE Read Local Supported Features 获得匹配响应，无超时 |
| B2 | 注册驱动，接入 NimBLE 广播与扫描 | 手机可发现 P4 广播；P4 可识别指定 BLE 测试设备；启停 20 次无泄漏或卡死 |
| B3 | 验证单连接 GATT | 手机连接后读写测试特征并接收递增序号通知，连续 10 分钟正确；断开重连 20 次通过 |
| B4 | Wi-Fi、BLE、microSD 共存及故障恢复 | 网络持续收发、BLE 通知、SD 文件校验并发 30 分钟；无数据损坏或持续内存增长 |

B4 还需覆盖 HCI 超时、队列满、C6 复位和 BLE 单独关闭。恢复后可重新连接，旧请求
以错误结束，Wi-Fi 不因正常 BLE 启停而掉线。microSD 未完成时先执行 Wi-Fi/BLE
共存，三者组合保留为后续必验项。

## 6. 依据

- [ESP-Hosted 蓝牙架构与 Hosted-HCI](https://github.com/espressif/esp-hosted-mcu/blob/main/docs/features/bluetooth.md)
- [ESP-Hosted 传输头定义](https://github.com/espressif/esp-hosted-mcu/blob/main/common/esp_hosted_header.h)
