# MIPI-DSI 屏幕显示适配（ESP32-P4X · EK79007）

> **评委导览**：本域以 Route A（custom chip + custom board）方式把 openvela（NuttX 内核）
> 的 MIPI-DSI 显示通路适配到 ESP32-P4X-Function-EV-Board。全部证据来自仓库内开发计划
> 与开发日志，真机验收截至 2026-08-24；"待验证"项按原文保留，未把计划写成已完成。

| 元信息 | 内容 |
| --- | --- |
| 适配对象（屏） | 7 英寸 LCD 模组 AML070JGI50-07403L，1024 x 600，MIPI-DSI 接口 |
| 适配对象（控制器） | EK79007AD + EK73217BCGA（面板 DCS 初始化 + DPI video 接收） |
| 适配方式 | Route A：custom chip `chips/esp32p4`（DSI Host/DPI Panel 芯片层）+ custom board `board/esp32p4/esp32p4-function-ev-board`（板级装配）+ NuttX 通用 EK79007 驱动 |
| 当前真机状态 | `dsi_probe pattern` / `dsi_probe video` 色条真机视觉 PASS；`/dev/fb0` 注册 + 标准 `fb` 示例 PASS；静态 LVGL 首页首屏 PASS（均 2026-08-24）；连续多次启动等回归、DSI error IRQ/FAULT 状态机待验证 |
| 关键代码入口 | `chips/esp32p4/common/espressif/esp_mipi_dsi.c`、`esp_mipi_dsi_dpi_panel.c`、`esp_ldo.c`、`drivers/nuttx/drivers/lcd/ek79007.c`、`board/esp32p4/esp32p4-function-ev-board/src/esp32p4_lcd.c`、`esp32p4_fb.c`、`app/dsi_probe/` |

## 一、适配背景与目标

目标是在 ESP32-P4X-Function-EV-Board 上打通并逐层验收以下显示通路：

```text
ESP32-P4 MIPI-DSI Host（D-PHY 2 lane / 1000 Mbps，LDO channel 3 / 2.5 V）
  -> EK79007 面板（DCS 初始化 + DPI video stream，1024 x 600 RGB565）
  -> PSRAM framebuffer（1,228,800 B）-> NuttX /dev/fb0
  -> LVGL 静态 Smart Home 首页
```

硬件基线（出处：`docs/开发计划/显示与触摸/ESP32-P4X-LVGL显示与触摸适配计划.md` §2）：

| 项目 | 结论 |
| --- | --- |
| 开发板 / 芯片修订 | ESP32-P4X-Function-EV-Board；P4 rev v3.1 及以上，实板识别 v3.2（`CONFIG_ESP32P4_REV_MIN_301=y`） |
| P4 DSI 能力 | 仅 1 个 DSI Host，最多 2 条 data lane；P4X 固定 2 lane、1000 Mbps |
| D-PHY 供电 | 内部 LDO channel 3、2.5 V |
| 面板复位 / 背光 | GPIO27 -> LCD Adapter `RST_LCD`（低有效）；GPIO26 -> Adapter `PWM`（当前静态背光） |

技术难度体现（对应"底层驱动扩展"评分）：在 NuttX 仅有 `mipi_dsi_host` 通用抽象、无 ESP32-P4 DSI 支持的起点上，于 custom chip 层新增 LDO 薄封装、MIPI-DSI 命令/视频 Host、DW-GDMA scanout 与 DPI Panel 对象，并新增通用 EK79007 面板驱动。全程区分两个验收层次（`ESP32-P4X-LCD测试调用全链路.md` §1）：**Host 路径完成**（返回成功、帧计数增长、无错误位）与**视觉显示通过**（实屏显示 1024 x 600 RGB565 八段色条），只有后者才记显示 PASS。

## 二、适配流程

### 阶段 1：MIPI-DSI Host 设计方案（M1/M2 分期）

按 `docs/开发计划/显示与触摸/ESP32-P4-MIPI-DSI-Host设计与实施方案.md`：

