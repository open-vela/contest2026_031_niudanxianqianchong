# ESP32-P4X Function EV Board OpenVela 适配

本项目参加 2026 OpenVela AI 硬件开发者大赛“新硬件适配”赛道，以 Route A
（custom chip / custom board）方式适配 **ESP32-P4X-Function-EV-Board**。

当前工作已从最小启动推进到本地显示、静态 LVGL Smart Home 首页和 GT911 单指
触摸事件验证。正式 Smart Home LVGL 离线启动配置已就绪，等待真机验证；云端模型、
网络、MCP、Node 协作和手机 App 尚未进入 P4X 真机验收。

## 当前能力

截至 2026-08-24，已在 P4X 实板完成以下验证：

| 能力 | 状态 | 验证方式 |
| --- | --- | --- |
| USB Serial/JTAG 与 NSH | 通过 | 串口进入 `nsh>`，可执行内建命令 |
| ESP32-P4 芯片层与 Route A 板级启动 | 通过 | `nsh` 固件构建、烧录和启动 |
| EK79007 MIPI-DSI 命令/视频通路 | 通过 | `dsi_probe pattern`、`dsi_probe video` 实板显示色条 |
| NuttX framebuffer | 通过 | `/dev/fb0` 注册，标准 `fb` 示例完成绘制 |
| 静态 LVGL Smart Home 首页 | 通过 | `smart_home` 在 `/dev/fb0` 显示首页并运行 LVGL 定时循环 |
| GT911 单指触摸 | 通过 | `/dev/input0` 和 `gt911_probe` 输出有效 `DOWN/MOVE/UP` |
| 正式 Smart Home LVGL 离线 UI | 待真机验证 | 常规 Agent + LVGL UI，使用 `/dev/fb0`、`/dev/input0`；不初始化网络或 TLS |
| GT911 多点、坐标校准与 LVGL 输入 | 待验证 | P3.2 将 `/dev/input0` 交给正式 LVGL，待验证导航点击和坐标方向 |
| 以太网、DNS、TLS、cAGENT、MCP、Node、App Bridge | 待验证 | 后续按独立阶段启用，避免干扰已验证的显示链路 |

## 快速开始

### 1. 准备驱动软链接

本仓维护 EK79007 与 GT911 源码，构建前将其映射到本地 NuttX 工作树：

```bash
cd ~/openvela

contest2026_031_niudanxianqianchong/scripts/link_nuttx_display_drivers.sh
contest2026_031_niudanxianqianchong/scripts/link_nuttx_display_drivers.sh --check
```

该脚本只创建开发期驱动软链接，不修改 NuttX 源码或 Kconfig。P4X 的 LittleFS MiSans
子集由 LVGL 自带 TinyTTF 读取，不依赖 `nuttx/external` 或外部 FreeType 包；首次切换
到该配置时只需让 `build.sh` 重新配置即可。首次准备环境时，还需确认 `nuttx/drivers/input/` 已有
`CONFIG_INPUT_GT911` 和 `gt911.c` 的 Kconfig、Make、CMake 构建入口；详见
[GT911 适配文档](docs/硬件适配/gt911适配.md)。

### 2. 选择一个配置构建

以下配置互相独立；每次只构建、烧录并验证一个目标。

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

# P3.2 正式 Smart Home LVGL 离线 UI + GT911
./build.sh \
  contest2026_031_niudanxianqianchong/board/esp32p4/esp32p4-function-ev-board/configs/smart_home \
  -j2
```

### 3. 烧录和串口连接

```bash
cd ~/openvela

esptool --chip esp32p4 --port /dev/ttyACM0 --baud 921600 \
  write-flash -fs 16MB -fm dio -ff 80m \
  0x2000 nuttx/nuttx.bin

# 等待 USB Serial/JTAG 重新枚举后连接
picocom -b 115200 /dev/ttyACM0
```

如果烧录后 `/dev/ttyACM0` 暂时消失，请等待设备重新枚举后再启动 `picocom`。

### 4. 写入 Smart Home 运行时资源

P4X 正式 Smart Home UI 从 LittleFS 读取技能、配置、MiSans 字体和 PNG 图标。构建
`smart_home` 固件后生成数据镜像并写入固定的 `0x800000` 分区：

```bash
cd ~/openvela

contest2026_031_niudanxianqianchong/scripts/make_p4x_littlefs_data_image.sh

esptool --chip esp32p4 --port /dev/ttyACM0 --baud 921600 \
  write-flash -fs 16MB -fm dio -ff 80m \
  0x800000 out/p4x_littlefs_data/data_lfs.bin
