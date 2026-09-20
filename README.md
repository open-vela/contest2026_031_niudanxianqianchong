# ESP32-P4X 智能家居中控面板 —— openvela 新硬件全栈适配

## 一、作品简介

本作品以 **Route A（custom chip + custom board）** 方式，将 openvela（NuttX 内核）
从零适配到 **ESP32-P4X-Function-EV-Board**，并在这块新硬件上落地了一个
**智能家居中控面板**应用：1024×600 MIPI-DSI 横屏深色 UI（完整 MiSans 字体）、
米家设备卡片与控制、摄像头实时预览通路、以及经板载 ESP32-C6 托管 WiFi 接入
云端大模型（cAGENT）的对话控制。

作品的两条主线：

1. **新硬件适配**：P4 芯片层（含 Espressif HAL 与 MIPI-DSI/CSI Host）+ 板级
   bring-up + 7 个外设域驱动逐项真机验收，携带 2 个 nuttx 通用 V4L2 修复
   patch，40 余个独立 defconfig 配置矩阵与 probe 独立验证程序。
2. **智能家居应用**：在适配完成的硬件上运行 SmartHome 中控 Demo，覆盖
   openvela 大赛判定标准的**图形**（LVGL/DSI）、**多媒体**（摄像头/音频）、
   **AI**（cAGENT 云端模型对话）三项核心能力。

### 硬件适配能力（真机验收，截至 2026-09-20）

| 能力域 | 状态 | 证据 |
| --- | --- | --- |
| USB Serial/JTAG 控制台 + NSH 最小系统 | ✅ 通过 | [competition/01](competition/01-系统启动与芯片移植.md) |
| MIPI-DSI 显示（EK79007，1024×600）：色条 → `/dev/fb0` → LVGL 首页 | ✅ 通过 | [competition/02](competition/02-MIPI-DSI屏幕显示适配.md) |
| GT911 触摸（I2C，`/dev/input0`）：单指 DOWN/MOVE/UP | ✅ 通过 | [competition/03](competition/03-触摸GT911适配.md) |
| SC2336 摄像头（MIPI-CSI，V4L2 RGB565）：300 帧 `app_fps=30.02` | ✅ 通过 | [competition/04](competition/04-MIPI-CSI摄像头SC2336适配.md) |
| WiFi（板载 C6，SDIO + ESP-Hosted）：枚举→关联→DHCP→DNS→TCP 443 | ✅ 通过（TLS 证据待补） | [competition/05](competition/05-WiFi-C6托管适配.md) |
| 以太网（P4 内置 EMAC，`eth0`） | 方案+代码就绪，待真机 | [competition/06](competition/06-以太网适配.md) |
| ES8311 音频（I2S/GDMA） | 构建与真机 codec 初始化通过 | [competition/07](competition/07-音频ES8311适配.md) |
| LittleFS 数据分区 + 完整 MiSans（7.9 MB）PSRAM 预加载 | ✅ 通过 | [competition/08](competition/08-存储LittleFS与系统集成.md) |

> 逐域证据文档（适配流程 / 真机日志 / 问题闭环）见 `competition/`，
> 全部日志逐字摘录并注明出处，"待验证"边界如实标注。

### 智能家居应用能力

| 能力 | 状态 |
| --- | --- |
| LVGL 中控面板 UI（1024×600 横屏深色、设备卡片、完整中文字体） | 静态首页真机验收通过；网络模式 UI 持续迭代中 |
| 米家设备接入（设备卡片、控制抽屉、摄像机 spec 拉取） | 已实现，完成多轮真机排障闭环（spec 重试、响应缓冲、IOB 池、控制抽屉刷新等） |
| 摄像头实时预览（`/dev/video0` V4L2 通路接入 UI） | 视频通路已真机验收（30 fps），UI 内预览接入推进中 |
| 云端模型对话（cAGENT，经 C6 WiFi → TLS → 模型 API） | 网络通路真机验收至 TCP 443；TLS/模型响应证据待补 |

## 二、选题方向

**新硬件适配（主）+ AI 硬件产品创新（智能家居应用）**。

选新硬件适配的理由：截至 2026 年 6 月，openvela 分支尚未合入 ESP32-P4；
相对已适配的 ESP32-S3，P4 的硬件跨度大——RISC-V HP 双核 400MHz、768 KB SRAM、
PSRAM、MIPI-DSI/CSI 高速接口、内置 EMAC、USB-OTG 2.0 HS——属于
"从 0 到 1 为全新芯片平台完成首次适配"。在此之上开发智能家居中控应用，
是为了验证适配的完整性：只有图形、多媒体、网络、存储等链路全部可用，
应用才立得住；应用反过来也为每个外设域提供了系统级的使用场景。