```text
M1：命令 Host（D-PHY 供电、PLL/PHY、DCS short/long packet、错误恢复）
M2：视频 Host（DPI 视频时序、framebuffer、DMA 描述符、cache 同步、vsync/error 中断）
```

M2 再细分：M2a Host 内建色条（无 framebuffer，验证 DPI timing/bridge 启动）；M2b GDMA 固定色条（RGB888 诊断路径，DMA 帧计数递增但当时仍黑屏，仅保留为底层诊断）；M2c EK79007 DPI Panel + `draw_bitmap()`，对齐 ESP-IDF 生命周期：先创建 DPI panel，EK79007 完成 DCS/sleep-out，再初始化连续 RGB565 scanout，最后整帧提交色条。分层责任：芯片层 `esp_mipi_dsi.c` 不含面板参数；板级 `esp32p4_lcd.c` 提供电源、复位、背光与 DPI profile；通用 `ek79007.c` 通过 NuttX DCS API 控制面板。

### 阶段 2：Host probe 命令链路（2026-08-21，M1 验收）

新增独立验证程序 `app/dsi_probe/` 与 `configs/dsi_probe/defconfig`，不引入 LVGL、framebuffer 或网络。实板完成（出处：`docs/开发日志/编译/2026-08-21-DSI-Host-Probe排障记录.md`）：LDO acquire、P4 rev3 XTAL reference clock、PHY PLL lock、lane stop、Host ready；generic short/long packet 与 EK79007 默认初始化**写**序列全部被 Host 接受；`DCS GET_POWER_MODE` 请求完成但 RX FIFO 为空，保留为非阻塞诊断（见 §5.1 问题 C）。结论限定（原文）："不能据此宣称 EK79007 已显示、DCS read 已可用或 LVGL 已可运行"。

### 阶段 3：EK79007 初始化与色条

`ek79007_panel_initialize()` 默认发送的 DCS 写序列（出处：
`ESP32-P4X-LCD测试调用全链路.md` §7，逐字）：

```text
0x80 = 0x8b
0x81 = 0x78
0x82 = 0x84
0x83 = 0x88
0x84 = 0xa8
0x85 = 0xe3
0x86 = 0x88
0xb2 = 0x10        # 2-lane PAD_CONTROL
0x11               # Exit Sleep Mode
等待 120 ms
```

每条 DCS 写经 `ek79007_write() -> mipi_dsi_dcs_write() -> mipi_dsi_transfer() ->
host->ops->transfer() -> esp_mipi_dsi_transfer()` 进入 DSI Host command FIFO，再经
D-PHY 到 EK79007。`dsi_probe video` 为贴合已点亮的 ESP-IDF 参考工程，特意跳过 generic packet、DCS `0x29` Display ON 与 DPI 活动期间的 Power Mode 读取。DPI video 参数（同上 §9.1）：1024 x 600 / RGB565；HSync/HBP/HFP = 10/160/160；VSync/VBP/VFP = 1/23/12；请求像素时钟 52 MHz，日志实际 48 MHz（240 MHz / 5）。DW-GDMA 以 64-bit transfer width、循环 LLI 将 PSRAM framebuffer 搬运至 `MIPI_DSI_BRG_MEM_BASE`，启动顺序 `dw_gdma_ll_channel_enable() -> mipi_dsi_host_ll_enable_video_mode() -> mipi_dsi_brg_ll_enable_dpi_output()`。色条：白 -> 黄 -> 青 -> 绿 -> 品红 -> 红 -> 蓝 -> 黑。

### 阶段 4：DBI LP 黑屏排障闭环（2026-08-24）

M2 软件路径全部通过（PLL lock、lane 活动、DMA 帧计数约 61 frame/s 递增、无 DMA error / Bridge underrun）但屏幕始终黑屏；同硬件运行 ESP-IDF 参考工程 `~/Project/espidf_lcd`（提交 1f057d1）可显示色条。与 ESP-IDF `esp_lcd_new_panel_io_dbi()` 对照后定位并修复 **DSI command/DBI 配置缺失**（Generic/DCS/MRPS 命令默认走 HS，面板未可靠完成初始化），修复后 `dsi_probe pattern 10` 与 `dsi_probe video 10` 均真机显示成功，详见 §5.2（出处：`docs/开发日志/编译/2026-08-24-ESP32-P4X-DSI黑屏DBI配置排障闭环.md`）。

