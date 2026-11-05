# ESP32-P4 开发计划

本目录记录 ESP32-P4 Function EV Board 的开发计划、设计方案与验收条件。
每篇文档文首有状态行；判断某项是否完成，以该状态行和对应的板端记录为准。

## 基础与规划

| 文档 | 内容 |
| --- | --- |
| [ESP32-P4 上游成熟适配吸收计划](基础与规划/ESP32-P4上游成熟适配吸收计划.md) | 上游 NuttX / OpenVela 的 P4 成熟适配吸收策略：固定来源、逐能力单元吸收、本地 DSI overlay 保留 |
| [ESP32-P4 竞赛参考差异对照](基础与规划/ESP32-P4竞赛参考差异对照.md) | 竞赛参考实现与本项目现网实现的差异 |
| [ESP32-P4X 双核 SMP 适配方案](基础与规划/ESP32-P4X-双核SMP适配方案.md) | 次核启动、跨核中断与调度、Flash/缓存保护；已实施实验性双核启动 |
| [ESP32-P4X SMP 双核适配方案](基础与规划/ESP32-P4X-SMP双核适配方案.md) | 2026-10-25 的 v1.0 版本，上文的前身；保留四阶段取证思路 |
| [ESP32-P4X SMP 启动复位修复方案](基础与规划/ESP32-P4X-SMP启动复位修复方案.md) | 启动复位故障基线与修复 |
| [ESP32-P4X SMP 启动修复日志](基础与规划/ESP32-P4X-SMP启动修复日志_20261028.md) | 2026-10-28 的启动修复过程记录 |
| [SMP 音频链路故障分析](ESP32-P4X-SMP音频链路故障分析.md) | 音频全链路、SMP 下的 6 个失效点与候选根因 |
| [SMP 音频链路排查与修复方案](ESP32-P4X-SMP音频链路排查与修复方案_20261101.md) | 落到代码行的违规点清单与修复补丁；已由 `d3374a3` 落地 |
| [驱动缺口与 BUG 清单](驱动缺口与BUG清单.md) | 按"已验收 / 代码就绪"口径统计的驱动缺口 |

## 显示与触摸

| 文档 | 内容 |
| --- | --- |
| [MIPI-DSI Host 设计与实施方案](显示与触摸/ESP32-P4-MIPI-DSI-Host设计与实施方案.md) | DSI Host 的芯片层边界、接口、DMA/video 分期 |
| [LVGL 显示与触摸适配计划](显示与触摸/ESP32-P4X-LVGL显示与触摸适配计划.md) | LVGL 阶段门与验收条件 |
| [ESP-IDF LCD 参考实现对比与收敛计划](显示与触摸/ESP32-P4X-ESP-IDF-LCD参考实现对比与收敛计划.md) | 与同硬件可显示的 ESP-IDF 工程逐项对照 DPI、GDMA、Bridge |
| [LCD 测试调用全链路](显示与触摸/ESP32-P4X-LCD测试调用全链路.md) | `dsi_probe video` 从 NSH 到面板的逐函数路径 |
| [GT911 中断模式改造实施方案](显示与触摸/ESP32-P4X-GT911中断模式改造实施方案.md) | 中断模式设计；`gt911_irq` 配置已存在 |

## 摄像头与视觉

| 文档 | 内容 |
| --- | --- |
| [MIPI-CSI 摄像头适配方案](摄像头与视觉/ESP32-P4X-MIPI-CSI摄像头适配方案.md) | 板级 SC2336 驱动、ISP RGB565、V4L2 多缓冲；`/dev/video0` 已可用 |
| [端侧目标检测方案](应用与AI/ESP32-P4X-端侧目标检测方案.md) | 检测的两种范式、run_service 改造项、D0~D4 阶段；未实施 |

## 网络与连接

| 文档 | 内容 |
| --- | --- |
| [板载 C6 Wi-Fi 接入方案](网络与连接/ESP32-P4X-SmartHome板载C6-WiFi接入方案.md) | C6 经 SDIO 的 ESP-Hosted 接入；控制面与 WLAN 数据面已通 |
| [板载以太网接入方案](网络与连接/ESP32-P4X-SmartHome板载以太网接入方案.md) | EMAC 接入方案；方案阶段 |
| [板载 C6 蓝牙/BLE 适配方案](网络与连接/ESP32-P4X-板载C6蓝牙BLE适配方案.md) | Hosted-HCI、NimBLE、Wi-Fi/BLE 共存；未实施 |

## 存储与文件系统

| 文档 | 内容 |
| --- | --- |
| [microSD 卡存储适配方案](存储与文件系统/ESP32-P4X-microSD卡存储适配方案.md) | SDMMC 双槽共享、FAT32 接入；方案阶段 |

## 应用与 AI

