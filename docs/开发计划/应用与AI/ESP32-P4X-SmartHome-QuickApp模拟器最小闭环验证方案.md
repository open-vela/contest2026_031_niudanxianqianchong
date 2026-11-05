# ESP32-P4X SmartHome QuickApp 模拟器最小闭环验证方案

> 状态：实施前方案。本文定义 Goldfish 模拟器上的 P0.0 验证，不代表 P4X 真机、C6
> Wi-Fi 或摄像头已适配完成。
>
> 目标：先在 openvela Goldfish 上证明 QuickApp、原生 Feature 和 SmartHome 状态服务可
> 真实协作，再将同一套 API 契约迁移至 P4X `smart_home_quickapp`。

相关文档：

- [SmartHome 快应用 UI 与原生服务分层方案](./ESP32-P4X-SmartHome快应用UI与原生服务分层方案.md)
- [SmartHome Demo 测试开发计划](ESP32-P4X-SmartHome-Demo测试开发计划.md)
- [QuickApp 开发指南](../../../../docs/zh-cn/contest_2026/quickapp/quickapp_manual.md)

---

## 1. 验证结论与边界

本阶段要验证的“最小真实闭环”是：QuickApp 页面不是在 JS 内自行伪造设备状态，而是通过
`system.smarthome` Feature 调用原生 SmartHome 服务；原生服务拥有状态、处理命令并在控制
Promise 中返回最终确认的状态。

```text
QuickApp Home 页面
  → system.smarthome Feature
  → QuickApp IPC Client（requestId 关联）
  → POSIX message queue / 原生服务通道
  → SmartHome Service / Simulator Device Provider
  → State Store（revision 唯一递增）
  → IPC response
  → controlDevice() 的确认结果 / getSnapshot()
  → QuickApp Store 与卡片刷新
```

`smart_home` 与 RPK 不得假定为同一进程：前者保存 `DeviceService` 的内存状态，后者由 VAPP
启动并加载 Feature。因此 Feature 不能持有 `smart_home_device_service_t *`，也不能以全局变量
直接调用 Provider。P0 的正式实现使用 POSIX message queue（或后续等价的本地 IPC）：

- SmartHome 服务端在启动后持续接收固定大小的请求，调用 Provider，并按 `requestId` 回传结果；
- Feature 客户端在非 JS 线程接收 IPC 回包，再通过 Feature 的异步队列 resolve/reject Promise；
- 仅用于宿主机单测的 in-process Provider bridge 不构成模拟器或真机的运行时通道。

模拟器中的“设备”可以是原生侧的确定性夹具，不是 P4X 的真实灯、C6、MQTT 或传感器。
因此本阶段验证的是架构、生命周期与 API 契约；不把它误报为硬件或性能验收。

本阶段不包含：真实 `wlan0`、ESP-Hosted、C6 配网、Matter/MQTT/HA、真实传感器、cAGENT
云端模型请求、摄像头预览、AI 推理和 P4X 30 FPS 测量。

## 2. 配置拓扑

保持现有 P4X LVGL 基线不变，并新增项目专用的 Goldfish QuickApp 配置：

```text
board/goldfish-arm64/configs/
├── smart_home/                 # 现有：SmartHome 服务 + QuickApp Runtime，Console UI
└── smart_home_quickapp/        # 新增：QuickApp 页面 + Mock Native Feature

board/esp32p4/esp32p4-function-ev-board/configs/
├── smart_home/                 # 现有：Native LVGL 真机基线
└── smart_home_quickapp/        # 后续：P4X QuickApp 真机验证
```

Goldfish `smart_home_quickapp` 从项目已有的 `board/goldfish-arm64/configs/smart_home/`
派生，而非从 ESP32-S3 或 P4X defconfig 复制。前者已有 QuickApp、Feature Framework、UIKit
和 SmartHome 依赖；后者分别包含不适用于模拟器的 P4X/ESP32-S3 板级硬件参数。

目标 `.config` 的关键状态如下；具体依赖以 Kconfig 最终解析结果为准：

```text
CONFIG_SMART_HOME_DEMO=y
CONFIG_SMART_HOME_DEMO_UI_QUICKAPP=y
# CONFIG_SMART_HOME_DEMO_UI_LVGL is not set

CONFIG_GRAPHICS_LVGL=y                      # QuickApp 的底层渲染依赖，不能关闭
CONFIG_UIKIT=y
CONFIG_FEATURE_FRAMEWORK=y
CONFIG_QUICKAPP=y
CONFIG_QUICKAPP_VAPP=y                      # 以 RPK 作为应用入口时启用
CONFIG_FEATURE_SYSTEM_SMARTHOME=y
```

`SMART_HOME_DEMO_UI_QUICKAPP` 与 `SMART_HOME_DEMO_UI_LVGL` 必须位于同一个 choice，保证
只选择一个产品 UI 后端。QuickApp 模式不编译或启动 `smart_home_lvgl.c` 等原生产品 UI，
但仍保留图形库 LVGL，供 UIKit/QuickApp 使用。

## 3. 最小产品切片

P0.0 只实现一个房间、一盏灯和一个页面，设备标识固定为 `sim-living-room-light`：

