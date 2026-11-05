# smart_home 环境模拟设计

> 历史设计文档：保留旧页面方案供追溯，不代表当前实现。当前产品使用 V9 首页、聊天和设置，代码入口见 [code-architecture.md](code-architecture.md)。


## 背景

当前 smart_home demo 已经可以通过 Agent 工具控制虚拟设备，例如开关灯、调整亮度、
开启空调和运行场景。但 demo 里的环境数据仍然偏静态，用户无法主动修改温度、湿度、
环境光等传感状态来模拟真实家庭环境变化。

这会限制 demo 的表达能力：

- Agent 可以执行设备动作，但缺少环境变化输入。
- 用户无法模拟“天气变热”“屋内变暗”“湿度升高”等场景。
- 面板更像设备遥控器，而不是智能家居中控。
- `get_home_status` 能返回设备状态，但环境状态不足以支撑更自然的 Agent 推理。

因此需要在 demo 中加入可编辑的环境模拟能力。

## 目标

第一阶段目标是让用户能在 LVGL Panel 中手动修改环境数据，并让这些数据进入
cAGENT context 和工具查询结果。

核心目标：

```text
用户在 Panel 修改环境值
  -> 更新 smart_home_state_t
  -> 刷新 LVGL 面板显示
  -> get_home_status 返回新环境值
  -> context provider 注入新环境值
  -> Agent 基于环境状态做判断和设备控制
```

非目标：

- 不接入真实传感器驱动。
- 不做环境数据持久化。
- 不让 LLM 直接伪造或修改传感器数据。
- 不引入复杂房间/设备发现模型。

## 状态模型

建议把状态分为两类。

### Device state

设备状态表示可控设备的当前状态，由 Agent 工具或 UI 控制改变：

```text
living_room.light.on
living_room.light.brightness
bedroom.light.on
bedroom.light.brightness
bedroom.ac.on
bedroom.ac.temperature
```

### Environment state

环境状态表示传感器输入或模拟传感器输入，由 UI/设备层修改：

```text
environment.temperature
environment.humidity
environment.ambient_light
```

第一版使用全局环境值，不区分房间：

```c
int env_temperature;   /* Celsius, 0-45 */
int env_humidity;      /* percent, 0-100 */
int env_light;         /* lux or normalized level, 0-1000 */
```

原因：

- UI 更简单，适合 466x466 或方形模拟器屏幕。
- demo 阶段重点展示 Agent 感知环境变化，而不是完整多房间传感网络。
- 后续可以平滑扩展到 per-room environment。

第二阶段可扩展为：

```text
living_room.environment.temperature
living_room.environment.humidity
living_room.environment.ambient_light
bedroom.environment.temperature
bedroom.environment.humidity
bedroom.environment.ambient_light
```

## 默认值和范围

建议默认环境：

| 字段 | 默认值 | 范围 | 单位 | 说明 |
|------|--------|------|------|------|
| `env_temperature` | 26 | 0-45 | C | 当前室内环境温度 |
| `env_humidity` | 45 | 0-100 | % | 当前室内湿度 |
| `env_light` | 300 | 0-1000 | lux | 当前环境光照强度 |

UI 上可以显示为：

```text
Temp 26C
Hum 45%
Light 300lx
```

## JSON 表达

`smart_home_device_build_status_json()` 建议输出：

```json
{
  "living_room": {
    "light": {
      "on": true,
      "brightness": 35
    }
  },
  "bedroom": {
    "light": {
      "on": false,
      "brightness": 0
    },
    "ac": {
      "on": true,
      "temperature": 25
    }
  },
  "environment": {
    "temperature": 26,
    "humidity": 45,
    "ambient_light": 300
  }
}
```

context provider 中继续复用当前状态 JSON：

```text
Current virtual home state JSON:
{...}
```

这样模型可以通过 `get_home_status` 或 context 看到同一份环境状态。

## Tool 边界

不新增 `set_environment` LLM 工具。这个约束不仅适用于第一版，也适用于后续
per-room environment、更多传感器或真实传感器接入阶段。

原因：

- 环境状态代表传感器输入，不是 Agent 可控动作。
- 让 LLM 修改环境值会混淆“感知”和“控制”的边界。
- demo 需要展示用户/设备侧环境变化驱动 Agent 行为，而不是模型自导自演。
- 真实产品中，温度、湿度、环境光等值应来自传感器、模拟器或测试注入，不应由
  LLM 通过工具写入。

第一版工具边界：

