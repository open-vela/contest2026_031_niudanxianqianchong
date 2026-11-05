# ESP32-P4X 智能家居中控面板 —— openvela 新硬件全栈适配

## 一、作品简介

本作品以 **Route A（custom chip + custom board）** 方式，将 openvela（NuttX 内核）
从零适配到 **ESP32-P4X-Function-EV-Board**，并在这块新硬件上落地了一个
**智能家居中控面板**应用：1024×600 MIPI-DSI 横屏 UI（light-calm 浅色主题、
完整 MiSans 字体）、米家设备卡片与控制、摄像头实时预览通路、语音输入与播报
（端侧 KWS + 云端 ASR/TTS），以及经板载 ESP32-C6 托管 WiFi 接入云端大模型
（cAGENT）的对话控制。

作品的两条主线：

1. **新硬件适配**：P4 芯片层（含 Espressif HAL 与 MIPI-DSI/CSI Host）+ 板级
   bring-up + 8 个外设域驱动逐项真机验收（7 域通过，以太网待板测），并完成
   HP 双核 SMP、PPA/2D-DMA 图像加速与 ESP-NN INT8 算子后端接入；携带 2 个
   nuttx 通用 V4L2 修复 patch，57 个独立 defconfig 配置矩阵与 probe 独立验证程序。
2. **智能家居应用**：在适配完成的硬件上运行 SmartHome 中控 Demo，覆盖
   openvela 大赛判定标准的**图形**（LVGL/DSI）、**多媒体**（摄像头/音频）、
   **AI**（cAGENT 云端模型对话 + 端侧 KWS 唤醒 + 云端 ASR/TTS）三项核心能力。

### 硬件适配能力（真机验收，截至 2026-11-05）

| 能力域 | 状态 | 证据 |
| --- | --- | --- |
| USB Serial/JTAG 控制台 + NSH 最小系统 | ✅ 通过 | [competition/01](competition/开发日志/01-系统启动与芯片移植.md) |
| MIPI-DSI 显示（EK79007，1024×600）：色条 → `/dev/fb0` → LVGL 首页 | ✅ 通过 | [competition/02](competition/开发日志/02-MIPI-DSI屏幕显示适配.md) |
| GT911 触摸（I2C，`/dev/input0`）：单指 DOWN/MOVE/UP + 触摸交互可用 | ✅ 通过 | [competition/03](competition/开发日志/03-触摸GT911适配.md) |
| SC2336 摄像头（MIPI-CSI，V4L2 RGB565）：300 帧 `app_fps=30.02` | ✅ 通过 | [competition/04](competition/开发日志/04-MIPI-CSI摄像头SC2336适配.md) |
| WiFi（板载 C6，SDIO + ESP-Hosted）：枚举→关联→DHCP→DNS→TLS/模型对话 | ✅ 通过（2026-09-20 真机闭环） | [competition/05](competition/开发日志/05-WiFi-C6托管适配.md) |
| 以太网（P4 内置 EMAC，`eth0`） | 方案+代码就绪，待真机 | [competition/06](competition/开发日志/06-以太网适配.md) |
| ES8311 音频（I2S/GDMA）：16 kHz 单声道播放/录音、录→存→放闭环 | ✅ 通过（10 s 录音 320000 B，字节数精确，人声清楚） | [competition/07](competition/开发日志/07-音频ES8311适配.md) / [11](competition/开发日志/11-音频ES8311录放闭环验收.md) |
| LittleFS 数据分区 + 完整 MiSans（7.9 MB）PSRAM 预加载 | ✅ 通过 | [competition/08](competition/开发日志/08-存储LittleFS与系统集成.md) |
| PPA + 2D-DMA 图像加速：copy/fill/srm/blend/convert 五项 | ✅ 五项全 PASS，100 + 50 轮压测 0 失败，堆用量零变化 | [competition/10](competition/开发日志/10-PPA与2D-DMA图形加速适配.md) |
| HP 双核 SMP：跨核调度、PSRAM/Flash 并发安全、LittleFS 50 轮 | ✅ 通过（含 `ostest` 全量） | [competition/12](competition/开发日志/12-SMP双核适配验收.md) |
| ESP-NN INT8 算子 P4 后端（TFLM，xespv PIE 指令） | ⚠️ 接入生效，`kws_smoke` p50=2.38 s；逐算子数值比对未执行 | [competition/13](competition/开发日志/13-ESP-NN-P4后端接入与验收.md) |

