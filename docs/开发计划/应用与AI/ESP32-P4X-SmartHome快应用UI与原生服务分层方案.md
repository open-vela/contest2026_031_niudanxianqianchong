# ESP32-P4X SmartHome 快应用 UI 与原生服务分层方案

> 状态：候选架构方案，需完成 P0 可行性验证后方可作为主实现路线。
>
> 目标平台：ESP32-P4X Function EV Board + EK79007 1024×600 横屏 + GT911 触摸。
>
> 本文只讨论“快应用负责 UI、原生服务负责业务与硬件”的分层方案；不要求
> 快应用直接访问 GPIO、摄像头或网络驱动，也不改变当前 LVGL 原生 UI 的可运行路径。

相关文档：

- [ESP32-P4X 智能家居中控面板 UI 设计方案](./ESP32-P4X-智能家居中控面板UI设计方案.md)：产品 IA、视觉规范及当前 LVGL 路线。
- [ESP32-P4X 端侧目标检测方案](./ESP32-P4X-端侧目标检测方案.md)：AI 事件和摄像头资源约束。
- [ESP32-P4X SmartHome 板载 C6-Wi-Fi 接入方案](../网络与连接/ESP32-P4X-SmartHome板载C6-WiFi接入方案.md)：网络服务边界。

---

## 1. 背景与决策问题

SmartHome 的目标是常驻式 1024×600 家庭中控，包含首页、设备、场景、安防、
能耗、设置和 Agent 等多层页面。此类产品既有复杂的视觉与交互需求，也有设备
控制、网络、Agent、端侧 AI 感知和摄像头等实时业务需求。

当前 `demos/smart_home` 的 UI 与业务均由原生 C 实现，P4X 配置已启用 LVGL。
仓库中的快应用目前仅有最小样例，尚未在 P4X 上验证快应用运行时、触摸、中文字体、
原生 Feature 及资源占用。因此不能把“页面会越来越复杂”等同于“立即替换为快应用”。

本方案提出以下候选分工：

```text
快应用：页面、组件、主题、交互、UI 状态、轻量动画
原生服务：设备、场景、Agent、网络、鉴权、AI 事件、摄像头与资源调度
```

只有在 P0 验证证明 P4X 可稳定运行该链路后，快应用才可成为主 UI 路线；否则保留
既有 LVGL 组件化路线，不影响产品功能交付。

## 2. 目标、范围与非目标

### 2.1 目标

- 让 UI 的页面与样式能够以 HTML/CSS/JavaScript 快速迭代，并以 rpk 独立交付。
- 保持设备控制、AI 事件和安全策略在原生侧集中实现，避免 JS 层直接耦合硬件。
- 通过稳定、版本化的 JS-Native API，使 UI 更换技术栈时不重写业务逻辑。
- 将运行时异常限制在可恢复范围：UI 可重启和重连，设备服务可继续维持安全状态。

### 2.2 本方案覆盖的功能

- 家庭状态、设备和场景展示；设备控制；Agent 对话与执行进度；安防与 AI 提醒。
- 快应用的路由、主题、组件、状态管理、加载/错误/离线体验。
- 原生 Feature 的命令调用、状态快照、事件订阅、权限校验和断连恢复。

### 2.3 非目标

- 不将 GPIO、I2C、SPI、Wi-Fi、Matter、MQTT、摄像头驱动或模型推理暴露给 JS。
- 不让快应用承载 API Key、设备密钥和安全策略。
- 不在 P0/P1 通过 JS 传输摄像头原始帧、PCM 或高频推理中间结果。
- 不要求快应用与 LVGL 在同一页面中同时渲染同一套产品界面。

## 3. 总体架构

```text
┌─────────────────────────────────────────────────────────────┐
│                    SmartHome Quick App                       │
│  Pages / Components / Theme / Store / Router / UI Animation │
└───────────────────────────┬─────────────────────────────────┘
                            │ Promise + event subscription
┌───────────────────────────▼─────────────────────────────────┐
│             SmartHome Feature（JS-Native 契约）               │
│  参数校验 · 权限声明 · 序列化 · 版本协商 · 生命周期管理       │
└───────────────────────────┬─────────────────────────────────┘
                            │ 原生进程内 API 或受控 IPC
┌───────────────────────────▼─────────────────────────────────┐
│                 SmartHome Native Service                     │
│  State Store · Device Manager · Scene Manager · Agent Bridge │
│  Event Router · Policy Guard · Network / Gateway Adapter     │
└───────┬──────────────┬───────────────┬───────────────┬──────┘
        │              │               │               │
   设备/场景        cAGENT          C6 Wi-Fi        AI/Camera
```

