# ESP32-P4 开发计划

本文档只记录当前 ESP32-P4 Function EV Board 最小 bring-up 的工作顺序。状态以
实际构建输出和实板串口日志为准，不以计划项的存在视为完成。

显示与触摸扩展的独立计划见
[ESP32-P4X-LVGL显示与触摸适配计划](ESP32-P4X-LVGL显示与触摸适配计划.md)。该计划
以 P4X revision v3.x、EK79007 MIPI-DSI 面板和 GT911 触摸屏为前提，必须在本
文档的最小启动验收完成后实施。

其中 ESP32-P4 MIPI-DSI Host 的芯片层边界、接口、DMA/video 分期与验收门见
[ESP32-P4 MIPI-DSI Host 设计与实施方案](ESP32-P4-MIPI-DSI-Host设计与实施方案.md)。
其中 M1 命令 Host、P4X command-mode 板级装配、EK79007 DPI Panel 与 `dsi_probe`
源码已完成。严格对齐 ESP-IDF DBI 命令传输配置后，`dsi_probe pattern` 与
`dsi_probe video` 均已通过 P4X 实板视觉验收；根因和复测步骤见
[P4X DSI 黑屏 DBI 配置排障闭环](../开发日志/编译/2026-08-24-ESP32-P4X-DSI黑屏DBI配置排障闭环.md)。
当前 `dsi_probe video` 从 NSH 入口到 EK79007、DPI Panel、DW-GDMA、
DSI Bridge 和 D-PHY 的逐函数路径见
[ESP32-P4X LCD 测试调用全链路](ESP32-P4X-LCD测试调用全链路.md)。

基于已验证的 P4X DSI 显示基线测试 Smart Home Demo 的分阶段方案、代码边界、
defconfig 方向和验收矩阵见
[ESP32-P4X SmartHome Demo 测试开发计划](ESP32-P4X-SmartHome-Demo测试开发计划.md)。

P4X MIPI-CSI 摄像头的 `/dev/video0` 实施采用“板级私有 SC2336 驱动、
ESP32-P4 ISP 输出 RGB565、V4L2 多缓冲队列”的路径；改动文件、资源生命周期和
验收条件见[ESP32-P4X MIPI-CSI 摄像头适配方案](ESP32-P4X-MIPI-CSI摄像头适配方案.md)。

上游 Apache NuttX / OpenVela 的 ESP32-P4 成熟基础适配采用“固定来源、逐能力
单元吸收、本地 DSI overlay 保留”的策略，详见
[ESP32-P4 上游成熟适配吸收计划](ESP32-P4上游成熟适配吸收计划.md)。

## 当前验收目标

最小 `nsh` 固件完成构建，并在 ESP32-P4 Function EV Board 上获得可交互的
`nsh>` 提示符。

## 阶段与完成条件

| 阶段 | 工作内容 | 完成条件 |
| --- | --- | --- |
| P0：工作树 | P4 manifest 链接、custom chip / board 源码和 RV32 配置 | 配置阶段能生成正确的 `.config` 与 chip/board 链接 |
| P1：构建 | 运行 P4 `nsh` 构建，逐个处理首个真实错误 | 生成完整 bootloader 与 NuttX 固件产物 |
| P2：启动 | 确认烧录命令、分段和 flash offset | 实板串口输出启动日志并进入 `nsh>` |
| P3：基础外设 | UART、GPIO、I2C、SPI Flash、PSRAM、以太网 | 每项有独立的实板验证记录 |
| P4：扩展外设 | 板载音频、显示、触摸、C6 网络协处理器 | 在 P3 完成后单独制定配置和验收标准 |

## 当前工作原则

1. 只处理当前构建或实板验证中的第一个真实错误。
2. 不把应用集成作为 P0–P2 的前置条件。
3. HAL 的本地工作副本不直接提交；修改应落实为
   `chips/esp32p4/common/espressif/patches/` 中可复现的补丁。
4. 每个阶段完成后，在 `开发日志/` 记录命令、首个错误或串口证据。

## 构建入口

在 openvela 工作区根目录执行：

```bash
./build.sh contest2026_031_niudanxianqianchong/board/esp32p4/esp32p4-function-ev-board/configs/nsh -j2
```