| 工具 | 是否修改环境 |
|------|--------------|
| `get_home_status` | 否，只读取 |
| `set_light` | 否，只修改设备 |
| `set_ac` | 否，只修改设备 |
| `run_scene` | 否，只修改设备 |

后续如果需要做测试自动化，可以增加设备层或测试层 internal-only API，但不要把它
注册为 LLM-visible tool：

```c
int smart_home_device_set_environment(smart_home_state_t *state,
                                      int temperature,
                                      int humidity,
                                      int ambient_light);
```

## UI 设计

Panel 页建议分成两个区域：

```text
Smart Home                         Ready

Devices
  [ Living Light ]  [ Bedroom Light ]
  [ Bedroom AC    ]

Environment
  Temp      26C      [-] [slider] [+]
  Humidity  45%      [-] [slider] [+]
  Light     300lx    [-] [slider] [+]

Panel              Chat              Settings
```

### 交互方式

第一版建议使用环境卡片 + 弹窗编辑，而不是在主屏放三个大 slider。

主屏：

```text
[ Temp 26C ] [ Hum 45% ] [ Light 300lx ]
```

点击任意环境卡片后打开编辑弹窗：

```text
Temperature
26C
[---------------- slider ----------------]
[Cancel]                         [Apply]
```

理由：

- 466x466 或方形屏空间有限，主屏直接放三条 slider 会挤占设备卡片。
- 弹窗复用现有设备控制弹窗模式，代码和交互一致。
- 主屏传感器条仍保持可扫描。

### 范围与步进

| 字段 | Slider 范围 | 推荐步进 | 显示 |
|------|-------------|----------|------|
| Temperature | 0-45 | 1 | `26C` |
| Humidity | 0-100 | 1 | `45%` |
| Light | 0-1000 | 10 | `300lx` |

LVGL slider 本身不提供步进时，可以在 `LV_EVENT_VALUE_CHANGED` 中做量化：

```text
light = (raw / 10) * 10
```

## Agent 行为

skills 中应补充环境策略，帮助模型理解环境值。

建议规则：

```text
- If temperature is above 28C and the user asks for comfort or cooling,
  prefer turning on bedroom AC or lowering AC target temperature.
- If temperature is below 18C, avoid cooling actions unless explicitly requested.
- If ambient_light is below 80 lux and the user asks for better visibility,
  prefer increasing light brightness.
- If humidity is above 75%, mention high humidity and suggest ventilation or
  dehumidification if such devices exist.
- Do not claim to have changed environment sensor values. Environment values
  are simulated sensor inputs controlled by the UI/device layer.
```

示例对话：

```text
用户在 Panel 将 Temp 调到 31C
user: 卧室有点热，帮我调舒服一点
assistant tool_call: set_ac {"room":"bedroom","on":true,"temperature":24}
assistant: 已打开卧室空调并设置为 24C。
```

```text
用户在 Panel 将 Light 调到 30lx
user: 房间太暗了
assistant tool_call: set_light {"room":"living_room","on":true,"brightness":70}
assistant: 已打开客厅灯并把亮度调到 70%。
```

## 代码改动建议

### device 层

修改：

```text
src/device/smart_home_device.h
src/device/smart_home_device.c
```

新增字段：

```c
int env_temperature;
int env_humidity;
int env_light;
```

新增 API：

```c
int smart_home_device_set_environment(smart_home_state_t *state,
                                      int temperature,
                                      int humidity,
                                      int ambient_light);
```

更新：

- `smart_home_device_init()`
- `smart_home_device_build_status_json()`
- context provider 输出

### UI 层

修改：

```text
src/ui/lvgl/smart_home_lvgl_panel.c
src/ui/lvgl/smart_home_lvgl.h
```

建议新增 UI state：

```c
lv_obj_t *env_temp_card;
lv_obj_t *env_hum_card;
lv_obj_t *env_light_card;
lv_obj_t *env_popup;
lv_obj_t *env_slider;
lv_obj_t *env_title;
lv_obj_t *env_value_label;
int env_pending_type;
int env_pending_value;
```

其中：

```text
0 = temperature
1 = humidity
2 = ambient_light
```

### tools 层

无需新增 LLM-visible tool，后续扩展也应保持这个边界。

只需要确认 `get_home_status` 返回的 JSON 已包含 environment。

### skills 层

修改：

```text
src/skills/smart_home_skills.c
```

补充环境策略，让模型知道如何根据温度、湿度、环境光做设备控制。

## README 更新

README 应补充一段：