### 阶段 5：framebuffer 接入（2026-08-24）

新增 `configs/fb_probe/defconfig`（EK79007 MIPI-DSI `/dev/fb0` smoke test），把已点亮的 DPI/GDMA 扫描通路暴露为标准 NuttX `/dev/fb0`；板级 `esp32p4_fb.c` 负责 framebuffer 装配与 `updatearea()`（应用每次绘制调用 `FBIO_UPDATE` 后执行 DMA 所需的 cache clean）。该配置与 `dsi_probe` 互斥：两者都独占 P4 DSI Host、EK79007 面板和 DMA scanout，不能编入同一固件（出处：`2026-08-24-ESP32-P4X-framebuffer真机验收.md` §1-§2）。

### 阶段 6：LVGL 静态首页（2026-08-24）

`configs/smart_home/defconfig`（P2 阶段）：无网络、无触摸、无业务智能体的静态 LVGL 首页。LVGL NuttX port 打开 board late-init 注册的 `/dev/fb0`，以 1024x600 完成布局与首帧 flush，进入 `lv_timer_handler() + usleep()` 定时循环（出处：`2026-08-24-ESP32-P4X-LVGL静态首页真机验收.md`）。正式 Smart Home LVGL 离线 UI 已就绪、待真机验证（出处：`README.md` 当前能力表）。

## 三、关键代码与配置

### 3.1 代码位置（均已核实存在于本仓）

| 层 | 仓库相对路径 | 职责 |
| --- | --- | --- |
| P4 芯片层 | `chips/esp32p4/common/espressif/esp_mipi_dsi.c/.h` | D-PHY/LDO/PLL/Host、DCS transfer、DBI LP/ACK 配置、DPI video 与 DW-GDMA scanout、DMA buffer 薄封装 |
| P4 芯片层 | `chips/esp32p4/common/espressif/esp_mipi_dsi_dpi_panel.c/.h` | DPI Panel 生命周期、1,228,800 B PSRAM 单帧、整帧 `draw_bitmap()` |
| P4 芯片层 | `chips/esp32p4/common/espressif/esp_ldo.c/.h` | vendor LDO 到 NuttX errno 风格封装 |
| 通用面板驱动 | `drivers/nuttx/drivers/lcd/ek79007.c/.h` | EK79007 DCS 序列、setup/initialize/draw/shutdown、可选 DPI panel 衔接（经 `scripts/link_nuttx_display_drivers.sh` 软链接进 NuttX 工作树） |
| P4X 板级层 | `board/esp32p4/esp32p4-function-ev-board/src/esp32p4_lcd.c` | LDO3/2.5 V、2 lane/1000 Mbps、GPIO27 reset、GPIO26 背光、RGB565 DPI profile |
| P4X 板级层 | `board/esp32p4/esp32p4-function-ev-board/src/esp32p4_fb.c` | `/dev/fb0` 注册与 `updatearea()` cache clean |
| 验证程序 | `app/dsi_probe/dsi_probe_main.c` | command Host / pattern / video 色条测试编排 |
| 固件配置 | `board/esp32p4/esp32p4-function-ev-board/configs/{dsi_probe,fb_probe,smart_home}/defconfig` | 三个互斥的阶段性验证配置 |

NuttX 侧另有通用 MIPI-DSI 框架 `nuttx/drivers/video/mipidsi/`（host/device 抽象、DCS 封装、packet 编码）；芯片层实现并注册 `mipi_dsi_host_ops`，不复制通用编解码。芯片 Kconfig 关系（`2026-08-21-DSI-Host-Probe排障记录.md` §3）：`ESPRESSIF_MIPI_DSI` 须 `select DRIVERS_VIDEO` + `select ESPRESSIF_LDO` + `select MIPI_DSI`。

### 3.2 defconfig 关键项（已核实于各 defconfig / 源文档）

