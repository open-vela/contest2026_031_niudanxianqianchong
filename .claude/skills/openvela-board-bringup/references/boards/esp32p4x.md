# ESP32-P4X Function EV Board 板级 Quirks

只放本板特有知识。通用流程见 SKILL.md 与 ../phase-checklist.md。文档路径相对本仓根,以仓内文档为唯一事实源。

## 目录

- [结构与软链接机制](#结构与软链接机制)
- [已验证能力基线](#已验证能力基线)
- [外设坑索引](#外设坑索引)
- [构建与链接玄学](#构建与链接玄学)
- [probe 应用清单](#probe-应用清单)
- [文档落位惯例](#文档落位惯例)

## 结构与软链接机制

- 本仓维护 EK79007/GT911/SC2336 等源码,**构建前按 README"快速开始"把驱动源码软链接映射到本地 NuttX 工作树**——构建异常先查软链接是否就位。
- `chips/esp32p4/esp-hal-3rdparty` 是 ESP-HAL 三方库副本:历史上出现过旧副本残留/双层源码路径干扰构建,清理记录见 `docs/开发日志/ESP32-P4X-ESP-HAL旧副本清理记录.md`。改动 HAL 相关代码时先确认生效的是哪一层副本。
- 仿真配置在 `board/goldfish-arm64/configs/`(smart_home、smart_home_quickapp),真机配置在 `board/esp32p4/esp32p4-function-ev-board/configs/`。

## 已验证能力基线

以仓根 `README.md` 的能力表为准(USB Serial/JTAG 与 NSH、Route A 启动、EK79007 DSI 色条、NuttX framebuffer、静态 LVGL 首页、GT911 单指等)。开始新外设前先读该表,确认依赖的前序能力已验证。

## 外设坑索引

| 外设 | 已知坑 | 详读 |
|---|---|---|
| SC2336 摄像头 | CSI RAW 接收、DMA 数据流调用链 | `docs/硬件适配/ESP32-P4X-SC2336摄像头适配.md`、`ESP32-P4X-SC2336-视频通路架构与调用链.md`、`docs/开发日志/SC2336-CSI-DMA数据流与调用链.md`、`SC2336-CSI-RAW接收故障修复.md` |
| SC2336 交接 | 多人接力时的上下文 | `docs/硬件适配/ESP32-P4X-SC2336-CSI-RAW接收交接.md`、`ESP32-P4X-SC2336适配交接记录.md` |
| ES8311 音频 | codec 适配与 GDMA 问题 | `docs/硬件适配/ESP32-P4X-Function-EV-Board-ES8311音频适配方案.md`、`docs/开发日志/ESP32-P4X-ES8311-Codec适配与GDMA问题复盘.md` |
| C6 WiFi | ESP-Hosted 控制面/数据面、SDIO 枚举 | `docs/开发日志/ESP32-P4X-C6-ESP-Hosted控制面与WLAN数据面开发记录.md`、`ESP32-P4X-C6-SDIO基础枚举阶段记录.md` |
| EK79007 DSI | 关闭 Bridge underrun 中断,消除 flash 擦写冲突 | git log `fix(dsi)`;README 显示链路章节 |
| GT911 触摸 | 单指已验证;INT/RST 不在 HMI 子板,经适配板 J6 引出;中断模式需自行接线 | `docs/硬件适配/gt911适配.md` |
| LittleFS | 挂载失败与修复 | `docs/开发日志/ESP32-P4X-LittleFS挂载失败与修复.md` |
| PSRAM/字体 | MiSans 完整字体 PSRAM 预加载 | `docs/开发日志/ESP32-P4X-SmartHome-完整MiSans-PSRAM预加载修复.md` |
| HTTP | 发送阻塞修复 | `docs/开发日志/ESP32-P4X-SmartHome-HTTP发送阻塞修复.md` |
| Route A 移植 | 整体移植路径 | `docs/硬件适配/esp32p4-function-ev-board-route-a-porting.md`、`esp32p4-ev-board-adaptation.md`、`esp32p4-nsh-operation-and-test.md` |

## 构建与链接玄学

来自 git 提交历史,多数尚未写进 docs(改动相关功能前先自查这几条):

- `CONFIG_TLS_NELEM` 未设满时 **LIBCXX 会被静默丢弃**,链接期无报错、运行期缺符号(修法 `CONFIG_TLS_NELEM=2`)。
- tflite-micro 存在**双层源码路径**,引用错层导致符号缺失(修法见 `fix(voice)` 提交)。
- C/C++ 混合边界需要 **extern "C" 守卫**,否则链接期找不到实现。
- 缓冲信息结构名要**对齐本树 NuttX**(`ap_buf*` 系列),不能按上游文档照抄。
- 真机"冻死"先查栈:TTS 播放栈 8K→32K 修复过 speak 冻死;KWS arena 需要 512K。
- **头文件或链接布局变更后必须全量 clean**,陈旧对象会引发蓝屏(归档:`docs/开发日志/dev.md` 相关排查)。
- `savedefconfig` 生成后保持字母序,显式禁用不用的模块(如 ai_agent 示例),避免配置漂移。

## probe 应用清单

`app/` 下:`csi_probe`(摄像头)、`dsi_probe`(显示 pattern/video)、`gt911_probe`(触摸事件)、`video_test`(视频)、`hello_app`(最小应用)。新外设照此模式加独立 probe。

## 文档落位惯例

`docs/硬件适配/` = 适配方案与交接记录;`docs/开发日志/` = 故障复盘;`docs/开发指南/` = 编译烧录与资源部署指南(如 `ESP32-P4X-SmartHome编译烧录与资源部署指南.md`)。文件名带板名前缀 `ESP32-P4X-*`。