## 三、目录结构

| 本仓目录 | openvela 构建树位置（manifest 映射） | 用途 |
| --- | --- | --- |
| `chips/esp32p4/` | `vendor/espressif/chips/esp32p4/` | P4 custom chip、Espressif HAL、MIPI-DSI/CSI Host、ESP-Hosted SDIO 传输 |
| `board/esp32p4/common/` | `vendor/espressif/boards/esp32p4/common/` | P4 板级共享代码与链接脚本 |
| `board/esp32p4/esp32p4-function-ev-board/` | `vendor/espressif/boards/esp32p4/esp32p4-function-ev-board/` | 板级 bring-up、40 余个 defconfig、LCD/触摸/摄像头/音频/以太网/C6 装配 |
| `drivers/nuttx/` | 软链接到 `nuttx/drivers/` | GT911、EK79007 竞赛维护驱动源码 + 2 个 V4L2 修复 patch |
| `app/` | `apps/packages/demos/` | `dsi_probe` / `gt911_probe` / `csi_probe` / `video_test` 等独立硬件验证程序 |
| `demos/smart_home/` | `apps/packages/demos/` | 智能家居中控应用（UI、米家、摄像头、cAGENT、网络） |
| `scripts/` | — | 驱动软链接、HAL patch 应用、数据镜像打包、宿主回归测试等 |
| `competition/` | — | 8 份适配域评审证据文档 + 总索引（对齐大赛评分维度） |
| `docs/` | — | 开发计划 / 硬件适配 / 开发日志 / 开发指南全量文档（唯一事实源） |
| `logs/` | — | AI Coding 日志（32 天、80 个会话文件） |

## 四、运行方式

### 1. 拉取完整工程

```bash
repo init -u https://github.com/open-vela/contest2026_031_niudanxianqianchong \
  -b dev-ai-contest-2026 -m contest2026_031_niudanxianqianchong.xml
repo sync -c -j8
```

同步后本仓位于工作区 `contest2026_031_niudanxianqianchong/`，openvela 全量
源码在外层（`nuttx/`、`apps/`、`vendor/` 等），构建在工作区根目录进行。

### 2. 准备驱动软链接

本仓维护 EK79007 与 GT911 源码，构建前映射到本地 NuttX 工作树：

```bash
cd ~/openvela

contest2026_031_niudanxianqianchong/scripts/link_nuttx_display_drivers.sh
contest2026_031_niudanxianqianchong/scripts/link_nuttx_display_drivers.sh --check
```

该脚本只创建开发期驱动软链接，不修改 NuttX 源码或 Kconfig。首次准备环境时，
确认 `nuttx/drivers/input/` 已有 `CONFIG_INPUT_GT911` 和 `gt911.c` 的 Kconfig、
Make、CMake 构建入口；ESP-HAL 兼容补丁按需执行
`scripts/apply_p4x_hal_patches.sh`。

### 3. 选择一个配置构建

以下配置互相独立；每次只构建、烧录并验证一个目标（`configs/` 下共有 40 余个）：

```bash
cd ~/openvela

# 最小 NSH / USB Console
./build.sh \
  contest2026_031_niudanxianqianchong/board/esp32p4/esp32p4-function-ev-board/configs/nsh \
  -j2

# DSI 命令和色条验证
./build.sh \
  contest2026_031_niudanxianqianchong/board/esp32p4/esp32p4-function-ev-board/configs/dsi_probe \
  -j2

# Smart Home LVGL UI + GT911（+ C6 WiFi 网络模式）
./build.sh \
  contest2026_031_niudanxianqianchong/board/esp32p4/esp32p4-function-ev-board/configs/smart_home \
  -j2
```

### 4. 烧录和串口连接

```bash
cd ~/openvela

esptool --chip esp32p4 --port /dev/ttyACM0 --baud 921600 \
  write-flash -fs 16MB -fm dio -ff 80m \
  0x2000 nuttx/nuttx.bin

# 等待 USB Serial/JTAG 重新枚举后连接
picocom -b 115200 /dev/ttyACM0
```

如果烧录后 `/dev/ttyACM0` 暂时消失，请等待设备重新枚举后再启动 `picocom`。

### 5. 写入 Smart Home 运行时资源