```text
CONFIG_ARCH_CHIP_CUSTOM=y / CONFIG_ARCH_BOARD_CUSTOM=y      # Route A 双 custom 入口
CONFIG_ESP32P4_REV_MIN_301=y
CONFIG_LVX_USE_DEMO_CONTEST2026_031_DSI_PROBE=y             # dsi_probe（dsi_probe/defconfig）
CONFIG_LVX_USE_DEMO_CONTEST2026_031_DSI_PROBE_VIDEO_PATTERN=y
CONFIG_ESPRESSIF_SPIRAM=y / CONFIG_MM_KERNEL_HEAP=y / CONFIG_MM_REGIONS=2
CONFIG_EXAMPLES_FB=y                                        # fb_probe：标准 fb 示例
CONFIG_GRAPHICS_LVGL=y / CONFIG_SMART_HOME_DEMO_UI_LVGL=y   # smart_home
```

### 3.3 构建与烧录命令（出处：`README.md` 快速开始，逐字）

```bash
cd ~/openvela

# DSI 命令和色条验证
./build.sh contest2026_031_niudanxianqianchong/board/esp32p4/esp32p4-function-ev-board/configs/dsi_probe -j2

# P3.2 正式 Smart Home LVGL 离线 UI + GT911
./build.sh contest2026_031_niudanxianqianchong/board/esp32p4/esp32p4-function-ev-board/configs/smart_home -j2

# 烧录（USB Serial/JTAG，0x2000 偏移已验证）
esptool --chip esp32p4 --port /dev/ttyACM0 --baud 921600 \
  write-flash -fs 16MB -fm dio -ff 80m \
  0x2000 nuttx/nuttx.bin
picocom -b 115200 /dev/ttyACM0
```

`fb_probe` 配置路径相同（`configs/fb_probe`，见 framebuffer 验收记录），按同一
`build.sh` 模式构建；三个配置互相独立，每次只构建、烧录并验证一个目标。规避
`build.sh` 自动 `savedefconfig` 覆盖人工 defconfig 的替代流程（`configure.sh -E` +
`make olddefconfig`）见 `2026-08-21-DSI-Host-Probe排障记录.md` §4。

### 3.4 真机验证命令

```text
# dsi_probe 配置：命令 Host / Host 内建色条 / DMA 色条
nsh> dsi_probe
nsh> dsi_probe pattern 10
nsh> dsi_probe video 10
# fb_probe 配置：framebuffer / smart_home 配置：静态 LVGL 首页
nsh> ls /dev/fb0
nsh> fb
nsh> smart_home
```

## 四、适配证据（真机验收）

### 4.1 dsi_probe：命令链路 + pattern/video 色条（视觉 PASS）

M1 命令链路修复后的实板日志（出处：`2026-08-21-DSI-Host-Probe排障记录.md` §6-§7，逐字）：

```text
INFO: MIPI-DSI PHY clock sources cfg=6 pllref=19 ref_hz=40000000
INFO: MIPI-DSI Host ready bus=0
dsi_probe: EK79007 DCS initialisation writes accepted
dsi_probe: PASS command Host validation completed (DCS read=unavailable)
```

DBI 修复后的关键启动日志与视觉结论（出处：
`2026-08-24-ESP32-P4X-DSI黑屏DBI配置排障闭环.md` §5/§7/§8，逐字）：

```text
INFO: MIPI-DSI DBI configured cmd_mode_cfg=010f7f02 command_ack=1 transfer=LP
nsh> dsi_probe pattern 10    # 结果：屏幕成功显示 Host 内建 pattern
nsh> dsi_probe video 10      # 结果：屏幕成功显示 RGB565 DMA 色条
```

`cmd_mode_cfg=0x010f7f02` 的组成（同上 §5）：Command ACK=1，Generic（bits 8-14）、DCS
（bits 16-19）、MRPS（bit 24）LP 模式位全为 1。README 当前能力表对应条目："EK79007
MIPI-DSI 命令/视频通路 | 通过 | `dsi_probe pattern`、`dsi_probe video` 实板显示色条"。
`frames=61` / `frames=613` 等帧计数仅证明 GDMA completion 软件路径运行（出处：
`ESP32-P4X-ESP-IDF-LCD参考实现对比与收敛计划.md` §4），视觉 PASS 以实屏色条为准。

