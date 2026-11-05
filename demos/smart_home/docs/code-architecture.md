# smart_home 代码架构与调用关系

> 文档状态：2026-08-06
> 适用范围：`demos/smart_home`、`packages/cagent_addons` 与 smart_home 使用到的
> `packages/cAGENT` 公共接口。
> 当前工作区为 MCP 隔离实验，Goldfish smart_home defconfig 暂时关闭了
> `CONFIG_SMART_HOME_MCP_BRIDGE`；本文描述的 MCP 代码路径仍然保留。

## 1. 总体结构

smart_home 是应用装配层，cAGENT 是推理和工具核心，`cagent_addons` 是 Node/MCP
远程工具扩展层。cAGENT core 不知道智能家居设备、Node、MCP 或 LVGL 的业务含义。

```text
                           +-----------------------------+
用户触摸/键盘  ----------> | smart_home LVGL UI           |
手机/命令行  ------------> | panel / chat / settings     |
                           +--------------+--------------+
                                          |
                              smart_home_agent_run()
                                          |
+------------------+       +--------------v--------------+
| 本地工具         |------>| cAGENT agent_run/ReAct loop |
| smart_home_tools |       | context + skills + model    |
| device context   |       | tool registry + tool guard  |
+------------------+       +----+-------------+-----------+
                                  |             |
                    agent_tool_t |             | model provider
                                  |             v
                                  |       OpenAI-compatible HTTP
                                  |
       +--------------------------+-------------------------+
       |                                                    |
+------v---------------+                         +----------v----------+
| Node gateway glue    |                         | MCP bridge glue     |
| smart_home_node_     |                         | smart_home_mcp_     |
| gateway.c            |                         | bridge.c            |
+------+---------------+                         +----------+----------+
       | remote catalog + mutation worker                    |
       |                                                    |
+------v---------------+                         +----------v----------+
| WS transport         |                         | Streamable HTTP/TLS |
| OpenClaw-compatible  |                         | initialize/list/call |
+------+---------------+                         +----------+----------+
       |                                                    |
 sensor_node / Node                                  standard MCP server
```

## 2. 启动调用链

入口是 [`smart_home_main.c`](../src/app/smart_home_main.c)。无命令行参数时，启动
LVGL；带参数时走一次性命令行对话。

```text
main()
 ├─ smart_home_network_init()
 ├─ smart_home_agent_app_init()
 │   ├─ smart_home_status_init()
 │   ├─ smart_home_device_init()
 │   ├─ smart_home_skill_store_init()
 │   ├─ agent_create_simple()
 │   ├─ smart_home_device_register_context()
 │   ├─ smart_home_skills_register()
 │   ├─ smart_home_tools_register()
 │   ├─ agent_set_event_callback()
 │   ├─ agent_model_openai_create()
 │   ├─ agent_set_model_owned()
 │   ├─ smart_home_agent_app_apply_model_config()
 │   ├─ smart_home_node_gateway_start()   [可选]
 │   └─ smart_home_mcp_bridge_start()    [可选、仅装配]
 ├─ smart_home_lvgl_run()                [无 argv]
 │   ├─ lv_init()/smart_home_lvgl_style_init()
 │   ├─ lv_nuttx_init() -> display/input
 │   ├─ smart_home_lvgl_init()
 │   │   ├─ build_panel_screen()
 │   │   ├─ build_chat_screen()
 │   │   └─ build_settings_screen()
 │   ├─ agent_set_event_callback(..., smart_home_lvgl_event_cb)
 │   └─ LVGL timer loop 或 libuv loop
 └─ smart_home_agent_app_deinit()
     ├─ stop MCP / Node
     ├─ agent_destroy()
     └─ destroy skill store and mutex
```

`smart_home_agent_app_init()` 只负责装配。MCP 的 TLS、`initialize` 和
`tools/list` 仅在 Settings 页请求 Discover MCP 后，才由 operation worker 执行；网络
失败不会阻止 LVGL、本地工具或 Node gateway。

## 3. cAGENT 在应用中的使用

### 3.1 Agent、模型和会话

smart_home 只使用公共头文件 [`agent.h`](../../../packages/cAGENT/include/agent.h)，
不依赖 cAGENT 的 `src/` 内部头文件。

| 调用 | 用途 |
| --- | --- |
| `agent_create_simple()` | 创建 Agent 和系统提示词 |
| `agent_model_openai_create()` | 创建 OpenAI-compatible 模型适配器 |
| `agent_set_model_owned()` | 将模型所有权交给 Agent |
| `agent_model_openai_set_backend()` | 设置 host/path/port |
| `agent_model_openai_set_api_key()` | 设置模型 API key |
| `agent_model_openai_set_model()` | 设置模型名称 |
| `agent_set_limits()` | 设置超时、输出 token 和请求预算 |
| `agent_run()` | 同步执行一轮 ReAct 推理 |
| `agent_set_event_callback()` | 将运行、模型、工具事件转给 UI |

