# ESP32-P4X 新硬件适配证据文档（2026 OpenVela AI 硬件开发者大赛）

本目录是「新硬件适配」赛道的评审证据文档包。项目以 **Route A（custom chip +
custom board）**方式将 openvela（NuttX 内核）适配到 **ESP32-P4X-Function-EV-Board**
开发板：`chips/esp32p4/`（芯片层 + Espressif HAL + MIPI DSI/CSI Host）与
`board/esp32p4/`（板级装配 + 57 个独立 defconfig），EK79007/GT911 驱动源码
维护在 `drivers/nuttx/`，独立硬件验证程序在 `app/`（dsi_probe / gt911_probe /
csi_probe / video_test / graphics_probe / smp_flash_test / smp_psram_probe），
音频与语音 smoke 命令（audio_smoke / kws_smoke / asr_smoke / tts_smoke）随
`demos/smart_home` 构建、由配置开关控制。

每个适配域一份文档，统一结构：**适配背景与目标 → 适配流程 → 关键代码与配置 →
适配证据（真机验收）→ 遇到的问题与解决 → 当前验收状态与边界 → 原始文档索引**。

## 文档索引

| # | 文档 | 适配域 | 真机验收状态 |
| --- | --- | --- | --- |
| 01 | [系统启动与芯片移植](开发日志/01-系统启动与芯片移植.md) | Route A 芯片层、板级 bring-up、最小系统（USB Serial/JTAG + NSH）、ESP-HAL 治理 | ✅ NSH 启动与内建命令通过 |
| 02 | [MIPI-DSI 屏幕显示适配](开发日志/02-MIPI-DSI屏幕显示适配.md) | MIPI-DSI Host、EK79007 面板（1024×600）、NuttX framebuffer、LVGL | ✅ 色条 / `/dev/fb0` + `fb` 示例 / LVGL 静态首页三项通过 |
| 03 | [触摸 GT911 适配](开发日志/03-触摸GT911适配.md) | I2C 触摸、`/dev/input0`、gt911_probe | ✅ 单指 + LVGL 触摸交互通过；多点待验证 |
| 04 | [MIPI-CSI 摄像头 SC2336 适配](开发日志/04-MIPI-CSI摄像头SC2336适配.md) | MIPI-CSI、CSI Host/Bridge、GDMA、V4L2（RGB565） | ✅ 300 帧 `app_fps=30.02`、`sequence_gaps=0`；UI 实时预览已验收 |
| 05 | [WiFi 适配（板载 C6 · ESP-Hosted）](开发日志/05-WiFi-C6托管适配.md) | SDIO 总线、ESP-Hosted 控制面、WLAN 数据面 | ✅ 枚举→关联→DHCP→DNS→TCP 443→TLS/模型对话（2026-09-20 真机闭环） |
| 06 | [以太网适配](开发日志/06-以太网适配.md) | P4 内置 EMAC + 外置 PHY、`eth0` | 方案 + 代码就绪，待真机验证 |
| 07 | [音频 ES8311 适配](开发日志/07-音频ES8311适配.md) | ES8311 codec、I2S/GDMA、语音链路（TTS/ASR/KWS） | ✅ 10 秒录音 320000 B + 回放，用户听感确认；语音链路分段验收 |
| 08 | [存储 LittleFS 与系统集成](开发日志/08-存储LittleFS与系统集成.md) | LittleFS 数据分区（`0x600000`，10 MiB）、PSRAM 资源预加载、部署脚本 | ✅ `/data` 挂载 + 完整 MiSans（7,943,504 B）预加载通过 |
| 09 | [智能家居应用](开发日志/09-智能家居应用.md) | cAGENT 云端对话（DeepSeek）、米家设备控制、LVGL 中控 UI、摄像头实时预览 | ✅ 米家控制 + Agent 工具链真机闭环；摄像头 UI 预览已验收 |
| 10 | [PPA 与 2D-DMA 适配](开发日志/10-PPA与2D-DMA图形加速适配.md) | 片内 DMA2D、PPA（填充/缩放旋转镜像/混合/格式转换）、SMP CPU0 共存 | ✅ 五项全 PASS；100+50 轮压测 0 失败，堆用量零变化（2026-11-04） |
| 11 | [音频 ES8311 录放闭环验收](开发日志/11-音频ES8311录放闭环验收.md) | 16 kHz 单声道播放、录音、录→存→放闭环、连续录音稳定性 | ✅ 字节数全部精确；录音内容经 hexdump 确认为真实信号（2026-11-04） |
| 12 | [SMP 双核适配验收](开发日志/12-SMP双核适配验收.md) | HP CPU0/CPU1 跨核调度、PSRAM/Flash 双核安全、LittleFS 并发 | ✅ 真跨核并发 + 跨核调用 + 50 轮 Flash 并发 + ostest 全量（2026-11-04） |
| 13 | [ESP-NN P4 后端接入与验收](开发日志/13-ESP-NN-P4后端接入与验收.md) | xespv PIE 指令、esp-nn INT8 Conv2D/DWConv2D、TFLM 后端接线 | ⚠️ 接入生效（58 条 PIE 指令、p50=2.38 s 复现基线）；逐算子数值比对未执行 |

