# 文档索引

本目录的当前主题是 **ESP32-P4 Function EV Board 的 openvela Route A 移植**。
最小 `usbconsole` 配置已在实板进入 `nsh>`；MIPI-DSI Host Probe 已完成
M1 命令写、Host 内建 pattern 与 RGB565 DMA 色条的实板视觉验收。NuttX 标准
`/dev/fb0` 及 `fb` 示例也已完成真机验收；静态 LVGL Smart Home 首页已在该
framebuffer 上完成首屏验收。GT911 P3.1 已完成 P4X I2C 轮询装配，
`gt911_probe` 已真机验证单指 `DOWN/MOVE/UP`、坐标和触摸面积；GT911
现以 100 kHz 自动探测 `0x5d/0x14`，并已验证 `/dev/input0` 注册后可继续
挂载 0x800000 LittleFS 到 `/data`；
本轮尚未将 `/dev/input0` 交给 LVGL 页面，多点触摸也待继续验收。

## 目录骨架

```text
docs/
├── 硬件适配/    # P4 架构、板级适配和构建说明
├── 开发日志/    # 已发生问题、命令和结论的历史记录
│   └── 编译/    # Kconfig 与构建链路排障记录
├── 开发计划/    # 当前阶段、验收条件和下一步
└── archive/     # 已退出当前范围的历史材料
```

## 当前文档

| 文档 | 用途 | 阅读时机 |
| --- | --- | --- |
| [ESP32-P4 Function EV Board 适配文档](硬件适配/esp32p4-ev-board-adaptation.md) | 适配范围、目录映射、构建与排障信息 | 开始移植或定位构建错误时 |
| [Route A 移植方案](硬件适配/esp32p4-function-ev-board-route-a-porting.md) | custom chip / custom board 架构、阶段目标和风险 | 评审架构或新增 P4 外设前 |
| [P4 最小 NSH 操作与测试](硬件适配/esp32p4-nsh-operation-and-test.md) | P4 构建、烧录及最小 NSH 上板验收 | 上板测试时 |
| [P4X GT911 触摸适配](硬件适配/gt911适配.md) | I2C、双地址探测、触摸事件链和真机排障结论 | 验证 `/dev/input0` 或 LVGL 输入时 |
| [P4 构建与 Kconfig 排障](开发日志/编译/README.md) | 本次构建链路、Kconfig 阻塞与复测顺序 | 配置生成或编译失败时 |
| [P4X DSI Host Probe 排障](开发日志/编译/2026-08-21-DSI-Host-Probe排障记录.md) | DSI Host、Probe 注册与 USB Console 专项排障 | 验证 DSI 命令链路时 |
| [P4X DSI 黑屏 DBI 配置排障闭环](开发日志/编译/2026-08-24-ESP32-P4X-DSI黑屏DBI配置排障闭环.md) | DBI 命令 LP 传输配置导致黑屏的根因、修复和实板双路径验收 | 排查或复测 P4X 显示时 |
| [P4X LittleFS 挂载失败与修复](开发日志/ESP32-P4X-LittleFS挂载失败与修复.md) | ESP HAL 默认 Flash 芯片未初始化导致数据资源无法挂载的根因、修复和复测步骤 | 数据资源或完整 Smart Home 启动异常时 |
| [P4X framebuffer 真机验收](开发日志/编译/2026-08-24-ESP32-P4X-framebuffer真机验收.md) | `/dev/fb0` 注册、标准 `fb` 示例和 `FBIO_UPDATE` 的真机结果 | 接入 LVGL 前确认显示设备时 |
| [P4X LVGL 静态首页真机验收](开发日志/编译/2026-08-24-ESP32-P4X-LVGL静态首页真机验收.md) | P2 静态 Smart Home 首页绑定 `/dev/fb0`、首帧显示与定时刷新循环的真机结果 | 进入触摸或完整 Smart Home 前确认 UI 基线时 |
| [P4 移植开发记录](开发日志/dev.md) | 已发生问题的历史记录 | 复现相同错误时；不代表当前构建结论 |
| [当前开发计划](开发计划/README.md) | P4 最小 bring-up 的阶段与验收条件 | 安排或切换工作项时 |
| [上游成熟适配吸收计划](开发计划/ESP32-P4上游成熟适配吸收计划.md) | 上游 P4 基线的选择性同步边界、步骤和回归矩阵 | 计划同步 Apache NuttX / OpenVela P4 改动时 |
| [ESP-IDF LCD 参考实现对比与收敛计划](开发计划/ESP32-P4X-ESP-IDF-LCD参考实现对比与收敛计划.md) | 与同硬件可显示的 ESP-IDF 工程逐项对照 DPI、GDMA、Bridge 与视觉验收 | P4X 背光亮但 DSI 色条黑屏时 |
| [第三方依赖与许可证声明](../THIRD_PARTY_NOTICES.md) | P4 HAL 与工作区依赖的许可证信息 | 发布或交付前 |

## 文档边界

- 当前源码和配置以 `board/esp32p4/`、`chips/esp32p4/` 与根目录 README 为准。
- 不再维护 ESP32-S3 板级配置或上层应用集成说明；它们不属于 P4 最小 bring-up。
- 文中若出现尚未存在的配置、烧录命令或应用方案，应视为待验证建议，不能作为
  已完成能力的证明。

## 历史材料

[archive/](archive/) 保存旧 SmartHome 交付稿和 ESP32-S3-BOX-3 硬件笔记，仅供
追溯；其中的目录、构建命令、硬件结论和依赖关系均不适用于当前 P4 工作。