| 数据项 | 初始值 | 说明 |
| --- | --- | --- |
| `online` | `true` | 原生夹具维护，可用于后续模拟离线 |
| `power` | `false` | 唯一首期可控状态 |
| `brightness` | `0` | 关闭状态按现有原生模型如实返回 `0`；首次开启时由原生 Provider 使用 `50` 作为默认亮度。亮度控制留给下一小步。 |
| `revision` | `1` 起递增 | 每次确认的状态变化递增 |

QuickApp 首页只显示设备名、在线状态、开关、命令中状态和最后更新时间。不得先实现复杂
场景、天气、动画、摄像头卡片或 Agent 对话，以免 API/运行时问题被 UI 噪声掩盖。

## 4. Native Feature 契约

首版 JIDL/Feature 只提供以下 API：

```js
import smartHome from '@system.smarthome'

const caps = await smartHome.getCapabilities()
const snapshot = await smartHome.getSnapshot({
  deviceId: 'sim-living-room-light'
})

const result = await smartHome.controlDevice({
  requestId: 'uuid',
  deviceId: 'sim-living-room-light',
  command: 'setPower',
  value: true,
  expectedRevision: snapshot.revision
})

applySnapshot(result.snapshot)
```

约束如下：

- `getSnapshot()` 返回指定设备的原生 State Store 快照；JS Store 仅是渲染缓存。
- `controlDevice()` 成功时返回已由原生服务确认的 `snapshot`；页面不得先乐观修改状态。
- 同一 `requestId` 的重复请求不得重复改变状态。
- 旧 `expectedRevision` 必须以 `1002`（stale revision）拒绝；页面随后重新获取 snapshot。
- P0.0 不暴露 `subscribeState()`，也不以定时轮询替代。原生事件扇出完成后，P1 的订阅事件
  必须携带 `revision`；发现跳变或订阅断开时，QuickApp 重新获取 snapshot。
- Native Feature 只做参数校验和 IPC 桥接；不得阻塞 QuickApp JS 事件循环，也不得在 Promise
  包装函数中同步等待消息队列回包。

首期不提供 `askAgent()`，也不仿造 `@system.velaclaw`。cAGENT 接入应在该四个接口稳定后，
作为下一阶段独立验证。

## 5. 实施顺序

### M0：配置与启动

1. 在 `SMART_HOME_DEMO_UI_BACKEND` choice 增加 `SMART_HOME_DEMO_UI_QUICKAPP`；它依赖
   `QUICKAPP`、`QUICKAPP_VAPP` 和 `FEATURE_FRAMEWORK`，并选择 `FEATURE_SYSTEM_SMARTHOME`。
2. 新增 Goldfish `smart_home_quickapp` defconfig；保持 `smart_home` Console 配置不变。
3. 同步更新 `demos/smart_home/Makefile` 与 `CMakeLists.txt`：QuickApp 模式不包含原生
   LVGL 产品 UI 源文件，但包含 SmartHome 服务和 IPC 服务端。`smart_home` 在此模式只作为
   原生服务常驻，RPK 由独立 `vapp` 进程启动。
4. 构建并启动模拟器，确认 RPK 可从 VAPP 启动，且不出现第二个 `lv_init()`/原生 UI 主循环。

构建命令：

```bash
./build.sh \
  contest2026_031_niudanxianqianchong/board/goldfish-arm64/configs/smart_home_quickapp \
  --cmake -j8
```

产物目录以构建日志为准；启动时将该 CMake 输出目录传给 `./emulator.sh`。RPK 由 AIoT-IDE
打包后按 QuickApp 教程解压并推送到 `/data/app/<package>/`，在模拟器串口执行
`vapp hap://app/<package>`。

### M1：Native 状态服务与 Feature

1. 建立 `system.smarthome@1.0` JIDL 和 Goldfish 原生实现。
2. 建立仅包含 `sim-living-room-light` 的 State Store/Simulator Device Provider。
3. 建立服务端与 Feature 端的 request/response IPC：请求必须带关联 ID，回包必须带状态码及
   snapshot；接收在 worker/UV 异步队列中完成。
4. 完成 `getCapabilities()`、`getSnapshot()`，先不提供订阅。
5. 在 QuickApp 启动时按“能力协商 → snapshot”顺序渲染卡片。

### M2：控制与状态回写

1. 增加 `controlDevice(setPower)`，由 Native Service 验证设备、revision、值和 requestId。
2. 原生侧确认状态变化后递增 revision，并在 `controlDevice()` 结果中返回快照。
3. QuickApp 不使用乐观状态；仅以控制结果、拒绝或随后主动刷新来收敛。
4. 注入重复 requestId、旧 revision、离线设备和服务重启，确认错误路径不造成假成功。

### M3：生命周期与交付记录

1. 退出并重新启动 RPK，验证原生服务仍可提供最新 snapshot。
2. 重启原生服务后重新启动 RPK，验证 QuickApp 自动重新获取 snapshot；订阅中断恢复列为 P1 验收。
3. 记录最终 defconfig、CMake 构建日志、RPK SHA-256、启动日志、页面截图和验收结果。

