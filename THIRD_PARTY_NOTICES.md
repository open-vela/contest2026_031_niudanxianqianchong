# 第三方依赖与许可证声明

P4 依赖最后核对：2026-08-19；cAGENT V2 包替换记录：2026-10-25。

本文件记录本仓 ESP32-P4 Function EV Board 移植的已识别依赖。它不替代各
依赖随源代码分发的 `LICENSE`、`NOTICE` 或版权声明；再分发固件或源代码时，
应一并保留适用的上游声明。

## P4 最小 NSH 构建

| 组件 | 用途与版本 | 许可证 | 来源/声明 |
| --- | --- | --- | --- |
| Apache NuttX / openvela | 本仓 P4 芯片、共享层和板级代码的上游基础 | Apache-2.0 | 工作区 `nuttx/LICENSE`、`nuttx/NOTICE` 与 `apps/LICENSE`、`apps/NOTICE` |
| Espressif `esp-hal-3rdparty` | ESP32-P4 HAL、SDK 头文件及库 | Apache-2.0 | `https://github.com/espressif/esp-hal-3rdparty`，固定提交 `b90b1837cb5ad24747deb4c895246037cc206ce5`；许可证随该依赖提供 |

本仓 `chips/esp32p4/` 与 `board/esp32p4/` 中来自 Apache NuttX 的文件保留了
原有 SPDX、版权和 NOTICE 声明。`esp-hal-3rdparty` 是构建时拉取的依赖，不作为
本仓受跟踪源代码提交；其兼容补丁位于
`chips/esp32p4/common/espressif/patches/`，补丁本身随本仓以 Apache-2.0 发布。

## cAGENT V2 包

`packages/cAGENT/` 已替换为 cAgentV2 的本地快照（原 `packages/cAgent2/`）；
基线与额外未提交来源见包内 `V2_SNAPSHOT_COMMIT`。V2 Core、OpenAI Provider
及大部分端口保留 MIT 许可证，见包内 `LICENSE` 和各文件 SPDX 声明。
`ports/tls/src/tls_core.c` 保留 Apache-2.0 声明，其源码血缘见
`ports/tls/README.md`；Apache-2.0 全文随本仓根目录 `LICENSE` 提供。

## 本仓许可证范围

除保留自身许可证或版权声明的第三方材料外，本仓新增的原创代码、构建配置、
兼容补丁和文档以根目录 [LICENSE](LICENSE) 的 Apache License 2.0 发布。上游
衍生文件继续受其文件头及上游 NOTICE 的约束。