> 前 8 行是 01–08 的 8 个外设域（7 通过 + 以太网待板测）；后 3 行是适配过程中
> 额外完成的平台能力（PPA/2D-DMA、HP 双核 SMP、ESP-NN 算子后端），不计入 8 域。
> 逐域证据文档（适配流程 / 真机日志 / 问题闭环）见 `competition/`，
> 全部日志逐字摘录并注明出处，"待验证"边界如实标注。

### 智能家居应用能力

| 能力 | 状态 |
| --- | --- |
| LVGL 中控面板 UI（1024×600 横屏、light-calm 浅色主题、完整中文字体） | V9 界面：三项一级导航（首页/聊天/设置），首页含天气头、房间筛选、横向设备卡与场景下拉；设置页七类入口（显示与声音/网络连接/模型服务/语音服务/工具权限/设备与服务/系统健康） |
| 米家设备接入（设备卡片、控制抽屉、Agent 工具控制） | ✅ 真机闭环：UI 手动控制 + Agent 控制（LLM→摄像机真实执行），七层排障闭环见 [competition/09](competition/开发日志/09-智能家居应用.md) |
| 摄像头实时预览（`/dev/video0` V4L2 通路接入 UI） | 视频通路与 UI 内实时预览均已真机验收（30 fps） |
| 云端模型对话（cAGENT，经 C6 WiFi → TLS → DeepSeek） | ✅ 真机闭环：TLS 握手/请求/响应全通，支撑米家 Agent 控制 |
| 语音链路：云端 ASR / 云端 TTS / VAD（`smart_home_voice`） | 板端采音、短句云端 TTS 听感通过（“你好”清楚可辨）；较长文本正文接收超时未定位，ASR→LLM→TTS 全链路未验收 |
| 端侧 KWS 唤醒（TFLM + `wake_large` 模型，`smart_home_kws`） | ⚠️ `kws_smoke` 单次推理 p50=2.38 s，与模型契约一致；距 20 ms 推理步长的实时要求仍有数量级差距，实时唤醒未验收 |
| 提醒与闹钟（`smart_home_alarms`） | 最多 8 条持久化倒计时/一次性闹钟，落盘 `/data/smart_home/reminders.json`，到点播本地 PCM；待板测 |
| MCP / Node 网关 | `smart_home` 默认配置开启（`CONFIG_SMART_HOME_MCP_BRIDGE`、`CONFIG_SMART_HOME_NODE_GATEWAY`） |
| 智能管家 Skill 体系（8 份技能文档 + `read_skill` 按需加载 + LLM 可见摘要） | 米家控制/设备路由/场景模式/安全/计时/天气/设备控制/用户偏好记忆技能随数据分区部署，模型按需读取，新设备类型零代码接入 |
| 智能家居工具（米家统一工具 + 本地查询/场景工具 + 用户偏好） | `miot_device_list`/`miot_device_control` 统一控制真实米家设备；`run_scene` 场景模式经 type_name 语义匹配真实设备（睡眠→夜视自动+人形追踪开）；天气/计时等本地工具经 wttr.in 真实数据；`user_memory` 持久化长期偏好（`CONFIG_SMART_HOME_USER_MEMORY`） |

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
| `chips/esp32p4/` | `vendor/espressif/chips/esp32p4/` | P4 custom chip、Espressif HAL、MIPI-DSI/CSI Host、ESP-Hosted SDIO 传输、2D-DMA/PPA |
| `board/esp32p4/common/` | `vendor/espressif/boards/esp32p4/common/` | P4 板级共享代码与链接脚本 |
| `board/esp32p4/esp32p4-function-ev-board/` | `vendor/espressif/boards/esp32p4/esp32p4-function-ev-board/` | 板级 bring-up、57 个 defconfig、LCD/触摸/摄像头/音频/以太网/C6 装配 |
| `drivers/nuttx/` | 软链接到 `nuttx/drivers/` | GT911、EK79007 竞赛维护驱动源码 + 2 个 V4L2 修复 patch |
| `patches/nuttx/` | — | 15 个 NuttX 侧补丁（ES8311 音频修复、RISC-V 自定义 SMP、CMake 多链接脚本、乐鑫工具链等） |
| `third_party/` | 经 `scripts/link_esp_nn.sh` 链接到 `apps/mlearning/` | `esp-nn` 上游源码与 TFLM ESP-NN adapter（P4 PIE 内核） |
| `app/` | `apps/packages/demos/` | `dsi_probe` / `gt911_probe` / `csi_probe` / `video_test` / `graphics_probe` / `smp_flash_test` / `smp_psram_probe` 独立硬件验证程序 |
| `demos/smart_home/` | `apps/packages/demos/smart_home` | 智能家居中控应用（V9 UI、米家、摄像头、语音链路、cAGENT、网络） |
| `packages/cAGENT`、`packages/cagent_addons` | `apps/packages/cAGENT` 等 | 云端 Agent 与插件（构建 `smart_home` 必需） |
| `scripts/` | — | 驱动软链接、HAL patch 应用、工具链准备、数据镜像打包、宿主回归测试等 |
| `competition/` | — | 13 份评审证据文档 + 总索引（对齐大赛评分维度） |
| `docs/` | — | 开发计划 / 硬件适配 / 开发日志 / 开发指南全量文档（唯一事实源） |
| `logs/` | — | AI Coding 日志（40 天、100 个会话文件，2026-06-19 ~ 2026-11-04） |