```text
Panel 页支持模拟环境变化。点击 Temp / Hum / Light 卡片可以修改虚拟传感器值。
这些值会进入 get_home_status 和 cAGENT context，Agent 可基于环境状态调用设备工具。
环境值是传感器输入，不是 LLM 可直接修改的工具动作；后续也不应新增
LLM-visible `set_environment`。
```

## 验收标准

功能验收：

- Panel 显示 Temp/Hum/Light 三个环境值。
- 点击环境值可以打开编辑弹窗。
- Apply 后主屏环境值立即刷新。
- `get_home_status` 返回 environment JSON。
- 通过 Chat 询问当前环境，Agent 能读到最新环境值。
- 将温度调高后，用户要求“调舒服一点”，Agent 能调用 `set_ac`。
- 将环境光调低后，用户要求“太暗了”，Agent 能调用 `set_light`。

边界验收：

- 温度不会超过 0-45。
- 湿度不会超过 0-100。
- 环境光不会超过 0-1000。
- 修改环境值不触发 Agent tool call。
- Agent 不会通过 tool 修改环境值。

## 后续扩展

后续可以扩展：

- per-room environment。
- occupancy/motion 传感器。
- door/window 状态。
- PM2.5 / CO2 / noise level。
- 根据环境变化自动触发 Agent 建议，但不自动执行设备动作。
- 接入真实传感器驱动，将 UI 模拟值替换为硬件输入。

---

# 空调模式与风速扩展

当前 AC 工具只支持 `on` 和 `temperature` 两个参数。真实空调还有模式、风速、摆风等
设置。本节描述如何在不破坏现有 API 的前提下扩展 `set_ac` 工具和虚拟设备模型。

## 目标

```text
set_ac 增加 mode（制冷/制热/除湿/送风/自动）和 fan_speed（低/中/高/自动）两个
可选参数。未提供时使用默认值，完全向后兼容。
```

非目标：

- 不新增独立工具（如 `set_ac_mode`）。
- 不增加摆风、定时等低频参数（demo 阶段性价比低）。
- 不做多温区双温区空调。

## 状态模型

在 `smart_home_state_t` 中增加两个字段：

```c
typedef struct {
    /* ... 现有字段 ... */
    int bedroom_ac_mode;         /* 0=cool, 1=heat, 2=dry, 3=fan, 4=auto */
    int bedroom_ac_fan_speed;    /* 0=low, 1=medium, 2=high, 3=auto */
} smart_home_state_t;
```

初始值：

```c
state->bedroom_ac_mode = 0;       /* 制冷 */
state->bedroom_ac_fan_speed = 3;  /* 自动 */
```

核心状态机原则：

- AC 状态由 `on + mode + fan_speed + target_temperature` 共同组成。
- 未传入的 AC 参数应保持当前设备状态，而不是每次重置为默认值。
- 默认值只用于设备初始化，或字段没有历史值时。
- `on=false` 只关闭空调，不重置 mode、fan_speed、temperature。
- `dry` / `fan` 模式不使用 temperature 参数，但仍保留上一次 target_temperature。
- 从 `dry` / `fan` 切回 `cool` / `heat` / `auto` 时，可以继续使用保留的
  target_temperature。

Temperature 范围因模式不同而语义有差异：

| 模式 | 温度语义 | 范围 |
|------|---------|------|
| 制冷 (cool) | 目标制冷温度 | 16-30°C |
| 制热 (heat) | 目标制热温度 | 16-30°C |
| 除湿 (dry) | 不需要温度参数 | — |
| 送风 (fan) | 不需要温度参数 | — |
| 自动 (auto) | 目标温度 | 16-30°C |

因此 `temperature` 从 `required` 变为 `optional`——LLM 调用 `mode=fan` 时可以
不传温度值。

## Tool Schema 变更

```c
#define SCHEMA_SET_AC                                                       \
    "{\"type\":\"object\",\"properties\":{"                                 \
    "\"room\":{\"type\":\"string\",\"description\":\"Room name: bedroom\"}," \
    "\"on\":{\"type\":\"boolean\",\"description\":\"Whether AC is on\"},"    \
    "\"temperature\":{\"type\":\"integer\",\"description\":"                \
        "\"Target temperature for cool, heat, or auto mode\"},"              \
    "\"mode\":{\"type\":\"string\",\"enum\":"                               \
        "[\"cool\",\"heat\",\"dry\",\"fan\",\"auto\"],"                    \
        "\"description\":\"AC mode. fan and dry do not require temperature\"}," \
    "\"fan_speed\":{\"type\":\"string\",\"enum\":"                          \
        "[\"low\",\"medium\",\"high\",\"auto\"],"                          \
        "\"description\":\"Fan speed. Defaults to current value or auto\"}}," \
    "\"required\":[\"room\",\"on\"],"                                       \
    "\"additionalProperties\":false}"
```

