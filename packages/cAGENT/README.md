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
- SmartHome、cagent_addons 和现有板级配置仍使用部分 V1 API/配置，尚待适配。
  当前包替换不表示 SmartHome 已可编译或运行。
- 下一步需迁移初始化/启动、模型 Provider、工具、Skill、Session、事件与
  TTS/ASR 的 HTTP/TLS 接入，并重新验证容量、固件构建和板端行为。
- 此快照仅提供 Make 接入；CMakeLists.txt 是占位文件，不提供 V2 库目标。

许可证
------

V2 Core、Provider 和大部分端口保留 MIT 许可证（见 `LICENSE`）。
`ports/tls/src/tls_core.c` 保留 Apache-2.0 声明；其源码血缘见
`ports/tls/README.md`，归属声明见 `NOTICE`，Apache-2.0 全文见仓库根目录 `LICENSE`。