`smart_home_agent_run()` 使用固定会话 ID `SMART_HOME_SESSION_ID`。如果 Node 或
MCP 开启，应用用同一把 `agent_mutex` 串行保护 `agent_run()`、工具启停和远程工具
注册/注销，避免核心 registry 在并发修改时被访问。

### 3.2 本地工具注册

工具定义是 [`agent_tool_t`](../../../packages/cAGENT/include/cagent/tools.h)。cAGENT
注册表只保存结构体的浅拷贝，字符串和 `user_data` 必须在工具生命周期内有效。

smart_home 本地工具在 [`smart_home_tools.c`](../src/tools/smart_home_tools.c) 中
逐个构造完整 `agent_tool_t`，然后调用 `agent_register_tool()`：

```text
smart_home_tools_register()
 ├─ get_home_status  -> status_tool
 ├─ get_weather      -> weather_tool
 ├─ set_light        -> set_light_tool
 ├─ set_ac           -> set_ac_tool
 ├─ run_scene        -> run_scene_tool
 ├─ set_timer        -> set_timer_tool
 ├─ list_timers      -> list_timers_tool
 └─ cancel_timer     -> cancel_timer_tool
```

Skill loader 另外注册 `read_skill`，因此当前本地工具总数为 9 个。每个工具包含：

- `name`：模型调用的稳定名称；
- `description`：进入模型 tools schema；
- `input_schema_json`：严格的 JSON Schema；
- `execute`：`agent_tool_fn` 回调；
- `user_data`：通常指向 `smart_home_state_t` 或 Skill store；
- `flags`：`LLM_VISIBLE`、`READ_ONLY` 或 `SIDE_EFFECT`；
- `group_id`：来源/生命周期分组；
- `category_id`：smart_home 展示分类。

当前应用约定：

```c
#define SMART_HOME_TOOL_GROUP_LOCAL 0u
/* category: QUERY=1, CONTROL=2, SKILL=3 */
```

`group_id` 和 `category_id` 由应用定义，cAGENT core 只保存，不解释其业务语义。
风险控制使用通用 `flags`，不能用 category 代替安全属性。

### 3.3 设备上下文与 Skills

`smart_home_device_register_context()` 注册一个可选的 context provider。每轮模型
调用前，cAGENT 调用 provider 生成当前设备状态 JSON，加入上下文；它不是一个可调用
工具，也不占工具槽位。

`smart_home_skills_register()` 从 `/data/res/skills`（失败时回退到
`/data/res/res/skills`）加载 Markdown skill，并注册 `read_skill` 工具。Skill 是
模型行为指导，设备状态仍由 context provider 提供。

### 3.4 工具执行路径

```text
agent_run()
 ├─ context_builder: system prompt + device context + skills
 ├─ tool_schema: 收集启用且 LLM_VISIBLE 的 agent_tool_t
 ├─ model.complete()
 ├─ 解析 tool_calls
 ├─ tool_guard / policy 检查 flags、超时、调用次数
 ├─ tool.execute(call, result, user_data)
 ├─ 将 JSON result 放回会话
 └─ 继续 ReAct 或生成最终文本
```

注意：cAGENT 的工具超时目前主要是执行后测量，不能强行中断已经阻塞的回调。
Node/MCP 远程执行因此必须在 addon 自己实现网络 deadline。

## 4. LVGL UI 调用关系

LVGL 模块位于 [`src/ui/lvgl/`](../src/ui/lvgl/)，分为：

| 文件 | 责任 |
| --- | --- |
| `smart_home_lvgl.c` | LVGL/NuttX 初始化、屏保与三个产品页面、主事件循环 |
| `smart_home_lvgl_home_v9.c` | V9 首页、设备卡片与场景 |
| `smart_home_lvgl_device_dialog.c` | V9 设备控制抽屉 |
| `smart_home_lvgl_device_refresh.c` | 设备刷新与米家版本轮询 |
| `smart_home_lvgl_shell.c` | 公共顶栏、天气与时钟 |
| `smart_home_lvgl_chat.c` | 消息气泡、工具调用时间线、输入框 |
| `smart_home_lvgl_settings_v9.c` | 模型、工具开关、系统状态、Node/MCP 状态 |
| `smart_home_lvgl_agent.c` | Agent worker、事件复制、`lv_async_call` 回写 |
| `smart_home_lvgl_nav.c` | Home/Chat/Settings 导航 |
| `smart_home_lvgl_style.c` | 字体、颜色、图标和公共样式 |
| `smart_home_lvgl_internal.h` | 屏幕尺寸和模块内部声明 |