### 4.2 /dev/fb0 注册与 fb 示例绘制（PASS，2026-08-24）

固件配置 `configs/fb_probe`；`fb` 为 NuttX 标准 `apps/examples/fb` 示例，非项目私有
程序。串口证据（出处：`2026-08-24-ESP32-P4X-framebuffer真机验收.md` §2-§3，逐字）：

```text
nsh> ls /dev/fb0
 /dev/fb0

nsh> fb
VideoInfo:
      fmt: 11
     xres: 1024
     yres: 600
  nplanes: 1
PlaneInfo (plane 0):
    fbmem: 0x48000200
    fblen: 1228800
   stride: 2048
  display: 0
      bpp: 16
Mapped FB: 0x48000200
 0: (  0,  0) (1024,600)
 1: ( 93, 54) (838,492)
 2: (186,108) (652,384)
 3: (279,162) (466,276)
 4: (372,216) (280,168)
 5: (465,270) ( 94, 60)
FB test finished
```

已确认：`/dev/fb0` 注册成功；RGB565（fmt=11）、1024x600、单平面、stride 2048；
1,228,800 B PSRAM buffer 可被用户态映射；`FBIO_UPDATE` 回调（板级 `updatearea()`
cache clean）可用。

### 4.3 LVGL 静态首页首屏 + 定时刷新（首屏 PASS，2026-08-24）

固件配置 `configs/smart_home`。串口证据（出处：
`2026-08-24-ESP32-P4X-LVGL静态首页真机验收.md` §2-§3，逐字）：

```text
nsh> ls /dev/fb0
 /dev/fb0

nsh> smart_home
=== Smart Home Static LVGL P2 ===
starting local dashboard without network, cAGENT, or touch

[lvgl-static] lv_init
[lvgl-static] framebuffer=/dev/fb0 resolution=1024x600
[lvgl-static] dashboard shown; entering timer loop
```

实板视觉确认：静态 Smart Home 首页显示正常。已确认范围：`/dev/fb0` 可被 LVGL NuttX
port 打开；LVGL 以 1024x600 完成布局与首帧 flush；页面运行在 `lv_timer_handler() +
usleep()` 循环；启动日志无网络/触摸初始化信息。

## 五、遇到的问题与解决

### 5.1 DSI Host Probe 排障（2026-08-21）

出处：`docs/开发日志/编译/2026-08-21-DSI-Host-Probe排障记录.md`（共 12 项问题，以下为显示链路关键三项）：

**A. P4 rev3 D-PHY PLL 锁定超时（§6）** 现象：冷启动在面板 reset 前失败，
`ERROR: MIPI-DSI timeout stage=phy_pll_lock ...`、`phy_status=00001528`、
`dsi_probe: FAIL step=host_initialize ret=-110`。定位：失败先于 GPIO27 reset，与
adapter、背光、面板命令无关。根因：rev3 D-PHY PLL reference clock mux、时钟源门控
与 lane stop-state 未按硬件修订版完成。修复：`esp_mipi_dsi.c` 补齐时钟源使能、选择
rev3 XTAL reference source 与 divider、查询实际参考频率后再配 PLL，依次等待 PLL lock
与有效 lane stop。复测：`INFO: MIPI-DSI Host ready bus=0`，实板通过。

**B. DCS 写命令误入 BTA/读路径（§7）** 现象：EK79007 首条写 `B2=0x10` 报
`ERROR: MIPI-DSI timeout stage=read_response_fifo ...`、`dsi_probe: DCS command 0xb2
failed ret=-110`。根因：Host 曾以 `mipi_dsi_msg.rx_len` 是否大于零分派读/写，DCS
write helper 只保证发送字段，未定义接收字段导致误入 BTA/RX 分支。修复：改为仅由 DSI
packet data type（`GENERIC_READ_0/1/2_PARAM`、`DCS_READ_0_PARAM`）决定 read/BTA
路径，其余走纯发送。复测：`dsi_probe: EK79007 DCS initialisation writes accepted` +
`PASS command Host validation completed (DCS read=unavailable)`。

