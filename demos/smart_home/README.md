# smart_home

智能家居smart_home以 cAGENT 骨架在上层搭建业务和应用。将 agent 组装、本地工具、领域 Skill、虚拟设备状态和终端 UI 分离，开发者后续可将虚拟设备层替换为真实智能家居驱动。

本 demo 旨在展示产品级嵌入式 Agent 的工程结构，而非仅演示如何调用一次模型。



## 文档索引

README 只保留项目结构、核心能力和常用构建流程。更细的设计、排障和对外材料按用途放在
`docs/` 目录：

| 文档 | 定位 | 适合什么时候看 |
|------|------|----------------|
| [docs/dual-platform-guide.md](docs/dual-platform-guide.md) | 双平台构建与运行主指南 | 接入 `openvela_smarthome`、构建 Goldfish/ESP32-S3、部署资源、排查构建问题 |
| [docs/esp32s3-box.md](docs/esp32s3-box.md) | ESP32-S3-BOX-3 真机运行方案 | 调试 Wi-Fi、LittleFS、LVGL、触摸、API key 和资源烧录 |
| [docs/dev.md](docs/dev.md) | 开发联调记录 | 追溯 HTTPS、mbedTLS、触摸、资源分区、网络等历史问题和修复思路 |
| [docs/ui-design.md](docs/ui-design.md) | LVGL UI 设计规范 | 继续迭代 Panel、Chat、Settings、导航、错误态和视觉规范 |
| [docs/chat-thinking-chain.md](docs/chat-thinking-chain.md) | Chat 可解释事件链设计 | 调整 cAGENT event、工具调用卡片和模型响应展示 |
| [docs/environment-simulation.md](docs/environment-simulation.md) | 环境模拟设计 | 理解 Temp/Humidity/Light 如何进入 `get_home_status` 和 Agent context |
| [docs/user-memory.md](docs/user-memory.md) | 长期偏好记忆 | 理解 `user_memory` 的自动记录边界、持久化和忘记流程 |
| [docs/multi-llm-backend.md](docs/multi-llm-backend.md) | 多 LLM 后端与安全管控方案 | 扩展 Settings 页、后端 preset、API key 和模型配置校验 |
| [../../docs/plans/llm-backend-toolcall-comparison.md](../../docs/plans/llm-backend-toolcall-comparison.md) | 多 LLM 工具调用对比测试方案 | 冻结用例、trace、评分口径和 Goldfish/BOX-3 验收证据 |
| [docs/node-gateway.md](docs/node-gateway.md) | Node gateway 联调 | 将两个 sensor_node 接入 smart_home，验证动态远程工具注册 |
| [../mcp_probe/README.md](../mcp_probe/README.md) | MCP 独立协议探针 | 在没有 LVGL/Node/模型的条件下验证设备直连 MCP Server |
| [docs/mbedtls-transport-route-b.md](docs/mbedtls-transport-route-b.md) | HTTPS transport 解耦方案 | 处理 ESP32-S3 Wi-Fi/HAL mbedTLS 与 apps mbedTLS 冲突 |
| [docs/espidf-iot-contest-proposal.md](docs/espidf-iot-contest-proposal.md) | ESP-IDF 方向参赛文案 | 复用 smart_home/cAGENT 能力撰写 ESP-IDF AIoT 申报材料 |
| [docs/基于 openvela智能体 的智能家居控制系统的比赛.md](<docs/基于 openvela智能体 的智能家居控制系统的比赛.md>) | 赛题/任务说明 | 查看项目背景、评分点和交付要求 |

维护原则：`dual-platform-guide.md` 是构建运行事实来源；`dev.md` 是历史排障记录；参赛/申报材料不作为工程构建依据。

## 架构

```text
                 +----------------------+
 用户 / NSH ---> | ui/ 终端前端          |
                 +----------+-----------+
                            |
                            v
                 +----------------------+
                 | agent/ cAGENT 装配    |
                 | model + tools+skills |
                 +----------+-----------+
                            |
              +-------------+-------------+
              |                           |
              v                           v
  +-----------------------+    +------------------------+
  | skills/ 领域策略       |    | tools/ LLM 可执行动作   |
  | 注入给 LLM 的上下文    |    | set_light/run_scene... |
  +-----------------------+    +-----------+------------+
                                           |
                                           v
                                +-----------------------+
                                | device/ 虚拟家居状态   |
                                | 后续替换为真实驱动      |
                                +-----------------------+
```