`cAGENT` 是原生服务消费状态、调用受控工具的 Agent 能力，不是家庭状态的第二份
事实来源。设备状态与命令结果先由 `Device Manager` / `State Store` 确认；Agent Bridge
再读取该状态或请求受控命令。这样 QuickApp 直接控制设备与 Agent 工具控制设备会收敛到
同一条审计、幂等和状态回写链路。

### 3.1 单一事实来源

原生 `SmartHome Native Service` 是家庭状态唯一事实来源。快应用内的 Store 是用于
渲染的缓存，不得自行推断设备最终状态。

- 所有控制命令都先发至原生服务，再由原生服务执行策略校验和设备操作。
- 原生服务完成操作或收到网关回报后，发布新的状态版本。
- 快应用以 `getSnapshot()` 恢复完整状态，再以事件流增量更新。
- UI 可做短暂的乐观更新，但必须以原生确认或超时回滚为准。

### 3.2 进程与生命周期原则

原生服务应为独立、可常驻的后台能力；快应用则是可启动、暂停、重启的前台 UI。
不要将“用户打开一个页面”作为设备服务、Wi-Fi 或 Agent 的唯一存活条件。

快应用启动、从待机恢复或 Feature 重连时，统一执行：

```text
连接 Feature → 协商 API 版本 → getCapabilities()
    → getSnapshot() → 订阅事件 → 恢复当前路由与渲染
```

### 3.3 受控混合 UI 原则

“QuickApp 做复杂页面、LVGL 做摄像头显示”不应实现为两个独立 UI 应用同时操作
`/dev/fb0` 或同一 LVGL screen。QuickApp 运行时本身基于 UIKit/LVGL；两个独立
渲染循环、触摸焦点和 framebuffer 所有权并行，当前没有 P4X 验证基础。

允许的形态只有两种：

```text
A. 同一 UIKit/LVGL 根：QuickApp 页面 + CameraPreview 原生 Widget
B. 页面级交接：QuickApp 保存状态并暂停 → Native Monitor 全屏页 → 返回后恢复
```

其中 A 需要实现并验证一个受控的原生 Widget；B 的实现和性能风险更低，作为实时
监控首期方案。首页与安防总览不常驻视频，采用缩略图、在线状态和告警摘要。

### 3.4 两种构建时 UI 模式

P4X 应将 LVGL 与 QuickApp 作为两种**互斥的构建时主 UI 模式**，而不是在同一固件中
由用户随意切换的两个主界面。现有 `demos/smart_home/Kconfig` 已有
`SMART_HOME_DEMO_UI_BACKEND` choice（Console / LVGL）；后续应在该 choice 中增加：

```kconfig
config SMART_HOME_DEMO_UI_QUICKAPP
    bool "QuickApp UI (RPK)"
    depends on QUICKAPP && QUICKAPP_VAPP
```

`SMART_HOME_DEMO_UI_LVGL` 保持为原生 LVGL 主 UI，`SMART_HOME_DEMO_UI_QUICKAPP`
表示由 QuickApp VAPP 启动 RPK 并拥有产品 UI 根。不要用一个额外的 bool 同时打开两者；
choice 必须始终只选择一个产品 UI 后端。Console 选项可保留给诊断，但不是 P4X 产品模式。

建议提供两个板级 defconfig，而非在同一产物中运行时改写模式：

| 配置目录 | UI 后端 | 用途与资源边界 |
| --- | --- | --- |
| `configs/smart_home/`（现有）或后续 `configs/smart_home_lvgl/` | `SMART_HOME_DEMO_UI_LVGL=y` | 当前默认和量产/比赛基线；保留本地 LVGL、cAGENT、诊断与回退路径。 |
| `configs/smart_home_quickapp/`（新增） | `SMART_HOME_DEMO_UI_QUICKAPP=y`、`QUICKAPP=y`、`QUICKAPP_VAPP=y` | QuickApp 验证/候选产品路线；显式配置 QuickJS、UIKit、Feature Framework、RPK 目录和所需字体/资源。 |