**C. DCS GET_POWER_MODE 无 payload（诊断边界，非阻塞）** 现象：`0x0a` 读超时于
`read_response_fifo`，快照 `gen_rd_cmd_busy=0`、`gen_pld_r_empty=1`
（`phy_status=000015bd cmd_pkt_status=00050015`）。结论：Host 已完成 BTA 读请求但
面板未返回 payload，不能区分"面板不支持该读命令"与"面板侧状态/连线因素"；保留为
可选诊断，不阻塞 M1 验收。DBI 修复后该诊断仍可能无返回，只要 pattern/video 视觉
验收通过即不阻塞显示链路验收（`2026-08-24-...DBI配置排障闭环.md` §8）。

### 5.2 DBI LP 命令模式黑屏根因（本项目最关键的排障闭环）

出处：`docs/开发日志/编译/2026-08-24-ESP32-P4X-DSI黑屏DBI配置排障闭环.md`。

- 现象：`dsi_probe pattern 10` 与 `dsi_probe video 10` 均黑屏，但 GPIO26 背光可开、
  PLL lock 保持、高密度 PHY 采样（连续 2000 次、间隔 50 us）显示 clock/data lane
  持续活动、DMA 帧计数约 60 Hz 递增、无 DMA error / Bridge underrun / PLL lock loss；
  同板同屏同 FPC 的 ESP-IDF 工程可显示色条。
- 定位：`pattern`（断开 Bridge 像素输入的 Host 内建 pattern）与 `video` 两条独立像素
  路径同时黑屏，共同前置只有 Host/PHY、面板复位与 EK79007 DCS 初始化，故优先排查
  面板控制链路，对照 ESP-IDF `esp_lcd_new_panel_io_dbi()` 的 9 项 DBI IO 配置。
- 根因：Host 的 `cmd_mode_cfg` 保持复位值，Generic/DCS/MRPS 命令默认使用 HS（P4 rev3
  的 `MIPI_DSI_LL_TRANS_SPEED_HS` 值为 0，恰与复位值相同）；命令能写入 Host FIFO 并
  打印 "writes accepted"，但 Host 接受 packet 不等于面板执行命令，EK79007 未可靠完成
  初始化/退出休眠，视频流即使正常发送屏幕仍只有背光；原实现还多开了 ESP-IDF 未开
  的 RX EoTp。
- 修复：`esp_mipi_dsi_configure_command_mode()`（位于
  `chips/esp32p4/common/espressif/esp_mipi_dsi.c`）逐项调用 P4 HAL DBI speed-mode API：
  关 TE acknowledge、开 command acknowledge、Generic/DCS short/long 读写与 MRPS 全部
  使用 LP，并移除 RX EoTp，不写 magic register。
- 复测：启动日志出现 `INFO: MIPI-DSI DBI configured cmd_mode_cfg=010f7f02
  command_ack=1 transfer=LP`，`pattern 10` 与 `video 10` 均真机视觉 PASS，固定色条
  阶段由"Host PASS、视觉待确认"提升为**真机视觉 PASS**。
- 经验（§9）："Host FIFO 接受命令不等于面板执行命令"；命令面与视频面必须分开验证；
  视觉结果是显示驱动的最终验收门。

### 5.3 M2b 帧缓冲申请失败 `-ENOMEM`（§10）

现象：Host/EK79007 写序列完成后 `video_dma_scanout` 返回 `-12`（ENOMEM），RGB888 单帧
申请量 1,843,200 B，发生在 GDMA 启动之前。根因：P4 HAL 的 NuttX `heap_caps` 兼容层对
非 retention capability 统一走 `kmm_memalign()`，不按 `MALLOC_CAP_SPIRAM` 选 PSRAM；
`CONFIG_MM_KERNEL_HEAP=y` 使 kernel heap 位于有限片内 SRAM，1.84 MiB 申请失败。修复：
`esp_mipi_dsi_dma_buffer_allocate()` 改为 64-byte 对齐 `memalign()`（从 user PSRAM
heap 取帧）、释放改 `free()` 保持配对，Kconfig 增加 `ESPRESSIF_SPIRAM_USER_HEAP`
依赖。复测：`dsi_probe` defconfig 重新配置、编译、链接成功，刷写后运行
`dsi_probe video 60`。