## 与大赛评分维度的对应

| 评分维度（分值） | 对应证据 |
| --- | --- |
| 技术难度（30） | 新芯片从 0 到 1 的 Route A 移植（01）；MIPI-DSI/CSI 双高速接口、SDIO 托管 WiFi、GDMA 音频等底层驱动扩展（02/04/05/07/11）；HP 双核 SMP 跨核调度与并发安全（12）；PPA/2D-DMA 图像加速（10）；xespv 私有指令集上的 INT8 算子后端（13）；携带 2 个 nuttx V4L2 修复 patch（04） |
| 项目完整度（20） | 57 个独立 defconfig 配置矩阵、probe-first 独立验证程序（`app/`）、SmartHome 可运行 Demo；本目录 13 份文档含完整适配流程与复现命令 |
| AI 开发（10） | AI Coding 日志已按大赛要求归集至仓内 `logs/`（见《AI Coding 日志归集与提交手册》） |
| 展示效果（10） | 各文档"适配证据"章节的真机日志与视觉验收记录可直接用于路演材料 |

## 证据规范

- **逐字摘录、注明出处**：所有真机日志、命令输出均逐字取自仓内源文档（各文档
  附录列明原始路径），不做改写或外推。
- **证据分级**：遵循"枚举成功 ≠ 数据有效 ≠ 时序/业务达标"，各文档按级给出结论。
- **如实标注边界**：源文档中"待真机验证""证据不足"等限定原样保留，方案类内容
  不写成已完成能力。
- **溯源**：原始文档唯一事实源在 `docs/硬件适配/`、`docs/开发日志/`、
  `docs/开发计划/`、`docs/开发指南/`；本目录为面向评审的整合视图。

## 提交材料清单（对照《参赛代码提交指南》）

1. 专属仓代码（本仓 `chips/` + `board/` + `drivers/` + `patches/` + `third_party/` + `app/` + `demos/` + `packages/` + `scripts/`）；
2. AI Coding 日志：仓内 `logs/`；
3. 作品介绍文档（.docx / .pdf / .pptx）与演示视频（≤ 5 分钟）另行准备，
   本目录 13 份证据文档（01–08 硬件适配 + 09 智能家居应用 + 10–13 图形加速/SMP/ESP-NN/音频闭环）可作为介绍文档的技术附录；
4. 仓库 README 为作品入口（名称、赛道、运行方式、简介）。

## 其他材料

| 文件 | 说明 |
| --- | --- |
| [作品技术报告.md](作品技术报告.md) / [.docx](作品技术报告.docx) | 按组委会《作品提交模板》撰写的作品技术报告（定稿，含系统架构图与实物/UI 实拍附录） |
| [演示/test.log](演示/test.log) | 真机串口演示日志样例（852 行，含米家控制与云端对话过程；凭据仅出现长度无实际值） |
| `2026 首届 openvela AI 硬件开发者大赛 - 作品提交模板.docx` | 组委会原始模板（提交打包时移出压缩包，不入仓评审视图） |
