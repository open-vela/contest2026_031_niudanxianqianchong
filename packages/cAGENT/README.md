cAGENT（V2 内核）
================

[cAgentV2]（上游仓库 `~/Documents/C/cAgentV2`，MIT；`tls/src/tls_core.c`
为 Apache-2.0，血缘见 `ports/tls/README.md`）在比赛工程内的 vendor 快照，
作为 NuttX 包接入，统一维护在 `packages/cAGENT/`。
快照来源见 `V2_SNAPSHOT_COMMIT`。

目录结构（保持上游形态，未含 docs/tests/examples）：

    include/                     公共 API（agent.h + agent/*.h）
    src/                         可移植 Core（ReAct loop/session/tool/skill/...）
    codecs/json/                 私有有界 JSON 读写器（内嵌 jsmn）
    providers/model/openai/      OpenAI 兼容 Chat Completions provider
    ports/runtime|transport|tls/ openvela 三件套 + mbedTLS TLS 后端

旧版 cAGENT 的源码、公共头文件、构建配置和文档已从本目录移除。
本包使用 V2 API，不提供 V1 API 兼容层。容量项由本包 Kconfig 的 `AGENT_*` 符号直接驱动
（NuttX config.h → include/agent/config.h），Makefile 不做宏桥接。

构建接入：

- `Make.defs` 注册 CONFIGURED_APPS 并对外暴露 `include/`；
- 总开关为 `CONFIG_CAGENT`；组件开关为 `CONFIG_CAGENT_PROVIDER_OPENAI`
  和 `CONFIG_CAGENT_PORT_OPENVELA_RUNTIME|TRANSPORT|TLS`；
- `Makefile` 按以上组件开关组装源文件；
- `Kconfig` 提供总开关、组件开关与容量项；
- CMake 构建路径暂不提供（Make 为权威路径）。

[工程契约]: 上游更新时重新快照并同步覆盖本目录，构建四件
（Make.defs/Makefile/Kconfig/CMakeLists.txt）与 V2_SNAPSHOT_COMMIT 是本仓
维护文件，上游不含。

迁移状态（2026-10-25）
---------------------

- 原 `packages/cAgent2/` 已迁入本目录；同一固件只编入这一份 V2 内核。
- `demos/cagent2_chat/` 和配套 Goldfish defconfig 已调整包路径及组件开关，
  Demo 名称及 `CONFIG_CAGENT2_CHAT` 保持不变。
- SmartHome Agent 层已接入 V2 生命周期及模型后端；通过 Host/mock 验证和
  P4 对象编译。模型配置与缓冲区由应用持有，运行与配置切换串行执行。
- 工具、Skill、设备上下文、UI、语音、cagent_addons 和现有板级配置仍有
  V1 API/配置依赖。完整 SmartHome 固件及真机业务尚未通过验收。
- 下一步需迁移初始化/启动、模型 Provider、工具、Skill、Session、事件与
  TTS/ASR 的 HTTP/TLS 接入，并重新验证容量、固件构建和板端行为。
- 此快照仅提供 Make 接入；CMakeLists.txt 是占位文件，不提供 V2 库目标。
- Make 通过 `AGENT_BUILD_CONFIG_HEADER` 为库和调用方统一加载
  `nuttx/config.h`，保证 `CONFIG_AGENT_*` 容量配置一致。

Agent 层 Host 验证（在竞赛仓根目录执行）：

```sh
python3 scripts/test_smart_home_agent.py
# 使用现有 P4 .config 做对象编译，不修改配置
python3 scripts/test_smart_home_agent.py --p4
```

测试链接真实 V2 Core/Provider，HTTP、设备、Skill 和 UI 使用 mock；
覆盖模型配置生命周期、失败回退、工具权限、截断、资源清理及线程互斥，
并启用 ASan/UBSan。测试需要外层工作区的 cJSON 源码和支持 sanitizer 的 Host C 编译器。

许可证
------

V2 Core、Provider 和大部分端口保留 MIT 许可证（见 `LICENSE`）。
`ports/tls/src/tls_core.c` 保留 Apache-2.0 声明；其源码血缘见
`ports/tls/README.md`，归属声明见 `NOTICE`，Apache-2.0 全文见仓库根目录 `LICENSE`。