Agent 的事件回调可能来自 Agent worker 或远程 mutation 线程，不能直接操作 LVGL
对象；UI 代码将事件复制到异步队列，再在 LVGL 线程中更新控件。

## 5. Node 多设备调用链

### 5.1 smart_home gateway 装配

入口是 [`smart_home_node_gateway.c`](../src/addons/smart_home_node_gateway.c)：

```text
smart_home_node_gateway_start()
 ├─ load_shared_token(/data/smart_home/secrets.json)
 ├─ caddons_remote_catalog_create()
 ├─ caddons_ws_transport_create()
 ├─ caddons_node_gateway_create()
 │   ├─ bind_host / port=18790 / path=/
 │   ├─ shared token
 │   └─ catalog_changed callback
 ├─ start mutation_worker
 └─ caddons_node_gateway_start()
```

Node gateway 监听 WebSocket。底层 `caddons_ws_transport` 负责 TCP、RFC 6455
握手、masked client frame、ping/pong 和 peer 生命周期；`caddons_node_gateway`
负责 OpenClaw-compatible Node 消息、认证、工具发现和远程调用。

### 5.2 Node 工具注册

```text
sensor_node
 └─ WS hello/connect + tool metadata
     └─ node_gateway catalog discover()
         └─ catalog_changed -> sem_post()
             └─ mutation_worker
                 └─ catalog_next_mutation()
                     └─ catalog_apply_mutation(agent)
                         └─ agent_register_tool(agent, remote_tool)
```

远程 descriptor 保存 `route_id`、`source_name`、`public_name`、schema、风险、
`connection_gen` 和执行函数。公共名称形如 `node_<node_id>_<command>`。Node 断开
时先进入 draining/unregister，旧连接的迟到 disconnect 不得删除新连接同名工具。

模型调用远程工具时，cAGENT 调用 remote tool execute；remote catalog 根据
`route_id + connection_gen` 找到 Node peer，经过 WS transport 发送 invoke，等待有界
响应，再把 JSON 结果返回给 cAGENT。

### 5.3 transport 抽象

[`transport.h`](../../../packages/cagent_addons/include/cagent_addons/transport.h) 定义
与协议无关的 peer/event 接口：

- `start_client()` / `start_server()`：客户端或 gateway 监听；
- `send()` / `close_peer()`：发送和关闭；
- `keepalive()`：WS ping 或未来 MQTT keepalive；
- `event_fn()`：CONNECTED、DATA、DISCONNECTED、ERROR；
- payload 仅在回调期间有效，上层需要保存必须复制；
- reader 回调不得执行慢操作，慢操作交给上层 worker；
- `stop()` 必须中断 I/O 并 join 所有 transport 线程。

当前实现是 WebSocket；MQTT 只需要实现相同 transport ops，Node catalog 和 smart_home
应用层不应感知具体线协议。

## 6. MCP 直连调用链

### 6.1 配置和线程

smart_home MCP glue 位于 [`smart_home_mcp_bridge.c`](../src/addons/smart_home_mcp_bridge.c)：

1. 读取非敏感 `/data/smart_home/mcp_bridge.json`；
2. 读取敏感 `/data/smart_home/secrets.json`，解析 `${VAR}` header 引用；
3. 校验 endpoint、allowlist、风险和 timeout；
4. 创建 `caddons_mcp_bridge`、remote catalog 和 idle operation worker；
5. Settings 页 Discover MCP 请求唤醒 operation worker；
6. operation worker 同步完成 MCP 网络发现，并负责写 cAGENT registry。

### 6.2 标准 MCP 请求顺序

```text
manual operation worker
 ├─ POST initialize
 │   └─ 保存 Mcp-Session-Id
 ├─ POST notifications/initialized
 ├─ POST tools/list
 │   └─ 只导入配置 allowlist 中的工具
 └─ catalog discover
     └─ mutation worker -> agent_register_tool(remote_tool)
```

工具调用时：

```text
agent_tool execute
 └─ caddons_mcp_bridge tools/call
     ├─ MCP JSON-RPC request id
     ├─ MCP-Protocol-Version header
     ├─ Mcp-Session-Id header
     ├─ Authorization / custom headers
     └─ response JSON -> cAGENT result
```

MCP 工具名称形如 `mcp_<server_id>_<tool_name>`，来源分组为 `CADDONS_TOOL_MCP`
（数值 20；smart_home 应用可映射为自己的 MCP group）。设备直接连接标准 MCP
Streamable HTTP server，不经过 sidecar，也不修改 cAGENT core。

### 6.3 HTTP/TLS 边界

MCP addon 不依赖具体网络库，只要求应用提供 `caddons_http_request_fn`。smart_home
实现该 callback：