## 四、运行方式

### 1. 拉取完整工程

```bash
repo init -u https://github.com/open-vela/contest2026_031_niudanxianqianchong \
  -b dev-ai-contest-2026 -m contest2026_031_niudanxianqianchong.xml
repo sync -c -j8
```

同步后本仓位于工作区 `contest2026_031_niudanxianqianchong/`，openvela 全量
源码在外层（`nuttx/`、`apps/`、`vendor/` 等），构建在工作区根目录进行。

### 2. 准备驱动与应用软链接

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

**应用目录不在 manifest 的 `<linkfile>` 里，`repo sync` 后必须手工建软链**，
否则 `apps/` 扫描不到 `smart_home` 与各 probe，构建会在 `SMART_HOME_DEPS cagent`
处失败（找不到 `packages/cAGENT`）：

```bash
cd ~/openvela

# 云端 Agent 与应用本体（smart_home 的 CMake 依赖 packages/cAGENT）
ln -sfn ../contest2026_031_niudanxianqianchong/packages/cAGENT          apps/packages/cAGENT
ln -sfn ../contest2026_031_niudanxianqianchong/packages/cagent_addons   apps/packages/cagent_addons
ln -sfn ../../contest2026_031_niudanxianqianchong/demos/smart_home      apps/packages/demos/smart_home

# 独立验证程序（manifest 只登记了 hello_app / dsi_probe / csi_probe）
for p in gt911_probe video_test graphics_probe smp_flash_test smp_psram_probe; do
  ln -sfn "../../contest2026_031_niudanxianqianchong/app/$p" \
          "apps/packages/demos/contest2026_031_$p"
done
```

校验（每条都应指向本仓内的真实目录，不要出现断链）：

```bash
ls -l ~/openvela/apps/packages/demos/ | grep contest2026
ls -l ~/openvela/apps/packages/cAGENT ~/openvela/apps/packages/cagent_addons
```

ESP-NN 后端还需要把上游源码挂到 `apps/mlearning/`（`smart_home_kws` 配置）：

```bash
contest2026_031_niudanxianqianchong/scripts/link_esp_nn.sh
contest2026_031_niudanxianqianchong/scripts/link_esp_nn.sh --check
```

### 3. 选择一个配置构建

以下配置互相独立；每次只构建、烧录并验证一个目标（`configs/` 下共有 57 个）：

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

# Smart Home LVGL UI + GT911（+ C6 WiFi 网络模式 + 提醒/闹钟 + MCP/Node 网关）
./build.sh \
  contest2026_031_niudanxianqianchong/board/esp32p4/esp32p4-function-ev-board/configs/smart_home \
  -j2