两种模式都构建同一份 `SmartHome Native Service`、Device Manager、State Store、cAGENT
Bridge、Vision Service 与网络服务；只有 UI 入口、资源包和渲染树不同。QuickApp 模式
不得编译/启动现有 `smart_home_lvgl.c` 作为第二个产品主循环；LVGL 模式也不启动 VAPP。
这避免 `lv_init()`、framebuffer、触摸焦点和 UI 生命周期的双重所有权。

## 4. 职责边界

| 层级 | 应负责 | 不应负责 |
| --- | --- | --- |
| 快应用 UI | 页面、路由、组件、主题、输入校验、展示状态、加载与错误反馈 | 设备协议、密钥、硬件驱动、业务安全策略 |
| SmartHome Feature | 类型转换、权限范围、版本协商、调用转发、事件分发 | 保存家庭业务状态、直接实现设备协议 |
| 原生 SmartHome 服务 | 设备状态、场景执行、Agent、网络、AI 事件、策略、审计 | 具体页面布局、颜色、卡片状态 |
| 驱动/网关/模型 | 采集、协议适配、推理、设备执行 | UI 语义与页面流程 |

UI 只使用业务语义，例如 `device.set`、`scene.run`、`security.event`；不得出现
“调用 MQTT topic”“打开 I2C 总线”等底层语义。

## 5. Feature API 草案

API 应以能力、命令和事件三类接口组织。以下为 P0/P1 所需最小集合，命名仅为
设计草案，具体 JIDL/Feature 组织方式以 openvela 快应用框架实现为准。

### 5.0 真实数据接入模型

QuickApp 的数据来源必须区分“通用系统传感器”与“家庭业务状态”：

| 数据 | JS 侧接口 | 原生数据源与边界 |
| --- | --- | --- |
| 加速度、环境温湿度、光照等标准传感器 | 已有 `@system.sensor` | 仅当 P4X 驱动将真实数据发布到 uORB 时可用；不以模拟器数据替代真机验收 |
| 设备状态、房间、场景、能耗、网关连接 | `@system.smarthome`（新增） | Native Service 汇聚 C6/HA/MQTT/Matter 与本地设备；它是唯一家庭状态来源 |
| Agent 回复和工具进度 | `@system.smarthome`（或已验证的 Agent Feature） | cAGENT 在原生侧执行，JS 不持有模型密钥或工具策略 |
| 摄像头、AI 感知 | `@system.smarthome` + 原生预览 Widget | QuickApp 只得到预览状态、缩略图和语义事件，绝不得到 RGB565 原始帧 |

`@system.sensor` 适合展示单个、低频的真实硬件读数；SmartHome 产品页面必须优先消费
`@system.smarthome` 的已校验快照和事件，避免每个页面自行订阅 uORB 或自行解释协议。
Feature Framework 使用 JIDL 描述 JS 与 C/C++ 的接口，并支持异步回调/Promise；耗时的
网络、设备、Agent 和推理工作必须在原生服务/worker 中完成，再将结果投递回 Feature。

### 5.0.1 快应用教程的可复用边界

openvela 比赛教程说明了 RPK、Feature 导入和 Promise 调用的通用方式，但不提供本项目
可直接使用的 cAGENT 或 SmartHome Feature。教程中的 `@system.velaclaw` 桥接的是
openvela `ai_agent`，不能视为调用现有 cAGENT 的通用入口；不得为了复用该名称把
cAGENT 的密钥、工具或设备协议暴露给 JS。教程中的模拟器传感器数据也只能用于页面开发，
不能替代 P4X 真机的真实数据验收。

因此本项目新增并维护 `system.smarthome@1.0`：它是 QuickApp 的唯一家庭业务入口，
原生实现内部再适配 cAGENT、uORB、C6 网络上的网关或其他协议。参考资料：