```

脚本默认仅打包 `MiSans-Normal-subset.ttf` 并重命名为设备侧的
`/data/res/fonts/MiSans-Normal.ttf`，以及 `res/icons/*.png`；不会打包完整字体或
`secrets.json`。`src/ui/lvgl/icons/*.c` 的 LVGL 字体图标则会直接编译进
固件，不在数据镜像中；设备、风扇、灯等核心语义图标即使 LittleFS 中缺少 PNG 仍可显示。
如需调试最小镜像，可传入 `WITH_FONTS=0 WITH_ICONS=0`。

### 5. 真机验证命令

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

## 目录与构建关系

| 本仓目录 | OpenVela 构建树位置 | 用途 |
| --- | --- | --- |
| `chips/esp32p4/` | `vendor/espressif/chips/esp32p4/` | P4 custom chip、Espressif HAL 与 MIPI-DSI Host 适配 |
| `board/esp32p4/common/` | `vendor/espressif/boards/esp32p4/common/` | P4 板级共享代码和链接脚本 |
| `board/esp32p4/esp32p4-function-ev-board/` | `vendor/espressif/boards/esp32p4/esp32p4-function-ev-board/` | P4X 配置、bring-up、DSI framebuffer 与 GT911 装配 |
| `drivers/nuttx/` | 软链接到 `nuttx/drivers/` | EK79007、GT911 的竞赛维护源码 |
| `app/` | `apps/packages/demos/` | `dsi_probe`、`gt911_probe` 等独立硬件验证程序 |
| `demos/smart_home/` | `apps/packages/demos/` | P2 静态首页与 P3.2 正式离线 Smart Home LVGL UI |

不要把构建产生的对象文件、`.depend` 或 `Make.dep` 提交到本仓；它们已在
`.gitignore` 中忽略。

## 分阶段边界

当前 `smart_home` 采用正式 UI 的离线本地模式：初始化 cAGENT 对象、设备状态、工具和
完整 LVGL 页面，但不初始化网络、不编入 TLS、不请求模型，也不启用 MCP、Node Gateway
或 App Bridge。缺少运行时 skills 仅记录状态；这样显示和触摸问题仍可独立定位。

下一阶段按以下顺序推进：

1. 验证正式 UI 已创建 `/dev/input0` 输入设备，完成四角、点击、拖动和滑动校准。
2. 验证 GT911 2~5 点 ID 稳定性、快速滑动和长时间轮询。
3. 单独启用 P4X 网络、DNS 与 TLS，再验证模型 API。
4. 最后启用云端 cAGENT 请求、MCP、Node 协作和手机 App Bridge。

## 文档

| 文档 | 内容 |
| --- | --- |
| [P4X 硬件适配总览](docs/硬件适配/esp32p4-ev-board-adaptation.md) | 硬件差异、适配范围和构建排障 |
| [最小 NSH 操作与测试](docs/硬件适配/esp32p4-nsh-operation-and-test.md) | NSH 构建、烧录和基础上板验证 |
| [GT911 触摸适配](docs/硬件适配/gt911适配.md) | I2C 配置、调用链、问题根因、Probe 与验收 |
| [P4X LVGL 显示与触摸适配计划](docs/开发计划/ESP32-P4X-LVGL显示与触摸适配计划.md) | DSI、framebuffer、LVGL 与触摸阶段计划 |
| [P4X Smart Home Demo 测试计划](docs/开发计划/ESP32-P4X-SmartHome-Demo测试开发计划.md) | P2~P6 的功能边界与验收矩阵 |
| [DSI 黑屏 DBI 配置排障闭环](docs/开发日志/编译/2026-08-24-ESP32-P4X-DSI黑屏DBI配置排障闭环.md) | DBI LP 命令模式根因、修复和视觉验收 |
| [文档索引](docs/README.md) | 全部开发计划和日志入口 |

## 开发约定

- 项目源码、文档和补丁只在 `contest2026_031_niudanxianqianchong/` 内维护。
- 不将 `nuttx/`、`apps/`、`vendor/` 等公共工作区的临时修改混入本仓提交。
- 板级功能、第三方 HAL 兼容补丁、独立验证程序和文档应拆分为可审阅提交。
- 本仓新增内容使用 [Apache License 2.0](LICENSE)；交付前按
  [第三方依赖声明](THIRD_PARTY_NOTICES.md) 复核依赖许可。