# 语音链路：VAD + 云端 ASR + 云端 TTS（audio_smoke / asr_smoke / tts_smoke）
./build.sh \
  contest2026_031_niudanxianqianchong/board/esp32p4/esp32p4-function-ev-board/configs/smart_home_voice \
  -j2
```

其余常用目标可直接走 `./build.sh`：`dma2d` / `ppa`（PPA 与 2D-DMA）、
`video_test` / `graphics_probe` / `csi_probe` / `gt911_probe`（独立 probe）。
带 TFLM/ESP-NN 的 `smart_home_kws` 与 `smp_smart_home` 见下一节，必须用
`build_p4x.sh`。

`smart_home_alarms` 是提醒/闹钟专项配置：`smart_home` 只开
`CONFIG_SMART_HOME_REMINDERS=y`，响铃 `CONFIG_SMART_HOME_REMINDER_RING` 靠 Kconfig
默认值；`smart_home_alarms` 把它显式固定下来，便于单独回归。它的构建入口会临时切换
`packages/demos/smart_home`、`packages/cAGENT`、`packages/cagent_addons` 三个软链到
指定工作树，因此必须用这个脚本（构建前后自动恢复软链），不要直接 `./build.sh`：

```bash
# 在 openvela 根目录执行，脚本会自行定位工作区
contest2026_031_niudanxianqianchong/scripts/build_smart_home_alarms.sh -j8
```

### 3.1 `smart_home_kws` 与 `smp_smart_home` 需要乐鑫工具链（必须用 `build_p4x.sh`）

这两个配置开启了 `CONFIG_ESP32P4_ESPRESSIF_TOOLCHAIN`，用乐鑫
`riscv32-esp-elf` 工具链构建（与 ESP32-S3 用 `xtensa-esp32s3-elf` 同理）。
该开关是为 ESP-NN 的 PIE 指令准备的，因此凡是带 TFLM/ESP-NN 的目标都必须用它；
其余配置仍用 OpenVela 自带的 `riscv-none-elf`，行为不变。

**这两个配置都不要用 `./build.sh`。** `build.sh` 不会应用 `patches/nuttx/*`，
开关会被 Kconfig 静默丢弃，最终编出的是 xPack 的镜像，而且没有任何提示。

正确步骤：

```bash
cd ~/openvela/contest2026_031_niudanxianqianchong

# 1. 准备工具链（约 430 MB，不入库，位置 prebuilts/gcc/linux-x86_64/）
scripts/setup_toolchain.sh --check    # 已装好时只校验
scripts/setup_toolchain.sh            # 未安装时下载 + 校验 + 裁剪
scripts/setup_toolchain.sh --source ~/.espressif/tools/riscv32-esp-elf/esp-14.2.0_20260121/riscv32-esp-elf
                                      # 或从本机已有安装复制

# 2. 构建（会自动打补丁、切换编译器，工具链缺失时硬报错）
scripts/build_p4x.sh smp_smart_home -j8
scripts/build_p4x.sh smart_home_kws -j8
```

第一行输出应是：

```text
CC: .../prebuilts/gcc/linux-x86_64/riscv32-esp-elf/bin/riscv32-esp-elf-gcc (14.2.0)
```

看到 `riscv-none-elf-gcc` 就说明开关没生效，先停下来排查。

工具链的裁剪内容、查找顺序、回滚方式见
[docs/开发日志/编译/ESP32-P4X-乐鑫工具链S0-S3接入记录.md](docs/开发日志/编译/ESP32-P4X-乐鑫工具链S0-S3接入记录.md)。

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

# graphics_probe 配置（PPA / 2D-DMA）
nsh> graphics_probe all 5
nsh> graphics_probe copy 100        # 单项压测，其余 fill/srm/blend/convert 同理
nsh> graphics_probe matrix 20       # SRAM/PSRAM 组合矩阵

# smart_home 配置
nsh> ls /dev/fb0
nsh> fb
nsh> ls /dev/input0
nsh> gt911_probe 15
nsh> ls /data/res/fonts
nsh> ls /data/res/icons
nsh> smart_home

# smart_home_voice 配置（音频与语音链路）
nsh> audio_smoke play 1
nsh> audio_smoke record 10 /data/voice_gate_10s.pcm
nsh> audio_smoke playfile /data/voice_gate_10s.pcm
nsh> asr_smoke record 6
nsh> asr_smoke record-vad 6 500
nsh> tts_smoke speak 你好，我是智能家居管家

# smart_home_kws 配置（端侧唤醒）
nsh> kws_smoke --warmup 5 --repeat 50
```

录音 10 秒应得到精确的 320000 字节（16 kHz × 16 bit × 单声道 × 10 s）。

GT911 单指成功时，应观察到类似输出：

```text
DOWN id=0 x=944 y=133 size=30x30 flags=0x59
MOVE id=0 x=944 y=133 size=30x30 flags=0x5a
UP   id=0 x=944 y=133 size=30x30 flags=0x5c
```

按住不动时出现坐标相同的重复 `MOVE` 属于当前驱动保留完整有效帧的策略，不代表
触摸卡死；手指移动时坐标应连续变化，松开后必须出现 `UP`。

## 五、AI Coding 使用说明

本作品全程与 AI 结对开发，覆盖以下环节（完整对话日志见 `logs/`，40 天、
100 个会话文件，2026-06-19 ~ 2026-11-04，codex / claude-code JSONL 格式，含
manifest 索引）：

- **方案设计**：Route A 移植架构、DSI/CSI Host 分期方案、C6 ESP-Hosted 分层
  设计等（见 `docs/开发计划/`）。
- **驱动实现与真机排障**：DBI LP 命令模式黑屏根因定位、C6 WifiSetConfig 旧版
  协议兼容缺陷（补齐 `threshold`/`pmf_cfg` 嵌套消息并附先失败后通过的回归
  用例）、CSI 首帧 Bridge→GDMA 不传输、V4L2 序号重复等问题的闭环。
- **应用功能**：LVGL V9 界面重写、米家设备控制的多轮排障（spec 拉取重试、响应
  缓冲、IOB 池扩容、控制抽屉刷新）、语音链路（VAD/ASR/TTS 阶段验收）、端侧
  KWS 与 ESP-NN 后端接入、SMP 双核并发问题定位。
- **工程与文档**：中文提交规范执行、13 份证据文档的并行整理与交叉核验
  （`competition/`）。

沉淀的可复用 AI Skill（`.claude/skills/`，可直接被 Claude Code / Codex 等
AI 工具加载）：

| Skill | 用途 |
| --- | --- |
| `openvela-board-bringup` | 新板/新外设 bring-up 系统流程：三种源树风格定位、阶段骨架与四件套验收、probe-first 规则、参考板索引、板级 quirks 沉淀（含 esp32p4x 实例） |
| `openvela-esp32-workflow` | ESP32 构建烧录与真机验证流程 |
| `openvela-git-commit` | 工作区整理、可审阅提交拆分与中文提交说明规范 |
| `openvela-worktree-workflow` | 多 agent 并行开发的工作树纪律：主检出只读、分支隔离、补丁可重放 |
| `lvgl-host-review` | LVGL 界面的宿主端 RGB565 framebuffer 渲染与视觉对齐审查 |

## 附：文档索引

- **评审证据**：[competition/](competition/README.md) —— 13 份证据文档（01–08 适配域
  + 09 智能家居应用 + 10–13 PPA/SMP/ESP-NN/音频闭环），README 含与大赛评分维度的对应表。
- **全量文档**：[docs/README.md](docs/README.md) —— 开发计划（显示/摄像头/
  网络/应用与 AI）、硬件适配、开发日志（排障闭环）、开发指南的唯一事实源。
- **第三方依赖**：[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

## 开发约定

- 项目源码、文档和补丁只在 `contest2026_031_niudanxianqianchong/` 内维护；
  不将 `nuttx/`、`apps/`、`vendor/` 等公共工作区的临时修改混入本仓提交。
- 板级功能、第三方 HAL 兼容补丁、独立验证程序和文档拆分为可审阅提交。
- 本仓新增内容使用 [Apache License 2.0](LICENSE)。
