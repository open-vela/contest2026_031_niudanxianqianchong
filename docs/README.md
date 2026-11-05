# 文档索引

本目录记录 **ESP32-P4 Function EV Board 上的 openvela 智能家居中控面板**。
截至 2026-11-03，已上板的能力：DSI 显示（`/dev/fb0` 1024×600 RGB565）、
GT911 触摸（`/dev/input0`）、SC2336 摄像头（`/dev/video0`，RGB565 30 fps）、
ES8311 录放、LVGL 9.1 界面、cAGENT 工具调用、云端 ASR/TTS（短句）、
端侧 KWS 单次推理（`kws_smoke`）、ESP-NN P4 内核（覆盖 58.3% MAC）。

尚未完成：KWS 常驻服务在任何 defconfig 中均未开启；TTS 长句正文接收超时；
camera 自测任务退出后复用旧帧；SMP 下的 KWS/ESP-NN 组合验证。

## 常用入口

| 需求 | 文档 |
| --- | --- |
| 第一次了解项目 | [SmartHome 整体架构](架构设计/ESP32-P4X-SmartHome整体架构.md) |
| 编译烧录 | [SmartHome 编译烧录与资源部署指南](开发指南/ESP32-P4X-SmartHome编译烧录与资源部署指南.md) |
| 当前待修问题 | [SmartHome 问题分析](架构设计/ESP32-P4X-SmartHome问题分析.md) |
| 语音与能力进展 | [语音与能力合入实施计划](开发计划/应用与AI/ESP32-P4X-SmartHome语音与能力合入实施计划.md) |
| 全部开发计划 | [开发计划索引](开发计划/README.md) |
| 第三方许可证 | [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md) |

## 架构设计

| 文档 | 内容 |
| --- | --- |
| [SmartHome 整体架构](架构设计/ESP32-P4X-SmartHome整体架构.md) | 硬件资源、七层分层、启动序列、内存布局、线程模型、数据流与控制流 |
| [SmartHome 问题分析](架构设计/ESP32-P4X-SmartHome问题分析.md) | P0–P3 分级问题、证据等级与处理顺序（基线 `491ffa9`） |
| [SmartHome 卡顿/线程/内存优化方案](架构设计/ESP32-P4X-SmartHome卡顿线程内存优化方案.md) | 帧管线开销、UI 阻塞、双核分工、优先级、栈与 PSRAM/SRAM 分区 |

## 开发指南

| 文档 | 内容 |
| --- | --- |
| [SmartHome 编译烧录与资源部署](开发指南/ESP32-P4X-SmartHome编译烧录与资源部署指南.md) | 16 MiB Flash 分区、构建、烧录与 `/data` 资源部署 |
| [SmartHome 一次性提醒实现与验证](开发指南/SmartHome一次性提醒实现与验证.md) | 倒计时、闹钟持久化、弹窗与本地响铃 |
| [SC2336 摄像头 Demo 接入](开发指南/ESP32-P4X-SC2336摄像头Demo接入指南.md) | 板级接口、CSI 接收 API、缓存同步 |
| [PPA 与 2D-DMA 构建验证](开发指南/ESP32-P4X-PPA与2D-DMA构建验证指南.md) | 图形加速通路构建与验证步骤 |
| [Session 持久化](开发指南/SmartHome-Session持久化.md) | `/data/smart_home/sessions` 的格式与恢复流程，实现细节见[修改说明](开发指南/SmartHome-Session持久化修改说明.md) |

## 硬件适配