**关键变化**：`required` 从 `["room","on","temperature"]` 缩短为 `["room","on"]`。
`temperature`、`mode`、`fan_speed` 均为可选——工具侧提供默认值。

## 工具函数变更

```c
static int set_ac_tool(const agent_tool_call_t *call,
                       agent_tool_result_t *result,
                       void *user_data)
{
    smart_home_state_t *state = (smart_home_state_t *)user_data;
    char room[32] = "";
    char mode_str[16] = "";
    char fs_str[16] = "";
    int temperature = state->bedroom_ac_temperature;
    int mode = state->bedroom_ac_mode;
    int fan_speed = state->bedroom_ac_fan_speed;
    int on = 0;
    int ret;

    ret = json_get_string(call->arguments_json, "\"room\"", room, sizeof(room));
    if (ret == AGENT_OK) {
        ret = json_get_bool(call->arguments_json, "\"on\"", &on);
    }
    if (ret == AGENT_OK) {
        /* optional fields: use defaults if not present */
        json_get_int(call->arguments_json, "\"temperature\"", &temperature);
        if (json_get_string(call->arguments_json,
                            "\"mode\"",
                            mode_str,
                            sizeof(mode_str)) == AGENT_OK && mode_str[0]) {
            mode = parse_ac_mode(mode_str);
        }
        if (json_get_string(call->arguments_json,
                            "\"fan_speed\"",
                            fs_str,
                            sizeof(fs_str)) == AGENT_OK && fs_str[0]) {
            fan_speed = parse_fan_speed(fs_str);
        }
        ret = smart_home_device_set_ac(state, room, on,
                                        mode, fan_speed, temperature);
    }

    snprintf(output, sizeof(output),
             "{\"ok\":%s,\"room\":\"%s\",\"on\":%s,"
             "\"temperature\":%d,\"mode\":\"%s\",\"fan_speed\":\"%s\"}",
             ret == AGENT_OK ? "true" : "false",
             room[0] ? room : "",
             on ? "true" : "false",
             temperature,
             ac_mode_name(mode),
             ac_fan_speed_name(fan_speed));

    result->status = ret;
    result->content_json = output;
    return ret;
}
```

**默认值策略**：LLM 说 `"把空调调到制冷 24 度"` 时不传 `fan_speed`，工具自动用
当前风速；如果设备还没有历史风速，则使用 "自动风速"。LLM 不需要每次调用都指定
全部 5 个参数。LLM 的 Skills 文本可以描述：
"未指定的空调参数保持当前值；如果没有当前值，模式默认制冷，风速默认自动。"

## 字符串到整数的映射

```c
/* mode */
static const char *ac_mode_name(int mode) {
    switch (mode) {
    case 0: return "cool";
    case 1: return "heat";
    case 2: return "dry";
    case 3: return "fan";
    case 4: return "auto";
    default: return "cool";
    }
}

static int parse_ac_mode(const char *name) {
    if (strcmp(name, "heat") == 0) return 1;
    if (strcmp(name, "dry")  == 0) return 2;
    if (strcmp(name, "fan")  == 0) return 3;
    if (strcmp(name, "auto") == 0) return 4;
    return 0; /* cool */
}

/* fan_speed */
static const char *ac_fan_speed_name(int speed) {
    switch (speed) {
    case 0: return "low";
    case 1: return "medium";
    case 2: return "high";
    case 3: return "auto";
    default: return "auto";
    }
}

static int parse_fan_speed(const char *name) {
    if (strcmp(name, "low")    == 0) return 0;
    if (strcmp(name, "medium") == 0) return 1;
    if (strcmp(name, "high")   == 0) return 2;
    return 3; /* auto */
}
```

## Device 层变更

