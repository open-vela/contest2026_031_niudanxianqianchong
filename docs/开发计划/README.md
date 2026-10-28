# ESP32-P4 开发计划

本文档目录记录 ESP32-P4 Function EV Board 的开发计划、设计方案与验收条件。
状态以实际构建输出和实板串口日志为准，不以计划项的存在视为完成。

## 文档分类

本目录按主题分为六类。分类只表示文档的组织方式，不代表完成状态；
每篇文档自身的状态行与验收条件才是判断依据。

### 基础与规划

| 文档 | 内容 |
| --- | --- |
| [ESP32-P4 上游成熟适配吸收计划](基础与规划/ESP32-P4上游成熟适配吸收计划.md) | 上游 Apache NuttX / OpenVela 的 ESP32-P4 成熟基础适配吸收策略：固定来源、逐能力单元吸收、本地 DSI overlay 保留 |
| [ESP32-P4 竞赛参考差异对照](基础与规划/ESP32-P4竞赛参考差异对照.md) | 竞赛参考实现与本项目现网实现的差异对照 |
| [ESP32-P4X 双核 SMP 适配方案](基础与规划/ESP32-P4X-双核SMP适配方案.md) | 次核启动、跨核中断与调度、Flash/缓存保护及分阶段验收 |

### 显示与触摸

| 文档 | 内容 |
| --- | --- |
| [ESP32-P4X LVGL 显示与触摸适配计划](显示与触摸/ESP32-P4X-LVGL显示与触摸适配计划.md) | 以 P4X revision v3.x、EK79007 MIPI-DSI 面板和 GT911 触摸屏为前提的显示与触摸阶段门 |
| [ESP32-P4 MIPI-DSI Host 设计与实施方案](显示与触摸/ESP32-P4-MIPI-DSI-Host设计与实施方案.md) | DSI Host 的芯片层边界、接口、DMA/video 分期与验收门 |
| [ESP32-P4X ESP-IDF LCD 参考实现对比与收敛计划](显示与触摸/ESP32-P4X-ESP-IDF-LCD参考实现对比与收敛计划.md) | 与同硬件可显示的 ESP-IDF 工程逐项对照 DPI、GDMA、Bridge 与视觉验收 |
| [ESP32-P4X LCD 测试调用全链路](显示与触摸/ESP32-P4X-LCD测试调用全链路.md) | dsi_probe video 从 NSH 入口到面板、DPI Panel、DW-GDMA、DSI Bridge 与 D-PHY 的逐函数路径 |

### 摄像头与视觉

| 文档 | 内容 |
| --- | --- |
| [ESP32-P4X MIPI-CSI 摄像头适配方案](摄像头与视觉/ESP32-P4X-MIPI-CSI摄像头适配方案.md) | 板级私有 SC2336 驱动、ESP32-P4 ISP 输出 RGB565、V4L2 多缓冲队列的路径、改动文件与验收条件 |

### 网络与连接

| 文档 | 内容 |
| --- | --- |
| [ESP32-P4X SmartHome 板载 C6 Wi-Fi 接入方案](网络与连接/ESP32-P4X-SmartHome板载C6-WiFi接入方案.md) | 板载 ESP32-C6 经 SDIO 的 ESP-Hosted 接入：分层设计、W0~W4 阶段与验收 |
| [ESP32-P4X SmartHome 板载以太网接入方案](网络与连接/ESP32-P4X-SmartHome板载以太网接入方案.md) | 板载 EMAC 以太网接入方案与验收 |
| [ESP32-P4X 板载 C6 蓝牙/BLE 适配方案](网络与连接/ESP32-P4X-板载C6蓝牙BLE适配方案.md) | Hosted-HCI、NimBLE、C6 固件匹配及 Wi-Fi/BLE 共存验收 |

### 存储与文件系统

| 文档 | 内容 |
| --- | --- |
| [ESP32-P4X microSD 卡存储适配方案](存储与文件系统/ESP32-P4X-microSD卡存储适配方案.md) | SDMMC 双槽共享、标准 MMCSD/FAT32 接入及 C6 并发验收 |

### 应用与 AI