运行时流程：

```text
用户输入
  -> cAGENT 构建 context（system prompt + skills + device state）
  -> 模型返回普通回答或 tool_calls
  -> 工具对虚拟设备状态执行操作
  -> 工具结果写回 session
  -> 模型生成最终 assistant 回答
  -> UI 打印回答和事件时间线
```

## 模块

```text
src/
  app/
    smart_home_main.c      进程入口

  agent/
    smart_home_agent.c     创建 cAGENT，绑定模型，注册 tools/skills
    smart_home_agent.h     应用层 Agent 对象和运行 API

  tools/
    smart_home_tools.c     LLM 可调用的动作
    smart_home_tools.h
    smart_home_user_memory_tools.c  remember/list/forget 工具

  memory/
    smart_home_user_memory.c  偏好文件、去重、遗忘和 Context 快照

  skills/
    smart_home_skill_loader.c  从 /data/res/skills 加载 Markdown Skill
    smart_home_skills.c        注册 Skill 和 read_skill 工具
    smart_home_skills.h

  device/
    smart_home_device.c    虚拟家居状态和场景执行
    smart_home_device.h

  net/
    smart_home_network.c   区分模拟器 eth0 与真机 wlan0
    smart_home_wifi.c      ESP32-S3 Wi-Fi/WAPI 连接

  ui/
    smart_home_ui.c        终端交互循环和 cAGENT 事件时间线
    smart_home_ui.h
    lvgl/
      smart_home_lvgl.c            LVGL runtime 入口和 run loop
      smart_home_lvgl_agent.c      Agent worker 与 lv_async_call 回写
      smart_home_lvgl_chat.c       对话气泡和工具调用卡片
      smart_home_lvgl_nav.c        底部导航
      smart_home_lvgl_home_v9.c    首页设备卡片和场景
      smart_home_lvgl_device_dialog.c 设备控制抽屉
      smart_home_lvgl_shell.c      顶栏、天气和时钟
      smart_home_lvgl_settings_v9.c   设置页
      smart_home_lvgl_style.c      颜色、卡片样式、可选字体资源

  config/
    smart_home_backends.c  DeepSeek/MiMo/Qwen/OpenAI 等后端 preset
    smart_home_config.h    默认 host/model/key、session id 和 buffer 大小
```

## 分层边界

`agent/` 负责 cAGENT 集成：

- `agent_create_simple()`
- `agent_attach_openai()`
- `agent_register_tool_simple()`
- `agent_register_skill()`
- `agent_register_context_provider()`
- `agent_set_event_callback()`

`tools/` 是 LLM 的动作面。工具应短小、确定性、返回 JSON。不应打印 UI 文本或直接与模型对话。

`skills/` 是策略和领域知识。在 cAGENT 中，Skill 是上下文而非可执行函数。Skill 告诉模型智能家居应该如何行为，工具执行实际的状态变更。

`device/` 是硬件边界。当前实现为内存中的虚拟家居。要接入真实设备，保持工具 schema 不变，将本层函数替换为驱动、总线、RPC、Matter、BLE、Wi-Fi 或厂商 SDK 调用。

`ui/` 负责用户交互和观测。打印对话提示符、assistant 输出和 ReAct 事件时间线，但不持有设备状态。

UI 架构先收敛为两条路径：

- 默认可运行路径：`CONFIG_SMART_HOME_DEMO_UI_CONSOLE=y`，使用 NSH/console。
- 产品 UI 路径：`CONFIG_SMART_HOME_DEMO_UI_LVGL=y`，使用圆屏 LVGL。

LVGL 的三屏职责固定为：

| 屏幕 | 职责 |
|------|------|
| 面板 | 设备管理与控制：设备状态、按房间筛选、创建、编辑、删除 |
| 对话 | Agent 聊天、工具调用链、错误/超时反馈 |
| 设置 | LLM 后端 preset、host/path/port、model、API key、timeout 和输出预算 |

## 工具

当前注册的 LLM 可见工具包括：