- [QuickApp 教程索引](https://github.com/open-vela/docs/blob/dev-ai-contest-2026/zh-cn/contest_2026/quickapp/quickapp_guide_index.md)
- [VelaClaw Feature 教程](https://github.com/open-vela/docs/blob/dev-ai-contest-2026/zh-cn/contest_2026/quickapp/quickapp_velaclaw.md)
- [Feature Framework README](../../../../frameworks/runtimes/feature/README.md)

### 5.1 查询与能力协商

```js
const caps = await smartHome.getCapabilities()
const snapshot = await smartHome.getSnapshot()
```

`getCapabilities()` 返回 API 版本、设备类型、控制能力、是否支持 Agent、摄像头预览
和 AI 感知等能力开关。页面必须按能力降级，不假设每块板或每个家庭都具有相同硬件。

`getSnapshot()` 至少包含：

```json
{
  "revision": 42,
  "timestampMs": 0,
  "home": { "mode": "home", "network": "online" },
  "devices": [],
  "scenes": [],
  "environment": {},
  "agent": { "state": "idle" },
  "alerts": []
}
```

其中 `revision` 是单调递增版本号。每个增量事件必须携带 `revision`，用于发现事件丢失。

### 5.2 控制命令

```js
await smartHome.controlDevice({
  requestId: "uuid",
  deviceId: "living-room-light",
  command: "setPower",
  value: true,
  expectedRevision: 42
})

await smartHome.runScene({ requestId: "uuid", sceneId: "movie" })
```

约束：

- `requestId` 用于幂等和 UI 重试关联；同一命令重复提交不得造成重复执行。
- 原生服务必须执行参数范围、设备在线状态、家庭安全策略和权限校验。
- 响应只表示“已受理/被拒绝/执行失败”；最终设备状态以状态事件为准。
- 对可能造成风险的操作（解锁、安防撤防等）应由原生服务要求二次确认，而不能只信任 UI。

### 5.3 Agent 与事件

```js
const task = await smartHome.askAgent({ requestId: "uuid", text: "开启观影模式" })
const off = smartHome.subscribe((event) => applyEvent(event))
```

事件按业务域划分：

| 事件域 | 示例 | UI 行为 |
| --- | --- | --- |
| `state.changed` | 设备、场景、环境变更 | 按 revision 合并 Store |
| `command.result` | 控制成功、失败、超时 | 结束加载，给出反馈 |
| `agent.progress` | 思考、调用工具、回复完成 | Agent 页展示过程，不展示内部密钥 |
| `security.alert` | 跌倒风险、陌生人、门窗异常 | 告警卡片、通知或全屏确认 |
| `service.health` | 网络断开、网关重连、功能不可用 | 受控降级，不阻塞整个页面 |

应限制事件载荷为结构化状态和轻量文本。高频事件在原生侧聚合或限流后再推送；例如
传感器数值只需按 UI 所需频率刷新，不能把原始采样流直接送入 JS。

### 5.4 `system.smarthome` 最小接口与调用约束

P0/P1 优先实现以下接口，而不是一开始暴露所有设备协议：

```js
import smartHome from '@system.smarthome'

const snapshot = await smartHome.getSnapshot()
const stop = smartHome.subscribeState((event) => applyEvent(event))
await smartHome.controlDevice({ requestId, deviceId, command, value })
smartHome.subscribeSecurityEvent((event) => showAlert(event))
```

原生实现流程为：定义 `system.smarthome@1.0.jidl` → 生成 Feature 胶水代码 → 在 C/C++ 中
连接 SmartHome Native Service → 注册到 QuickApp runtime → 在 rpk 的 `manifest.json` 声明
Feature。接口必须异步：Feature 回调不得同步等待 Wi-Fi、C6、Home Assistant、设备 ACK 或
模型推理；完成后再通过 Promise、回调或状态事件通知页面。

Feature 与 QuickApp JS 共用事件循环。原生适配层只能完成参数校验、请求入队和轻量事件
分发；网络 I/O、cAGENT 等待、设备协议与数据聚合均在 worker/服务线程执行，并通过
`FeaturePost` 返回主循环。每次回调都必须可取消或在应用退出时安全丢弃，避免旧页面收到
过期状态。

所有事件携带 `revision` 和时间戳。环境值在原生侧聚合/限流后推送（通常 1~5 秒一次）；
设备控制与告警在状态变化时立即推送。UI 发现版本跳变、订阅中断或服务重启时，必须重新
调用 `getSnapshot()`，不能只依赖本地缓存。

## 6. 摄像头、AI 与多媒体边界

摄像头预览和端侧推理会占用大量 PSRAM 与内存带宽。以下分层必须坚持：

- 原始相机帧、推理输入、目标检测中间数据始终留在原生侧。
- 快应用仅接收语义事件，例如“客厅有人”“检测到跌倒风险”，以及可控频率的缩略图
  或单帧截图。
- 若产品需要实时预览，应首先验证原生渲染 Surface/纹理如何嵌入快应用，或将预览
  作为原生页面/受控原生组件；未经实测不得把完整视频帧序列经 JS Bridge 传输。
- 预览不可用时，安防页仍应展示最近截图、摄像头状态和告警记录，而不是白屏。

### 6.1 摄像头的唯一消费者与帧分发

Vision Service 是 `/dev/video0` 的唯一流式消费者，负责 `DQBUF → 分发/处理 → QBUF`。
QuickApp、Agent 工具和其他页面不得自行打开设备或持有 V4L2 缓冲。Vision Service 以
“最新帧优先、落后即丢弃”策略向下游提供受控预览，并以跳帧方式运行 1~2 FPS 的 AI
检测。这样预览卡顿不会阻塞采集，也不会使旧帧占满 PSRAM。

```text
/dev/video0 → Vision Service（唯一 DQBUF/QBUF）
             ├── AI 推理：跳帧、低频结果
             ├── 缩略图：按事件或 0.2~1 FPS
             └── 原生预览：5 FPS 起步，实测后最多评估 10 FPS
```

摄像头采集可以独立达到 30 FPS；它不等于预览显示、复杂 QuickApp 页面或 AI 检测也必须
达到 30 FPS。复杂 QuickApp 交互页的 30 FPS 验收不包含同屏全分辨率视频重绘。

### 6.2 快应用中的实时预览方案

`CameraPreview` 是待实现的原生能力，而不是向 JS 发送图片字节的普通 Feature 方法。
它接收 `cameraId`、区域几何、可见性和模式等控制参数，在与 QuickApp 相同的
UIKit/LVGL 根中创建/更新受控原生 Widget。QuickApp 负责其外层卡片、控制按钮和状态，
原生代码负责像素显示、裁剪、释放和帧节流。

```text
QuickApp SecurityPage
 ├── CameraPreview（Native Widget，预览像素）
 ├── 设备状态、AI 事件、控制按钮（QuickApp）
 └── getSnapshot/subscribeState（SmartHome Feature）
```

现有 UIKit Video Adapter 依赖 `MEDIA_SERVER` 和视频 tunnel，不能直接视作 P4X
`/dev/video0` 的现成方案。首期优先验证自定义 `CameraPreview`；若无法在同一根中稳定
管理，则退回页面级交接：QuickApp 保存路由与 Store 状态，进入独立 Native Monitor 页，
退出后重新同步 snapshot 并恢复页面。

### 6.3 安防页面的分级体验

| 场景 | 画面 | 性能目标 |
| --- | --- | --- |
| 首页、普通设备页 | 最近缩略图 + 在线/告警状态 | 不开连续预览，保持复杂 UI 30 FPS |
| QuickApp 安防总览 | 受控小窗或缩略图 | 预览从 5 FPS 验收，UI 仍以 30 FPS 为目标 |
| 实时监控 | 独立 Native Monitor 全屏页 | 先验证画面连续、无花屏、无内存增长；实测评估 10 FPS |
| 预览 + AI | 原生视频 + 低频检测框/文本 | 视频 5~10 FPS，AI 1~2 FPS；不承诺全局 30 FPS |

## 7. UI 架构建议

快应用采用“单向数据流”：Feature 事件写入 Store，组件只渲染 Store；组件交互转为
Command，不在任意页面中保存另一套设备真相。

```text
Feature snapshot/event → Store → Page / Component
Page interaction        → Action → Feature command → Native Service
```

建议目录：

```text
quickapp/smart_home_ui/
├── src/pages/          # 首页、设备、场景、安防、能耗、Agent、设置
├── src/components/     # DeviceCard、SceneCard、AlertCard、Dialog 等
├── src/store/          # snapshot、event reducer、selector、连接状态
├── src/services/       # SmartHome Feature 包装、重连与错误归一化
├── src/theme/          # 颜色、字号、间距、深/浅主题 token
├── src/assets/         # 小型图标和必要图片
└── manifest.json       # 权限、Feature、入口与版本
```

页面仍遵循产品方案定义的“首页 → 设备/场景/安防/能耗/设置 → 详情”结构，最大深度
控制在三层。通用卡片、弹层和导航只能有一套实现，避免每页复制 CSS 与业务判断。
若启用 `CameraPreview`，其位置与可见性由页面声明，帧管理仍完全留在原生侧；不允许
QuickApp Store 保存视频帧或反复创建/销毁预览对象。

## 8. 分期验证与决策门

### P0：运行时与桥接可行性（必须先完成）

目标是在 P4X 真机上证明基础链路，不改造完整 UI。

1. 新建 `configs/smart_home_quickapp/` P4X 验证配置，在既有 UI backend choice 中选择
   `SMART_HOME_DEMO_UI_QUICKAPP`，并显式满足 `QUICKAPP`、`QUICKAPP_VAPP`、UIKit、
   Feature Framework、QuickJS、字体和 RPK 资源依赖；保留现有 LVGL `smart_home` 配置
   作为独立回退镜像。
2. 部署最小 rpk，验证 1024×600 显示、GT911 触摸、中文字体、待机/恢复和启动失败提示。
3. 实现 `system.smarthome@1.0` 的只读 `getCapabilities()`、`getSnapshot()` 与
   `state.changed`，并在 rpk manifest 中声明该 Feature；至少接入一项真实传感器或真实
   设备状态，不能只用 mock。
4. 实测启动时间、空闲/页面切换/连续事件时的 RAM、PSRAM、Flash、CPU 和触摸响应，
   与纯 LVGL 基线比较并记录。
5. 连续重启快应用与原生服务，确认一方重启不使另一方失控；重连后状态可收敛。

**P0 通过条件**：显示与触摸可用；Feature 调用和事件订阅稳定；资源使用仍留有可供
摄像头、网络和 Agent 的余量；无不可恢复的渲染、重连或异常退出问题。

### P1：一条垂直业务链

实现“首页 + 灯光设备卡 + 开关/亮度控制 + 原生状态订阅”。覆盖加载、超时、离线、
命令重复、状态冲突和应用重启恢复。状态必须来自真实 SmartHome Native Service，至少
验证一条“真实数据 → Feature → QuickApp”和一条“QuickApp 命令 → 真实设备 → 状态回写”。
这一阶段不接摄像头实时预览，也不迁移全部 Agent。

**P1 通过条件**：用户操作与最终设备状态一致；命令可追踪、可重试且不重复执行；
连续运行下不存在明显内存增长或事件积压。

### P2：产品 UI 与 Agent

扩展首页、设备、场景、安防、能耗、设置、Agent 页面，接入场景控制、环境状态、
Agent 进度和安全提醒。`askAgent()` 必须验证“QuickApp 请求 → cAGENT → 原生工具/状态
仓库 → `agent.progress` / `command.result` / `state.changed`”的完整链路；不得让 Agent
绕开 Native Service 直接修改 UI 缓存。先用语义事件/缩略图完成安防体验，再评估视频
预览方案。

### P2.5：CameraPreview 或 Native Monitor 验证

1. 在 Vision Service 独占 `/dev/video0` 的前提下，完成 320×180 或同级小窗预览。
2. 验证页面切换、隐藏、待机、QuickApp 重启和 Vision Service 重启时的停止/恢复顺序。
3. 分别测量 UI-only、UI + Vision、UI + Vision + AI 三种场景的帧率、PSRAM、CPU、丢帧和
   触摸 P95 响应。
4. 若同根 Native Widget 的层级、裁剪或资源释放不稳定，停止嵌入式路线，采用 Native
   Monitor 全屏页；不得以跨 JS 拷贝视频帧作为替代。

### P3：决定主路线

| 结果 | 决策 |
| --- | --- |
| P0/P1 通过且 UI 迭代效率明显提升 | 快应用成为 P4X 主 UI；原生 LVGL 保留为诊断/回退界面 |
| 基础 UI 可用但视频、资源或稳定性不达标 | 快应用用于普通页面；高性能预览采用独立原生能力 |
| P0 未通过或资源余量不足 | 停止 UI 迁移，回到现有 LVGL 组件化路线 |

## 9. 可靠性、安全与升级

- **断线**：网络或服务不可用时显示最后一次状态和明确的“数据可能过期”标识；不得
  伪造控制成功。
- **重连**：订阅异常后指数退避重连，并重新获取 snapshot；发现 revision 断档也应全量同步。
- **安全**：API Key、设备凭据、网络密码仅存于原生安全配置；快应用仅获得最小业务权限。
- **策略**：设备范围、定时上限、安防操作确认和审计必须在原生服务强制执行。
- **升级**：Feature API 使用语义版本与能力协商；新增字段保持向后兼容，删除/改义字段
  必须升级主版本或保留兼容层。
- **回退**：保留一份已验证的原生 LVGL 基础控制页面，用于快应用损坏、资源包缺失或
  现场排障；二者不同时充当正式主界面。

## 10. 风险清单

| 风险 | 影响 | 缓解措施 |
| --- | --- | --- |
| P4X 未有快应用运行时基线 | 集成工作量和可用性未知 | 先以独立 P0 配置验证，不动主固件路线 |
| QuickJS、布局与 UI 运行时增加资源占用 | 挤压摄像头、网络和 Agent 余量 | 资源基线实测；限制图片、节点数、动画与事件频率 |
| JS-Native 接口设计过早固化 | 后续 UI 与业务迭代受阻 | 先实现最小 API；使用 capability 与版本协商 |
| UI 乐观更新与设备真实状态不一致 | 用户误以为控制成功 | 命令确认、revision、超时回滚和最终状态事件 |
| 摄像头帧跨 JS 传递 | 卡顿、内存压力或崩溃 | 原始帧留在原生侧，先做语义事件与缩略图 |
| QuickApp 与 Native UI 同时争夺显示根 | 黑屏、触摸异常、生命周期错乱 | 仅用同根原生 Widget 或页面级交接；禁止两个独立 UI 应用混绘 |
| Feature 阻塞 UI 事件循环 | 页面卡顿、订阅丢失 | 所有网络/推理/设备等待移至 Native Service/worker，异步回投 |
| 两套 UI 长期并行 | 维护成本翻倍、交互不一致 | 明确 P3 决策门，LVGL 仅保留回退/诊断职责 |

## 11. 与现有 LVGL 方案的关系

本方案不否定现有 LVGL 资产。现有设备模型、cAGENT、工具调用、事件机制和资源管理
应继续作为两种模式共用的原生业务基础；现有 LVGL UI 则是 P0 的对照基线与可靠回退
镜像。两种模式不在运行时互相覆盖或互相嵌套。

在 P0/P1 未通过前，原《ESP32-P4X 智能家居中控面板 UI 设计方案》附录 B 的 LVGL
推荐仍然有效。P1 通过后，再根据实测结果修订该文档的最终技术选型，而不是让两份
文档给出相互矛盾的“已决定”结论。

## 12. 维护记录

| 日期 | 内容 |
| --- | --- |
| 2026-09-13 | 建立候选方案：定义快应用 UI 与原生 SmartHome 服务的边界、Feature API 草案、验证阶段与决策门。 |
| 2026-09-13 | 补充真实设备/传感器数据的 Feature 接入模型，以及 CameraPreview 原生 Widget、Native Monitor 回退、帧所有权和 P2.5 验证方案。 |
| 2026-09-14 | 根据 QuickApp 教程核正 `@system.velaclaw` 仅对接 `ai_agent`，明确 cAGENT 必须经 `system.smarthome` 原生 Feature 适配；补充 Feature 事件循环、真实数据与 Agent 闭环验收要求。 |
| 2026-09-14 | 确定 LVGL 与 QuickApp 为互斥的构建时主 UI 模式：在既有 UI backend choice 中增加 QuickApp 选项，并以独立 P4X defconfig 构建、验证和回退。 |