### 5.4 DW-GDMA 依赖与 HAL 头文件边界（§9）

现象 1：纳入 `upper_hal_dma/src/dw_gdma.c` 后链接期报 `dw_gdma_*` 未定义，补入后又
暴露 `freertos/FreeRTOS.h` 缺失；根因是该文件为 ESP-IDF 的 FreeRTOS 上层组件，非
NuttX 可直接复用的 HAL 源；修复为移除其 MK/CMake 构建引用，由 `esp_mipi_dsi.c`
直接使用已纳入构建的 `dw_gdma_ll.h` 配置 DSI 专用固定 channel 0 与 64-byte 对齐循环
LLI，不引入 FreeRTOS 兼容层。现象 2：`Board.mk` 不继承 ESP HAL 头路径，
`esp32p4_lcd.c` 直接包含 `esp_cache.h` 报 `fatal error`；修复为芯片层提供 errno 风格
`esp_mipi_dsi_dma_buffer_*()` 薄封装，vendor capability/cache/`esp_err_t` 不泄漏到板级。

### 5.5 Kconfig 与构建链路问题（custom chip/board 典型难度，§1-§4）

- custom chip Kconfig 未重新展开到旧 `.config`，`ARCH_CHIP_ESP32P4` 应选的
  `CONFIG_ARCH_RV32/RV_ISA_*` 缺失导致 `-march=i`、`-mabi=` 编译错误，以
  `olddefconfig` 统一导出；`MIPI_DSI` 依赖父级 `DRIVERS_VIDEO`，芯片 Kconfig 补
  `select DRIVERS_VIDEO` 后通过。板级/应用裸 `<board.h>` 找不到头文件，正确引用为
  `<arch/board/board.h>` 与 `<arch/chip/esp_mipi_dsi.h>`，DSI/LDO API 导出到
  `chips/esp32p4/include/`。
- `build.sh` 构建后无条件 `make savedefconfig` 回写，删除
  `CONFIG_LVX_USE_DEMO_CONTEST2026_031_DSI_PROBE=y` 等人工项，导致 `nsh> dsi_probe`
  提示 command not found；恢复人工 defconfig 并改用 `configure.sh -E` +
  `make olddefconfig` 流程。

### 5.6 ESP-IDF 对比收敛过程与结论

对照对象：`~/Project/espidf_lcd`（提交 1f057d1），与本项目同板、同屏、同 FPC（出处：
`ESP32-P4X-ESP-IDF-LCD参考实现对比与收敛计划.md`）。逐项差异中判定为 **P0** 的两项：
一是 GDMA 启动顺序，ESP-IDF 为 "DMA enable -> Host video -> Bridge output/update"，
原实现相反，已按参考实现排序修改 `esp_mipi_dsi_video_dma_start()`（该文档 P0 标注
"代码已实现，待实板复测"；`ESP32-P4X-LCD测试调用全链路.md` §9.2 记录的现行顺序
`dw_gdma_ll_channel_enable() -> mipi_dsi_host_ll_enable_video_mode() ->
mipi_dsi_brg_ll_enable_dpi_output()` 与参考实现一致）；二是 Bridge 观测，已采用
"Bridge ISR 锁存 + LPWORK 打印首个 underrun"的最小错误锁存。其余结论：D-PHY 基线与
RGB565 单帧 1,228,800 B 已对齐，非首要变量；面板 reset 时序差异（本项目低 20 ms/
高 120 ms vs ESP-IDF 约 10 ms/20 ms）记录为 A/B 项；实际 DPI 时钟 48 MHz（240/5，
约 55.7 Hz）与目标 52 MHz 的差异需寄存器实测对照。最终点亮由 §5.2 的 DBI LP 配置
完成；该文档写作时点的"软件路径通过、视觉未通过"状态，在 DBI 修复后已更新为两条
路径视觉通过。