| 工具 | 用途 |
|------|------|
| `get_home_status` | 以 JSON 返回当前虚拟家居状态 |
| `get_device_inventory` | 按来源和设备类型查询缓存摘要及已注册的 Node/MCP 工具 |
| `board_camera` | 仅操作 `board:camera:0`；无云台，拍照不会自动开机 |
| `miot_device_list` / `miot_device_control` | 查询米家能力 / 按 `milo:<did>` 提交明确控制 |
| `get_weather` | 查询指定位置的 demo 模拟天气 |
| `set_light` | 按确切 `device_id` 和房间设置演示灯状态和亮度 |
| `set_ac` | 按确切 `device_id` 和房间设置演示空调状态、模式、风速和温度 |
| `run_scene` | 执行睡眠、电影、离家、回家场景 |
| `set_timer` | 创建 demo 定时器或提醒 |
| `list_timers` | 查询当前 demo 定时器 |
| `cancel_timer` | 按 id 取消 demo 定时器 |
| `read_skill` | 按 Skill 名称读取 `/data/res/skills/` 中的完整策略 |
| `user_memory` | 记住明确长期偏好、分页查看或忘记指定偏好 |

来源与目标不明确时先查询、再澄清，不通过开机或拍照探测意图。来源不改变现有权限分组。
部署与板端验收见 [设备来源与澄清规则](docs/device-routing.md)。

`get_weather` 当前返回结构化的模拟天气数据，便于展示 Agent 如何把室外天气和室内设备控制结合起来。
`set_timer`/`list_timers`/`cancel_timer` 当前使用 demo 内存表记录 timer，不依赖后台 cron service，
也不会在到期时自动触发真实设备动作；后续可在工具内部替换为 openvela timer、cron service 或云端调度。

工具调用链示例：

```text
home> 我要睡觉了

MODEL_REQ
MODEL_RESP
TOOL_CALL run_scene {"scene":"sleep"}
TOOL_RES  run_scene {"bedroom":...}
MODEL_REQ
MODEL_RESP
assistant: 已进入睡眠模式...
```

## Skills

demo 从 `/data/res/skills/` 加载 Markdown Skill，并注册 `read_skill` 只读工具用于按需读取完整策略。
当前资源目录包含：

| Skill | 用途 |
|-------|------|
| `smart_home_device_control` | 灯光、空调和设备目录的控制策略 |
| `smart_home_safety` | 亮度、温度和安全边界 |
| `smart_home_scenes` | 睡眠、电影、离家、回家等场景映射 |
| `smart_home_timer` | demo 定时器/提醒的行为边界 |
| `smart_home_weather` | 模拟天气与家居调节建议 |
| `smart_home_memory` | 长期偏好、临时指令、敏感内容和遗忘规则 |

模型在决定调用 `run_scene`、`set_light`、`set_ac`、`get_home_status`、timer/weather 工具或需要补充策略时，可通过 `read_skill` 参考这些 Skill。

## 设备模型

虚拟状态当前使用动态设备表，而不是写死三个字段：

```text
devices[]:
  id
  room        living_room | bedroom
  name
  type        light | ac
  on
  brightness
  temperature
  mode
  fan_speed
```

默认启动时创建：

| 房间 | 设备 |
|------|------|
| `living_room` | `Living Light` |

Panel 页支持：

- `All/Living/Bedroom` 按房间筛选设备。
- 点击 `Add Device` 新增灯或空调。
- 点击设备卡片进入控制弹窗。
- 控制弹窗可开关设备、调整亮度/温度/空调模式/风速。
- 控制弹窗的 `Edit` 可修改设备名称和房间。
- 控制弹窗的 `Delete` 可删除设备。

当前 LLM-visible tools 仍保持精简 schema：`set_light`/`set_ac` 通过 `room + type`
控制该房间第一个同类设备。Panel 本地控制则按 `device_id` 精确控制单个设备。
这样 demo 可以展示设备目录管理，又不会让 ReAct 工具 schema 过早膨胀。

场景在 `src/device/smart_home_device.c` 中实现。

## 构建与运行

smart_home 当前维护两条主要运行路径：