| 文档 | 内容 |
| --- | --- |
| [P4 Function EV Board 适配](硬件适配/esp32p4-ev-board-adaptation.md) | 适配范围、目录映射、构建与排障 |
| [Route A 移植方案](硬件适配/esp32p4-function-ev-board-route-a-porting.md) | custom chip / custom board 架构与阶段目标 |
| [P4 最小 NSH 操作与测试](硬件适配/esp32p4-nsh-operation-and-test.md) | 构建、烧录及最小 NSH 上板验收 |
| [GT911 触摸适配](硬件适配/gt911适配.md) | I2C 双地址探测、触摸事件链与真机排障 |
| [ES8311 音频适配方案](硬件适配/ESP32-P4X-Function-EV-Board-ES8311音频适配方案.md) | Codec 配置、时钟与录放通路 |
| [SC2336 摄像头适配](硬件适配/ESP32-P4X-SC2336摄像头适配.md) | SCCB、MIPI-CSI、ISP 与 RAW 接收 |
| [SC2336 视频通路架构与调用链](硬件适配/ESP32-P4X-SC2336-视频通路架构与调用链.md) | 从 NSH 到面板的逐层调用路径 |
| [SC2336 适配交接记录](硬件适配/ESP32-P4X-SC2336适配交接记录.md) | 修复前快照与证据链（历史材料） |
| [SC2336 CSI RAW 接收交接](硬件适配/ESP32-P4X-SC2336-CSI-RAW接收交接.md) | 首帧超时证据与接手顺序（历史材料） |
| [新适配驱动板端验证](硬件适配/ESP32-P4X新适配驱动板端验证_20261104.md) | PPA/2D-DMA、ES8311 录放、SMP 双核实板结果与复现步骤 |

## 开发日志

按主题分目录，记录已发生的问题、命令与结论。追溯具体故障时按文件名检索。

| 主题 | 关键记录 |
| --- | --- |
| 构建与工具链 | [Kconfig 排障](开发日志/编译/README.md)、[乐鑫工具链 S0–S3](开发日志/编译/ESP32-P4X-乐鑫工具链S0-S3接入记录.md)、[DSI Host Probe](开发日志/编译/2026-08-21-DSI-Host-Probe排障记录.md)、[DSI 黑屏 DBI](开发日志/编译/2026-08-24-ESP32-P4X-DSI黑屏DBI配置排障闭环.md) |
| 显示与 UI | [framebuffer 验收](开发日志/编译/2026-08-24-ESP32-P4X-framebuffer真机验收.md)、[LVGL 静态首页](开发日志/编译/2026-08-24-ESP32-P4X-LVGL静态首页真机验收.md) |
| 音频 | [SMP ES8311 录放修复](开发日志/ESP32-P4X-SMP-ES8311录放修复记录_20261102.md) |
| 语音能力 | [分阶段验收](开发日志/ESP32-P4X-语音能力分阶段验收记录_20261102.md)、[ASR/TTS 审查修复](开发日志/ASR-TTS审查修复与验收_20261102.md)、[MiMo TTS 超时](开发日志/MiMo-V2.5-TTS正文接收超时问题总结_20261102.md) |
| 摄像头 | [CSI/DMA 数据流](开发日志/SC2336-CSI-DMA数据流与调用链.md)、[RAW 接收故障修复](开发日志/ESP32-P4X-SC2336-CSI-RAW接收故障修复.md)、[video_test 实现](开发日志/应用/ESP32-P4X-video_test实现方案.md) |
| SMP | [四项接入](开发日志/ESP32-P4X-SMP-四项接入记录_20261030.md)、[DSI 与 GT911 双核](开发日志/ESP32-P4X-SMP-DSI与GT911双核验证_20261029.md)、[GPIO IRQ 修复](开发日志/ESP32-P4X-GPIO-IRQ-SMP启动修复记录.md) |
| 米家与网络 | [C6 ESP-Hosted 控制面](开发日志/ESP32-P4X-C6-ESP-Hosted控制面与WLAN数据面开发记录.md)、[设备回读与多型号控制](开发日志/应用/2026-11-03-米家设备回读与多型号控制修复.md) |
| 存储 | [LittleFS 挂载失败](开发日志/ESP32-P4X-LittleFS挂载失败与修复.md) |
| 早期记录 | [P4 移植开发记录](开发日志/dev.md) |

## 文档边界

- 源码和配置以 `board/esp32p4/`、`chips/esp32p4/` 和 `demos/smart_home/` 为准，
  文档与之冲突时以代码为准确。
- 各文档文首的状态行为该文档的适用范围；标"方案"的文档描述设计意图，
  实施结果以开发日志中的板端记录为准。
- 本仓库不再包含 ESP32-S3 板级配置；涉及 S3 的描述仅作历史对照。

## 历史材料

[archive/](archive/) 保存旧 SmartHome 交付稿和 ESP32-S3-BOX-3 硬件笔记，
其中的目录结构、构建命令和硬件结论不适用于当前 P4 工作。