- `http`：非阻塞 TCP、HTTP/1.1、`Content-Length`、连接关闭；
- `https`：复用 cAGENT OpenVela runtime 的 `ov_tls_*` mbedTLS 封装；
- 所有连接、写入、响应头和响应体都有容量/时间上限；
- session 404 会清空 session 并重新 initialize 一次。

MCP 当前是 tools-only Profile。服务端请求、无限 SSE/chunked 流和分页 cursor 等
高级能力不应被误认为已经完整支持，真实第三方服务仍需按验收矩阵测试。

## 7. 远程与本地工具的共同生命周期

```text
发现/注册
  transport 或 MCP discovery 线程
        |
        v
  remote_catalog（线程安全 descriptor/state/inflight）
        |
        | mutation semaphore
        v
  应用 mutation worker
        |
        | agent_mutex
        v
  cAGENT tool registry
        |
        v
  agent_run 生成模型 tools schema
```

本地工具在启动时直接注册；远程工具采用“先全量发现、再动态注册”的 MVP 方案，
暂未使用 cAGENT tool provider 懒发现机制。两类工具最终都进入同一个 cAGENT
registry，因此模型调用、flags 检查、启停和 UI 状态查询路径一致。

## 8. 配置与构建开关

主要开关位于 [`Kconfig`](../Kconfig)：

| 开关 | 作用 |
| --- | --- |
| `SMART_HOME_DEMO` | 编译 smart_home 应用 |
| `SMART_HOME_DEMO_UI_LVGL` | 选择 LVGL 入口 |
| `SMART_HOME_NODE_GATEWAY` | 编译 Node gateway glue，并依赖 cagent_addons Node |
| `SMART_HOME_MCP_BRIDGE` | 编译 MCP glue，并依赖 cagent_addons MCP |
| `SMART_HOME_*_REQUIRED` | 远程服务失败时是否拒绝应用启动 |
| `CAGENT_MAX_TOOLS` | 本地 + Node + MCP 的总工具槽位上限 |
| `CAGENT_ADDONS_MAX_NODES` | Node peer 数量上限 |
| `CAGENT_ADDONS_MAX_MCP_TOOLS` | MCP allowlist/导入数量上限 |

应用目录的 `CMakeLists.txt`/`Makefile` 负责编译 smart_home；
`packages/cagent_addons` 提供独立 host 构建和单元测试；`packages/cAGENT` 提供
核心静态库。远程 addon 只通过 cAGENT 公共 API 接入，不能 include cAGENT 的内部
实现头文件。

## 9. 测试入口与当前状态

host 侧 addon 测试位于 [`packages/cagent_addons/tests`](../../../packages/cagent_addons/tests)：

- `test_ws_frame`：WebSocket frame 编解码；
- `test_ws_transport_loopback`：transport loopback；
- `test_node_proto`：Node 消息解析；
- `test_node_gateway` / `test_node_end_to_end`：gateway 与 Node 生命周期；
- `test_remote_tool_lifecycle`：catalog mutation、注册、注销和重连；
- `test_mcp_bridge`：MCP JSON-RPC mock 流程。

设备侧联调顺序建议为：

1. 关闭 MCP，确认纯 LVGL 和本地工具；
2. 启用 Node，先验证 gateway 监听和 sensor_node 上线；
3. 使用 QEMU mock MCP server 验证 HTTP/plain MCP；
4. 最后验证真实 HTTPS MCP、证书、header、allowlist 和工具调用。

## 10. 当前边界与后续工作

- 本地 9 个工具保持细粒度，不聚合为单一 `home_control`；
- Node/MCP 目前是全量发现后动态注册，不是 provider 懒加载；
- cAGENT core 的 timeout 是事后 guard，远程 addon 必须自行实现 I/O deadline；
- 远程工具 schema 由 Node metadata 或 MCP `tools/list` 提供；空 schema 只能走兼容
  路径，演示应始终提供精确 schema；
- `group_id` 表示来源/生命周期，`category_id` 表示 UI 分类，`flags` 表示通用风险；
- Node 使用 WebSocket/OpenClaw-compatible 线协议，MQTT 通过 transport interface
  作为后续实现，不改变 catalog 或 Agent 调用链；
- MCP 仍需补齐真实 socket/TLS、故障注入、分页和 trace/audit 等验收项。

相关文档：

- [`node-gateway.md`](node-gateway.md)
- [`mcp-client.md`](mcp-client.md)
- [`cagent-addons-design-rev2.md`](../../../docs/design/cagent-addons-design-rev2.md)
- [`mcp-extension-plan.md`](../../../docs/plans/mcp-extension-plan.md)
- [`packages/cAGENT` API reference](../../../packages/cAGENT/docs/api_reference.md)