| 文档 | 内容 |
| --- | --- |
| [SmartHome cAGENT V2 移植方案](应用与AI/ESP32-P4X-SmartHome-cAGENT-V2移植方案.md) | V2 接入顺序、验收范围与 Git 提交安排 |
| [ESP32-P4X SmartHome Demo 测试开发计划](应用与AI/ESP32-P4X-SmartHome-Demo测试开发计划.md) | 基于已验证 DSI 显示基线的 demo 分阶段方案、代码边界、defconfig 方向与验收矩阵 |
| [ESP32-P4X SmartHome 摄像头实时预览接入方案](应用与AI/ESP32-P4X-SmartHome摄像头实时预览接入方案.md) | 基于 `/dev/video0` V4L2 视频通路的本地预览架构、线程/缓冲边界、分期实施与真机验收 |
| [ESP32-P4X 智能家居中控面板 UI 设计方案](应用与AI/ESP32-P4X-智能家居中控面板UI设计方案.md) | 1024x600 横屏深色中控面板：信息架构、视觉规范、组件规划、业务解耦，附录含 LVGL 与快应用路线取舍 |
| [ESP32-P4X 端侧 KWS 方案](应用与AI/ESP32-P4X-端侧KWS方案.md) | 端侧唤醒词与命令词：可复用资产、数据流、与 cAGENT 边界、K0~K4 阶段 |
| [ESP32-P4X SmartHome `wake_large` 唤醒词模型接入方案](应用与AI/ESP32-P4X-SmartHome-wake_large唤醒词模型接入方案.md) | `wake_large` int8 模型的特征契约、TFLM 运行时、K0~K5 实施步骤与实机验收指标 |
| [ESP32-P4X 端侧目标检测方案](应用与AI/ESP32-P4X-端侧目标检测方案.md) | 端侧检测：工具与事件两种范式、数据流、run_service 改造项、D0~D4 阶段 |

## UI 与 QuickApp 设计

| 文档 | 内容 |
| --- | --- |
| [智能家居中控面板 UI 设计方案](应用与AI/ESP32-P4X-智能家居中控面板UI设计方案.md) | 1024×600 中控面板的信息架构、交互、性能目标和 UI 模式取舍 |
| [QuickApp UI 与原生服务分层方案](应用与AI/ESP32-P4X-SmartHome快应用UI与原生服务分层方案.md) | QuickApp、原生服务、cAGENT、设备数据与摄像头预览的边界 |
| [QuickApp 模拟器最小闭环验证方案](应用与AI/ESP32-P4X-SmartHome-QuickApp模拟器最小闭环验证方案.md) | Goldfish 验证和迁移至 P4X 的阶段门 |
| [UI 视觉资源设计与导入方案](应用与AI/ESP32-P4X-SmartHome-UI视觉资源设计与导入方案.md) | 图标、插画、字体资源及 LVGL/QuickApp 导入规范 |
| [UI 资源清单](应用与AI/ESP32-P4X-SmartHome-UI资源清单.md) | 当前原型所需和已准备的视觉资源 |

## 当前验收目标

最小 nsh 固件完成构建，并在 ESP32-P4 Function EV Board 上获得可交互的
nsh 提示符。

## 阶段与完成条件

| 阶段 | 工作内容 | 完成条件 |
| --- | --- | --- |
| P0：工作树 | P4 manifest 链接、custom chip / board 源码和 RV32 配置 | 配置阶段能生成正确的 .config 与 chip/board 链接 |
| P1：构建 | 运行 P4 nsh 构建，逐个处理首个真实错误 | 生成完整 bootloader 与 NuttX 固件产物 |
| P2：启动 | 确认烧录命令、分段和 flash offset | 实板串口输出启动日志并进入 nsh |
| P3：基础外设 | UART、GPIO、I2C、SPI Flash、PSRAM、以太网 | 每项有独立的实板验证记录 |
| P4：扩展外设 | 板载音频、显示、触摸、C6 网络协处理器 | 在 P3 完成后单独制定配置和验收标准 |

## 当前工作原则

1. 只处理当前构建或实板验证中的第一个真实错误。
2. 不把应用集成作为 P0–P2 的前置条件。
3. HAL 的本地工作副本不直接提交；修改应落实为
   chips/esp32p4/common/espressif/patches/ 中可复现的补丁。
4. 每个阶段完成后，在 开发日志/ 记录命令、首个错误或串口证据。

## 构建入口

在 openvela 工作区根目录执行：

    ./build.sh contest2026_031_niudanxianqianchong/board/esp32p4/esp32p4-function-ev-board/configs/nsh -j2