## 六、当前验收状态与边界

已通过（真机，截至 2026-08-24，出处：`README.md` 当前能力表 + 各验收日志）：

| 能力 | 状态 | 验证方式 |
| --- | --- | --- |
| MIPI-DSI 命令通路（M1） | 通过（一次实板验证） | `dsi_probe`：Host ready、generic packet、EK79007 初始化写序列 accepted |
| Host 内建 pattern 色条 | 真机视觉 PASS | `dsi_probe pattern 10` 实屏显示 |
| RGB565 DMA 色条（M2c） | 真机视觉 PASS | `dsi_probe video 10` 实屏显示八段色条 |
| NuttX framebuffer | PASS | `configs/fb_probe`：`/dev/fb0` 注册，标准 `fb` 示例六矩形绘制完成 |
| 静态 LVGL 首页 | 首屏 PASS | `configs/smart_home`：`smart_home` 首帧 flush + timer loop |

待验证 / 回归清单（保留原文限定，均为未完成项）：

- 固定色条 PASS 标准中的"连续 10 次启动无黑屏、花屏、DMA fault 或 Bridge underrun"
  回归，及重复 initialize/shutdown 各十次、冷启动复测（`ESP32-P4X-LCD测试调用全链路.md`
  §16；`2026-08-21-DSI-Host-Probe排障记录.md`）；
- DSI error IRQ / 显式 `FAULT` 状态机（设计方案标记"待实现"）；DCS read payload
  （`GET_POWER_MODE` 当前无返回，不阻塞显示验收）；
- framebuffer 回归（`fb` 连续 10 次、长时间静态扫描、LVGL 高频局部刷新、GT911 并发
  输入，`2026-08-24-...framebuffer真机验收.md` §4）；LVGL 回归（连续 10 分钟稳定
  性、重复冷启动、内存基线，`2026-08-24-...LVGL静态首页真机验收.md` §5）；
- CMake 双构建入口回归（设计方案验收矩阵仍标注待回归）；正式 Smart Home LVGL 离线
  UI 待真机验证（`README.md`）。

边界说明：`dsi_probe`、`fb_probe`、`smart_home` 三配置互斥，各独占 DSI Host/面板/DMA；
静态 LVGL 首页刻意不含网络、触摸与业务 UI；GT911 触摸已单指验收，但交给 LVGL 输入属
P3 触摸域，不在本文档展开。

## 附录：原始文档索引

| 文档（仓库相对路径） | 用途 |
| --- | --- |
| `docs/开发计划/显示与触摸/ESP32-P4-MIPI-DSI-Host设计与实施方案.md` | Host M1/M2 分期、接口与状态机、验收矩阵 |
| `docs/开发计划/显示与触摸/ESP32-P4X-LCD测试调用全链路.md` | `dsi_probe video` 全链路调用、DCS 序列、timing、证据边界 |
| `docs/开发计划/显示与触摸/ESP32-P4X-LVGL显示与触摸适配计划.md` | 硬件基线、分层归属、P0-P6 阶段门、内存预算 |
| `docs/开发计划/显示与触摸/ESP32-P4X-ESP-IDF-LCD参考实现对比与收敛计划.md` | 与可显示 ESP-IDF 工程的逐项差异与收敛顺序 |
| `docs/开发日志/编译/2026-08-21-DSI-Host-Probe排障记录.md` | M1 实板验收、12 项构建/链路问题与修复 |
| `docs/开发日志/编译/2026-08-24-ESP32-P4X-DSI黑屏DBI配置排障闭环.md` | DBI LP 黑屏根因、修复与真机视觉 PASS |
| `docs/开发日志/编译/2026-08-24-ESP32-P4X-framebuffer真机验收.md` | `/dev/fb0` 注册与 `fb` 示例 PASS |
| `docs/开发日志/编译/2026-08-24-ESP32-P4X-LVGL静态首页真机验收.md` | 静态 LVGL 首页首屏 PASS |
| `README.md` | 当前能力状态表、构建/烧录/验证命令 |