| 平台 | 构建配置 | 网络 | 资源部署 |
|------|----------|------|----------|
| Goldfish 模拟器 | `vendor/openvela/boards/vela/configs/goldfish-smart_home/` | Ethernet `eth0` | ADB 推送到 `/data/res/` |
| ESP32-S3-BOX-3 | `vendor/espressif/boards/esp32s3/esp32s3-box-3/configs/smart_home/` | Wi-Fi `wlan0` | LittleFS data 镜像烧录 |

详细排障、跨平台切换和历史问题见 [docs/dual-platform-guide.md](docs/dual-platform-guide.md)。README 只保留常用流程。

### 环境要求

- Linux 主机，已完成 openvela 工具链安装和 `source build/envsetup.sh`。
- Goldfish 需要 QEMU/emulator、ADB 和 `aarch64-none-elf-gcc`。
- ESP32-S3-BOX-3 需要 `esptool`、串口工具和 Xtensa/ESP32-S3 构建依赖。
- cAGENT 位于 `packages/cAGENT/`，是本项目内自研的 Agent core；其架构和部分运行时实现经验参考 openvela `packages/ai_agent`。

### API Key

内置后端（DeepSeek、MiMo、Qwen、OpenAI）的 API key 仅从运行时
`/data/smart_home/secrets.json` 的 `model_api_keys.<backend_id>` 读取。BOX-3 的
打包输入为 `res/config/secrets.json`，LittleFS 脚本使用 `WITH_SECRETS=1` 才会将其放入
数据分区；Goldfish 则通过 ADB 推送到相同运行时路径。

设置页选择内置 preset 时会自动填充 host/path/port/model，并显示密钥“已配置/未配置”；
不会显示、编辑或沿用前一个后端的 API key。`Custom` 允许填写只在当前运行期间有效的 key，
不会保存到 `settings.json`。不要把真实 API key 提交到 Git；已经暴露过的 key 应在服务商
后台吊销并重新生成。产品化应改用芯片安全存储。

### Wi-Fi

Goldfish 模拟器使用 Ethernet `eth0`，不需要 Wi-Fi 配置。ESP32-S3-BOX-3 真机需要在启动 `smart_home` 前完成 Wi-Fi 配置。

可以在设备 `/data` 分区写入运行时配置：

```bash
echo '{"ssid":"你的WiFi名称","password":"你的WiFi密码"}' > /data/wapi.conf
```

也可以修改 Kconfig 默认值后重新构建：

```text
CONFIG_SMART_HOME_WIFI_SSID="你的WiFi名称"
CONFIG_SMART_HOME_WIFI_PASSWORD="你的WiFi密码"
```

### 资源路径

运行时统一读取：

```text
/data/res/
  skills/*.md
  icons/*.png
  fonts/MiSans-Normal.ttf   # 可选，英文 UI 可回退到内置 Montserrat
```

`CONFIG_SMART_HOME_DEMO_DATA_ROOT` 默认是 `/data`。Skill loader 会优先读取 `/data/res/skills`，并兼容 `/data/res/res/skills` fallback；LVGL 图标默认读取 `/data/res/icons`，也兼容 `/data/res/res/icons` fallback。

Goldfish 推送资源：

```bash
adb push packages/demos/smart_home/res /data/
```

ESP32-S3-BOX-3 生成并烧录 LittleFS data 镜像：

```bash
WITH_ICONS=1 bash openvela_smarthome/scripts/make_box3_littlefs_data_image.sh
esptool --chip esp32s3 --port /dev/ttyACM0 --baud 921600 \
  write-flash 0xe00000 out/box3_littlefs_data/data_lfs.bin
```

### Goldfish 模拟器

构建：

```bash
cd openvela
source build/envsetup.sh

# 首次构建或从 ESP32-S3 切换到 Goldfish 时建议清干净
make -C nuttx distclean

./build.sh vendor/openvela/boards/vela/configs/goldfish-smart_home/ -j$(nproc)
```

如果使用 in-tree `build.sh`，模拟器启动目录需要包含 `nuttx`、`.config` 和 `vela_*.bin`：

```bash
mkdir -p cmake_out/vela_goldfish-arm64-v8a-ap
cp nuttx/.config cmake_out/vela_goldfish-arm64-v8a-ap/
cp nuttx/vela_*.bin cmake_out/vela_goldfish-arm64-v8a-ap/
ln -sf ../../nuttx/nuttx cmake_out/vela_goldfish-arm64-v8a-ap/nuttx

./emulator.sh cmake_out/vela_goldfish-arm64-v8a-ap
```