```c
static int clamp_mode(int mode) {
    return (mode < 0 || mode > 4) ? 0 : mode;
}

static int clamp_fan_speed(int speed) {
    return (speed < 0 || speed > 3) ? 3 : speed;
}

int smart_home_device_set_ac(smart_home_state_t *state,
                             const char *room,
                             int on,
                             int mode,
                             int fan_speed,
                             int temperature)
{
    if (!state || !room) return AGENT_ERROR_INVALID;
    if (!room_is(room, "bedroom")) return AGENT_ERROR_NOTFOUND;

    state->bedroom_ac_on = on ? 1 : 0;
    if (!state->bedroom_ac_on) {
        return AGENT_OK;            /* off does not reset previous AC settings */
    }

    state->bedroom_ac_mode = clamp_mode(mode);
    state->bedroom_ac_fan_speed = clamp_fan_speed(fan_speed);
    if (mode != 2 && mode != 3) {   /* fan and dry modes don't use temperature */
        state->bedroom_ac_temperature = clamp_temperature(temperature);
    }
    return AGENT_OK;
}
```

## Context Provider + Status JSON 更新

需要同时更新：

- `smart_home_device_build_status_json()` — 输出 mode 和 fan_speed
- `build_device_context()` — context provider 中的状态文本

```json
{
  "bedroom": {
    "light": { "on": false, "brightness": 0 },
    "ac": {
      "on": true,
      "temperature": 24,
      "mode": "cool",
      "fan_speed": "auto"
    }
  }
}
```

## Sense 映射

```text
cool  = 制冷 (空调吹冷风, 降低室温)
heat  = 制热 (空调吹热风, 升高室温)
dry   = 除湿 (不调温度, 降低湿度)
fan   = 送风 (风扇模式, 不调温度)
auto  = 自动 (空调自行判断制冷/制热/除湿)

low    = 低速风
medium = 中速风
high   = 高速风
auto   = 自动风速
```

中文名称只是 UX / Skill 文字中的指示，不进入 JSON schema 枚举值。JSON schema 的
enum 保留英文，这样 LLM 传入的参数是机器可解析的标识符，减少多语言转换的歧义。

## Smart Home Safety Skill 更新

```text
Smart home safety rules:
- Keep AC temperature between 16C and 30C.
- Keep light brightness between 0 and 100.
- AC modes: cool (16-30C), heat (16-30C), dry (no temperature), fan (no temperature), auto (16-30C).
- Keep unspecified AC parameters unchanged. Only use cool mode and auto fan speed as initial defaults.
- Prefer get_home_status before answering questions about current state.
- Use tools for device changes; do not claim a device changed unless a tool succeeded.
```

## Scene 映射更新

场景执行时，也设置 mode、fan_speed 和 temperature：

| 场景 | on | mode | fan_speed | temperature |
|------|----|------|-----------|-------------|
| sleep | true | cool | auto | 26C |
| movie | true | cool | low | 24C |
| away | false | 保留当前值 | 保留当前值 | 保留当前值 |
| home | true | auto | auto | 25C |

## UI Panel 更新

设备控制弹窗中增加两个下拉选择器：

```
AC Popup:
  标题: Bedroom AC
  开关: [ON / OFF]
  Mode:   [cool ▼]     ← lv_dropdown, 5 选项
  Fan:    [auto ▼]     ← lv_dropdown, 4 选项
  Temp:   ○────────●─── 24°C   ← lv_slider (仅 cool/heat/auto 模式可见)
  [Cancel]          [Apply]
```

卡片显示：

```
ON 状态:  "❄\nBedroom AC\n● ON\nCool | 24°C | Auto"
OFF 状态: "❄\nBedroom AC\n○ OFF"
```

## 验收标准

- `set_ac` 接受 `mode` 和 `fan_speed` 可选参数。
- 不传 `mode` 时默认制冷模式，不传 `fan_speed` 时默认自动风速。
- 已有 AC 状态时，不传 `mode` / `fan_speed` / `temperature` 应保持当前值。
- `on=false` 时只关闭空调，不重置 mode、fan_speed、temperature。
- `mode=fan` 时不要求传入 `temperature`，工具侧忽略温度值。
- `mode=dry` 时不要求传入 `temperature`，工具侧保留上一次温度值。
- `get_home_status` 返回当前 mode 和 fan_speed。
- LLM 在 Skill 指导下正确使用新模式参数。
- UI Panel 控制弹窗可以切换模式、风速、温度。
- 场景 `run_scene` 同步设置 mode 和 fan_speed。

## 不做的事

- **定时**：属于 cron 系统。"2 小时后关空调"应通过 `cron_add` 工具实现。
- **摆风 / 风向**：demo 阶段性价比低，且 LLM 不需要理解空间几何来决策设备控制。
- **双温区 / 多空调**：卧室只有一台 AC，多设备扩展时 room 参数自然支持。