## 6. 验收矩阵

| 项目 | 动作 | 通过条件 |
| --- | --- | --- |
| 构建 | Goldfish CMake 构建 | CMake 配置与编译成功；最终 `.config` 仅选中 QuickApp UI backend。 |
| 启动 | 启动模拟器并运行 RPK | 页面可见，未启动原生 LVGL 产品 UI。 |
| 初始状态 | 打开首页 | 卡片来自 `getSnapshot()`，显示 `revision` 与原生初始值一致。 |
| 实时状态（P1） | 原生 Provider 改变电源状态 | QuickApp 经 `state.changed` 刷新，无轮询伪造。 |
| 控制 | 连续切换开关、重复同一 requestId | 最终状态正确；重复请求不重复执行。 |
| 异常 | 旧 revision、离线和服务重启 | 有明确失败/过期状态；页面不冻结、不假报成功。 |
| 重启恢复 | 停止再启动 RPK | 重新获得 snapshot，状态与原生 State Store 收敛。 |

M0~M3 全部通过后，才允许把同一 JIDL、QuickApp 页面和 Feature 实现迁移到 P4X
`smart_home_quickapp`。P4X 阶段必须重新验证显示、GT911、PSRAM/Flash、真实设备、C6 网络
和 30 FPS；模拟器通过不能豁免任何真机验收。

### H0：P4X 显示、触摸与 IPC 冒烟

项目提供 `board/esp32p4/esp32p4-function-ev-board/configs/smart_home_quickapp/`，它继承
现有 `smart_home` 的显示、PSRAM、LittleFS 和 ESP-Hosted 板级参数，但仅切换产品 UI
后端。该配置有以下刻意取舍：

- 启用 QuickApp VAPP、QuickJS、UIKit、Feature Framework 和 `system.smarthome`；
- 启用 QuickApp 所需的 `UTILS_CURL` 依赖闭包：`LIB_ZLIB` 与 `CRYPTO_MBEDTLS`；
- 启用板级 GT911，关闭独立 `gt911_probe`，避免两个应用同时访问触摸设备；
- 关闭 `smart_home_lvgl`、LVGL demo、audio smoke 和 I2S0；QuickApp 运行时仍使用底层 LVGL，
  但不启动第二个原生产品 UI；
- 以 `TLS_TASK_NELEM=4` 启用 libc++ 与 libuv 所需的任务本地存储；否则 Kconfig 会将
  `LIBCXX` 回退为 `LIBCXXNONE`，导致 Yoga 缺少 C++ 标准库头文件；
- 保留 C6/ESP-Hosted 配置但维持 `SMART_HOME_DEMO_OFFLINE_UI`。首轮不以联网、音频或
  摄像头为通过条件。

编译前，必须将 `system.smarthome` Feature 的 JIDL、IPC 协议和实现合入构建树实际使用的
`frameworks/runtimes/feature` 仓库；仅合入比赛项目不足以构建 QuickApp 模式。然后在
openvela 根目录执行：

```bash
source myenv/bin/activate
./build.sh \
  contest2026_031_niudanxianqianchong/board/esp32p4/esp32p4-function-ev-board/configs/smart_home_quickapp \
  --cmake -j8
```

构建后必须检查最终 `.config`，确认以下模式互斥和依赖成立：

```bash
rg '^(CONFIG_SMART_HOME_DEMO_UI_QUICKAPP|CONFIG_FEATURE_SYSTEM_SMARTHOME|CONFIG_QUICKAPP_VAPP|CONFIG_ESP32P4_FUNCTION_EV_BOARD_GT911)=' nuttx/.config
rg '^CONFIG_SMART_HOME_DEMO_UI_LVGL=' nuttx/.config && false || true
```

H0 仅验收“smart_home 服务常驻、VAPP 启动 RPK、屏幕可见、GT911 可点按、客厅模拟灯可读写并
回显 revision”。烧录地址、Flash 参数和串口必须以本次构建生成的产物与 `.config` 为准，不得
复用其他 P4X 镜像的命令。

## 7. 并行开发与止损

Wi-Fi 开发可继续在 P4X Make 构建通道进行；模拟器 QuickApp 不直接访问 C6、`wlan0`、
socket 或 MQTT，因此两者不共享运行时故障面。双方仅共享 `system.smarthome` 的 API 文档、
状态模型和错误码语义。

若 M0 无法稳定启动 RPK、M1 的 Feature 无法返回真实状态，或 M2 出现 UI/原生状态分叉，则暂停
页面扩展和 cAGENT 接入，先修复该最小链路。不得以 JS Mock 绕过 Native Feature 后宣布
验证通过。

## 8. 维护记录

| 日期 | 内容 |
| --- | --- |
| 2026-09-14 | 建立 Goldfish QuickApp 最小真实业务闭环方案：定义独立 defconfig、原生 Feature、模拟设备、M0~M3 实施步骤与迁移门。 |
| 2026-09-17 | 新增 P4X `smart_home_quickapp` H0 配置；将 Host IPC 契约设为强制依赖，明确 Feature 仓库与比赛项目须同步集成。 |