也可以用 CMake 构建直接生成对应目录：

```bash
./build.sh vendor/openvela/boards/vela/configs/goldfish-smart_home/ --cmake -j$(nproc)
./emulator.sh cmake_out/vela_goldfish-arm64-v8a-ap
```

启动后在 NSH 中联网和运行：

```text
goldfish-armv8a-ap> ifup eth0
goldfish-armv8a-ap> renew eth0
goldfish-armv8a-ap> smart_home "打开客厅灯，亮度35%"
goldfish-armv8a-ap> smart_home
```

无参数运行进入 LVGL UI；带参数运行会直接执行一次请求，适合命令行验证。System Status 会把模拟器显示为 `Platform: Simulator`、`Network: Online (eth0)`、`Wi-Fi: N/A`。

### ESP32-S3-BOX-3 真机

构建和烧录：

```bash
cd openvela
source build/envsetup.sh

# 日常重构建可只删 .config；跨架构切换时请 distclean
rm -f nuttx/.config

./build.sh \
  vendor/espressif/boards/esp32s3/esp32s3-box-3/configs/smart_home/ \
  -j$(nproc)

cd nuttx
make flash ESPTOOL_PORT=/dev/ttyACM0 ESPTOOL_BAUD=921600
cd ..
```

首次构建 ESP32-S3-BOX-3 时 HAL 可能需要补丁脚本；详见 [docs/dual-platform-guide.md](docs/dual-platform-guide.md#63-esp-hal-补丁)。

板端 Wi-Fi 和运行：
连接手机热点
```text
nsh> ifup wlan0
nsh> wapi psk wlan0 88888888 3
nsh> wapi essid wlan0 123 1
nsh> renew wlan0

nsh> smart_home
nsh> smart_home "打开客厅灯，亮度35%"
```
或者实验室网络
```text
nsh> ifup wlan0
nsh> wapi psk wlan0 SWUNBS552 3
nsh> wapi essid wlan0 BS552 1
nsh> renew wlan0

nsh> smart_home
nsh> smart_home "打开客厅灯，亮度35%"
```


真机 System Status 会把平台显示为 `ESP32-S3 Wi-Fi`，网络行显示当前可达性，例如 `Online (wlan0)` 或 `DNS failed (wlan0)`；Wi-Fi 行只显示真机无线关联细节。

### UI 模式

UI 后端是编译期 choice：

| 配置项 | 说明 |
|--------|------|
| `CONFIG_SMART_HOME_DEMO_UI_CONSOLE=y` | 终端 UI，进入 `home>` 交互循环 |
| `CONFIG_SMART_HOME_DEMO_UI_LVGL=y` | LVGL 面板/对话/设置三屏 UI |

编译 LVGL 后，无参数 `smart_home` 启动图形 UI；带参数 `smart_home "..."` 会走一次性请求路径，便于在 NSH 中快速验证 Agent。编译 Console 后，无参数进入终端交互。

LVGL Panel 支持模拟家庭环境变化。点击 `Temp`、`Humidity`、`Light` 三个环境入口可以修改虚拟传感器值。环境值会进入 `get_home_status` 和 cAGENT context，Agent 可以基于新的温度、湿度、环境光状态决定是否调用灯光或空调工具。环境值代表传感器输入，不提供 LLM-visible `set_environment` 工具。

终端 UI 本地命令：

```text
home> env
environment: Temp 26C | Hum 45% | Light 300lx
home> env temp 31
home> env hum 60
home> env light 30
home> node status
  node_living_sensor_get_temperature [enabled]: Read the current sensor temperature in degrees Celsius.
Node: 1 registered tool(s).
home> mcp status
MCP: Manual (last_error=0)
home> mcp discover
MCP discovery requested. Use 'mcp status' to check progress.
home> mcp status
MCP: Connected (last_error=0)
home> 打开客厅灯，亮度 35%
home> 10 分钟后提醒我关灯
home> quit
```

`env temp <0-45>`、`env hum <0-100>`、`env light <0-1000>` 只修改模拟传感器状态，不经过 LLM 工具调用。

`node status` 只读列出当前已认证并注册到 cAGENT 的 `node_*` 工具；没有工具时显示
`Node: no registered tools.`，需启用 `CONFIG_SMART_HOME_NODE_GATEWAY=y`。`mcp discover`
触发一次 MCP 工具发现，`mcp status` 查询其状态，需启用
`CONFIG_SMART_HOME_MCP_BRIDGE=y`。

### Kconfig 关键项

`CONFIG_SMART_HOME_DEMO` 会自动选择 cAGENT 核心库、OpenAI-compatible model provider 和 openvela TLS runtime。常见关键项如下：

```text
CONFIG_SMART_HOME_DEMO=y
CONFIG_SMART_HOME_DEMO_DATA_ROOT="/data"
CONFIG_CAGENT_MODEL_OPENAI=y
CONFIG_CAGENT_RUNTIME_OPENVELA_TLS=y
```

Goldfish 侧通常使用 Ethernet，不需要 ESP32-S3 Wi-Fi/WAPI：

```text
CONFIG_SMART_HOME_DEMO_UI_CONSOLE=y
# CONFIG_ESP32S3_WIFI is not set
# CONFIG_WIRELESS_WAPI is not set
```

ESP32-S3-BOX-3 LVGL 真机侧需要显示、触摸、Wi-Fi、LittleFS data 分区：

```text
CONFIG_SMART_HOME_DEMO_UI_LVGL=y
CONFIG_BOARD_LATE_INITIALIZE=y
CONFIG_BOARDCTL=y
CONFIG_GRAPHICS_LVGL=y
CONFIG_LV_COLOR_16_SWAP=y
CONFIG_LV_USE_NUTTX_LCD=y
CONFIG_LV_USE_NUTTX_TOUCHSCREEN=y
CONFIG_LV_USE_LODEPNG=y
CONFIG_LV_USE_FS_POSIX=y
CONFIG_LV_FS_POSIX_LETTER=65
CONFIG_ESP32S3_WIFI=y
CONFIG_WIRELESS_WAPI=y
CONFIG_ESP32S3_SPIFLASH_LITTLEFS=y
CONFIG_ESP32S3_SPIFLASH_LITTLEFS_MOUNTPT="/data"
```

### 平台切换注意事项

ESP32-S3 是 Xtensa，Goldfish 是 arm64。跨平台切换时必须清理旧架构产物：

```bash
make -C nuttx distclean
```

如果只删 `.config`，旧 `.o` 或 `include/arch` 链接可能残留，表现为 `xtensa/core.h`、`include/arch/barriers.h` 或奇怪的 undefined reference 错误。

常见问题速查：

| 现象 | 原因 | 处理 |
|------|------|------|
| `File Make.defs could not be found` | 直接使用了本仓库 `board/goldfish-arm64` 路径作为构建配置 | 将 defconfig 软链接到 `vendor/openvela/boards/vela/configs/goldfish-smart_home/` 后再构建 |
| `could not load kernel` | `cmake_out/.../nuttx` 不存在或软链接断链 | 检查 `ln -sf ../../nuttx/nuttx .../nuttx` |
| `smart_home_agent_app_init failed: -13` | `/data/res/skills` 未部署 | ADB 推送资源或烧录 LittleFS |
| Goldfish 显示 Wi-Fi 失败 | 旧版本把 Wi-Fi 状态当网络状态 | 当前代码区分 `eth0` 与 `wlan0`，请重新构建 |
| ESP32-S3 白屏 | LCD 字节序/显示配置不完整 | 检查 `LV_COLOR_16_SWAP` 和板级 LCD 配置 |

## 范围

第一版为仅含虚拟设备的骨架：

- 灯光：客厅、卧室
- 空调：卧室
- 场景：睡眠、电影、离家、回家
- 工具：家居状态、灯光、空调、场景、天气、timer 和 Skill 读取

暂不包括：

- 真实设备驱动
- 持久化家居状态
- 账号/用户权限模型
- 多房间发现

LVGL 图形化 UI 目前已经按页面拆分为面板、对话、设置和公共样式/Agent 桥接模块。后续新增设备、工具调用详情或模型配置表单，应优先扩展对应页面模块，避免重新堆回单个大文件。

以上应在不改动 `agent/` 中 cAGENT 集成形态的前提下，通过扩展 `device/`、`ui/` 和已注册的 Skill 来添加。