P4X 正式 Smart Home UI 从 LittleFS 读取技能、配置、MiSans 字体和 PNG 图标。构建
`smart_home` 固件后生成数据镜像并写入固定的 `0x600000` 分区（10 MiB，
与 defconfig 的 `CONFIG_ESPRESSIF_STORAGE_MTD_OFFSET=0x600000`、
`CONFIG_ESPRESSIF_STORAGE_MTD_SIZE=0xa00000` 一致）：

```bash
cd ~/openvela

contest2026_031_niudanxianqianchong/scripts/make_p4x_littlefs_data_image.sh

esptool --chip esp32p4 --port /dev/ttyACM0 --baud 921600 \
  write-flash -fs 16MB -fm dio -ff 80m \
  0x600000 out/p4x_littlefs_data/data_lfs.bin
```

脚本默认打包完整 `MiSans-Normal.ttf`（约 7.9 MB，设备侧路径
`/data/res/fonts/MiSans-Normal.ttf`，由 LVGL TinyTTF 从 PSRAM 预加载内存创建字体
实例）和 `res/icons/*.png`；不会打包 `secrets.json`。`src/ui/lvgl/icons/*.c` 的
LVGL 字体图标则会直接编译进固件，不在数据镜像中。如需调试最小镜像，可传入
`WITH_FONTS=0 WITH_ICONS=0`。

### 6. 真机验证命令

```text
# dsi_probe 配置
nsh> dsi_probe pattern 10
nsh> dsi_probe video 10

# smart_home 配置
nsh> ls /dev/fb0
nsh> fb
nsh> ls /dev/input0
nsh> gt911_probe 15
nsh> ls /data/res/fonts
nsh> ls /data/res/icons
nsh> smart_home
```

GT911 单指成功时，应观察到类似输出：

```text
DOWN id=0 x=944 y=133 size=30x30 flags=0x59
MOVE id=0 x=944 y=133 size=30x30 flags=0x5a
UP   id=0 x=944 y=133 size=30x30 flags=0x5c
```

按住不动时出现坐标相同的重复 `MOVE` 属于当前驱动保留完整有效帧的策略，不代表
触摸卡死；手指移动时坐标应连续变化，松开后必须出现 `UP`。

## 五、AI Coding 使用说明

本作品全程与 AI 结对开发，覆盖以下环节（完整对话日志见 `logs/`，32 天、
80 个会话文件，codex / claude-code JSONL 格式，含 manifest 索引）：

- **方案设计**：Route A 移植架构、DSI/CSI Host 分期方案、C6 ESP-Hosted 分层
  设计等（见 `docs/开发计划/`）。
- **驱动实现与真机排障**：DBI LP 命令模式黑屏根因定位、C6 WifiSetConfig 旧版
  协议兼容缺陷（补齐 `threshold`/`pmf_cfg` 嵌套消息并附先失败后通过的回归
  用例）、CSI 首帧 Bridge→GDMA 不传输、V4L2 序号重复等问题的闭环。
- **应用功能**：LVGL UI 迭代、米家设备控制的多轮排障（spec 拉取重试、响应
  缓冲、IOB 池扩容、控制抽屉刷新）。
- **工程与文档**：中文提交规范执行、8 份适配证据文档的并行整理与交叉核验
  （`competition/`）。

沉淀的可复用 AI Skill（`.claude/skills/`，可直接被 Claude Code / Codex 等
AI 工具加载）：

| Skill | 用途 |
| --- | --- |
| `openvela-board-bringup` | 新板/新外设 bring-up 系统流程：三种源树风格定位、阶段骨架与四件套验收、probe-first 规则、参考板索引、板级 quirks 沉淀（含 esp32p4x 实例） |
| `openvela-esp32-workflow` | ESP32 构建烧录与真机验证流程 |
| `openvela-git-commit` | 工作区整理、可审阅提交拆分与中文提交说明规范 |

## 附：文档索引

- **评审证据**：[competition/](competition/README.md) —— 8 个适配域各一份证据
  文档（适配流程、真机日志、问题闭环），README 含与大赛评分维度的对应表。
- **全量文档**：[docs/README.md](docs/README.md) —— 开发计划（显示/摄像头/
  网络/应用与 AI）、硬件适配、开发日志（排障闭环）、开发指南的唯一事实源。
- **第三方依赖**：[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

## 开发约定

- 项目源码、文档和补丁只在 `contest2026_031_niudanxianqianchong/` 内维护；
  不将 `nuttx/`、`apps/`、`vendor/` 等公共工作区的临时修改混入本仓提交。
- 板级功能、第三方 HAL 兼容补丁、独立验证程序和文档拆分为可审阅提交。
- 本仓新增内容使用 [Apache License 2.0](LICENSE)。