| 文档 | 内容 |
| --- | --- |
| [SmartHome 语音与能力合入实施计划](应用与AI/ESP32-P4X-SmartHome语音与能力合入实施计划.md) | **总进度文档**：P0 音频 → P6 SMP 七阶段的状态、证据与验收条件 |
| [cAGENT V2 移植方案](应用与AI/ESP32-P4X-SmartHome-cAGENT-V2移植方案.md) | V2 接入顺序与验收范围；已通过 Host/ASan/UBSan 与固件构建 |
| [SmartHome Demo 测试开发计划](应用与AI/ESP32-P4X-SmartHome-Demo测试开发计划.md) | 分阶段方案、代码边界、defconfig 方向与验收矩阵 |
| [端侧 KWS 方案](应用与AI/ESP32-P4X-端侧KWS方案.md) | K0~K4 分层判据与 cAGENT 边界约定；实现见 `voice/kws/` |
| [wake_large 唤醒词模型接入方案](应用与AI/ESP32-P4X-SmartHome-wake_large唤醒词模型接入方案.md) | 模型特征契约、TFLM 运行时、K0~K5 步骤；已实施，`kws_smoke` 板端可跑 |
| [KWS 推理优化与 P4 后端接入方案](应用与AI/ESP32-P4X-SmartHome-KWS推理优化与P4后端接入方案.md) | ESP-NN P4 后端接入与 mask 白名单；已生效，2.54× 加速 |
| [语音播报 TTS 接入方案](应用与AI/ESP32-P4X-SmartHome-语音播报TTS接入方案.md) | 四层架构、各后端 API 差异、24→16 kHz 重采样；短句通过，长句超时 |
| [语音链路 TTS-ASR-KWS 集成方案](应用与AI/ESP32-P4X-SmartHome-语音链路TTS-ASR-KWS集成方案.md) | 三段串联的时序与互斥约定；完整链路未一次跑通 |
| [摄像头实时预览接入方案](应用与AI/ESP32-P4X-SmartHome摄像头实时预览接入方案.md) | V4L2 视频通路的本地预览、线程/缓冲边界、PPA 缩放 |
| [聊天记录持久化方案](应用与AI/ESP32-P4X-SmartHome聊天记录持久化方案.md) | session JSONL 落盘与重启恢复 |
| [智能家居中控面板 UI 设计方案](应用与AI/ESP32-P4X-智能家居中控面板UI设计方案.md) | 1024×600 横屏深色面板：信息架构、视觉规范、组件规划 |
| [LVGL V9 实施方案](应用与AI/ESP32-P4X-SmartHome-LVGL-V9实施方案.md) | V9 界面落地；主线默认界面 |
| [LVGL 横屏 UI 重构方案](应用与AI/ESP32-P4X-SmartHome-LVGL横屏UI重构方案.md) | 从快应用回归原生 LVGL 的重构边界与顺序 |
| [LVGL 性能优化方案](应用与AI/ESP32-P4X-SmartHome-LVGL性能优化方案.md) | 帧时间与渲染开销分析；优化项按需实施 |
| [快应用 UI 与原生服务分层方案](应用与AI/ESP32-P4X-SmartHome快应用UI与原生服务分层方案.md) | QuickApp、原生服务、cAGENT、设备数据与摄像头预览的边界 |
| [QuickApp 模拟器最小闭环验证方案](应用与AI/ESP32-P4X-SmartHome-QuickApp模拟器最小闭环验证方案.md) | Goldfish 验证与迁移至 P4X 的阶段门 |
| [QuickApp 完整 UI 迁移设计](应用与AI/ESP32-P4X-SmartHome-QuickApp完整UI迁移设计.md) | UI 骨架、RPK 打包与 Goldfish 验证 |
| [UI 视觉资源设计与导入方案](应用与AI/ESP32-P4X-SmartHome-UI视觉资源设计与导入方案.md) | 图标、插画、字体资源及导入规范；MiSans 已在 P4X 真机验证 |
| [UI 资源清单](应用与AI/ESP32-P4X-SmartHome-UI资源清单.md) | 原型所需与已准备的视觉资源 |
| [面试高频考点](应用与AI/智能家居与本地音频项目面试高频考点.md) | 项目涉及的嵌入式与音频知识点梳理 |
| [问题复盘与 AI 辅助开发资产](应用与AI/智能家居与本地音频项目问题复盘与AI辅助开发资产.md) | 开发过程中的问题复盘与可复用资产索引 |

## 构建入口

在 openvela 工作区根目录执行，配置名取自
`board/esp32p4/esp32p4-function-ev-board/configs/<name>`：

```bash
contest2026_031_niudanxianqianchong/scripts/build_p4x.sh <config-name> -j8
```

脚本按 `CONFIG_ESP32P4_ESPRESSIF_TOOLCHAIN` 选择 `riscv32-esp-elf` 或
`riscv-none-elf`；直接用 `./build.sh` 时 `envsetup.sh` 只加 `riscv-none-elf`
到 PATH，启用乐鑫工具链的配置会找不到编译器。

当前配置（共 57 个，`smart_home` 系列 7 个）：

| 配置 | 差异 |
| --- | --- |
| `smart_home` | SMP 双核，camera 工具开启，V9 界面 |
| `smart_home_voice` | 单核语音基线，ASR + TTS + VAD + camera 工具 |
| `smart_home_kws` | KWS smoke（`CONFIG_SMART_HOME_KWS_SMOKE`），ESP-NN + mask |
| `smart_home_local` | 继承 `smart_home`，关闭 `SMART_HOME_DEMO_OFFLINE_UI`，覆盖现场 Wi-Fi 凭据 |
| `smart_home_service_lab` | 服务侧实验配置 |
| `smart_home_quickapp` | 快应用 UI 路径 |
| `smp_smart_home` | SMP + smart_home 组合 |
