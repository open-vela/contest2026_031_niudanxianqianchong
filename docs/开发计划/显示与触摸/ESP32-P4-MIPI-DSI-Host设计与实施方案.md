# ESP32-P4 MIPI-DSI Host 设计与实施方案

## 1. 目的与边界

本文定义 ESP32-P4 的 MIPI-DSI Host 适配边界、接口和实施验收门，服务于
`ESP32-P4X-Function-EV-Board` 的 EK79007 面板显示。它是
[ESP32-P4X-LVGL 显示与触摸适配计划](ESP32-P4X-LVGL显示与触摸适配计划.md)的芯片层专项设计，
不替代该计划中的板级装配和 LVGL 验收。

首期将 Host 分为两个独立里程碑：

```text
M1：命令 Host
  D-PHY 供电、PLL/PHY、MIPI DSI DCS short/long packet、错误恢复

M2：视频 Host
  DPI 视频时序、framebuffer、DMA 描述符、cache 同步、vsync/error 中断
```

M1 的目标是可靠地初始化面板和发送 DCS 命令；它**不**承诺已经具备持续像素
扫描输出能力。M2 完成前不得宣称已支持 framebuffer 或 LVGL 显示。

本文不包含 EK79007 寄存器表、GPIO27 复位、GPIO26 背光、GT911 触摸和业务
UI；这些分别属于通用面板驱动、P4X 板级层和输入层。

## 1.1 实施状态（2026-08-21）

M1 的芯片层、P4X command-mode 板级装配和独立 Probe 已完成；2026-08-21 已在
实板完成 command-write 路径验证。该证据不包含 DPI video、framebuffer 或实际
画面，故状态仍严格区分“命令写已验证”与“显示待验收”。

| 项目 | 状态 | 说明 |
| --- | --- | --- |
| LDO 薄封装 | 已实现 | `esp_ldo.c/.h` 封装 ESP HAL channel acquire/release/adjust，向上返回 NuttX 负 errno。 |
| MIPI DSI command Host | 已实现并完成命令写实板验证 | `esp_mipi_dsi.c/.h` 完成 bus 0、1/2 lane、P4 rev3 XTAL reference clock、PHY PLL、lane stop、NuttX Host 注册与 short/long/DCS packet。 |
| 等待与并发 | 已实现并完成路径修正 | PLL、lane stop 与 FIFO 使用超时轮询；Host 初始化、attach、transfer、shutdown 由同一 mutex 串行化；BTA 仅由 DSI read data type 触发。 |
| Kconfig 与双构建入口 | 已实现 | `ESPRESSIF_MIPI_DSI` 自动选择 `MIPI_DSI`、`ESPRESSIF_LDO`；Make/CMake 同时接入 Host 与 vendor HAL 源。 |
| DSI error IRQ / `FAULT` 状态机 | 待实现 | 当前仅以 `ready`、`registered` 表达可用状态，传输错误直接返回 errno。 |
| `dsi_probe` / P4X 板级 command 装配 | 命令写已实板验证 | P4X 固定 LDO3/2.5V、2 lane/1000 Mbps、GPIO27 active-low reset；generic packet 与 EK79007 初始化写序列通过。`GET_POWER_MODE` 未收到 payload，仅作为非阻塞诊断。 |
| M2a Host 内建色条 | Host 配置已实板验证，但未显示 | 可确认 DPI timing/bridge 寄存器启动，不是实际像素流证据。 |
| M2b GDMA 固定色条 | Host/DMA 已实板运行，视觉显示未通过 | 旧的板级 RGB888 固定色条已确认 DMA 帧计数持续递增，但屏幕仍黑；不能将 DMA 周期完成视为像素链路成功。 |
| M2c EK79007 DPI Panel + `draw_bitmap()` | 代码已实现，待构建与实板视觉验收 | 对齐 ESP-IDF 生命周期：先创建 DPI panel，EK79007 完成 DCS/sleep-out，再初始化连续 RGB565 scanout，最后通过 `draw_bitmap()` 提交一帧色条。 |

> 当前结论：M1 的 Host 与 EK79007 command-write 链路已通过一次实板验证；不能
> 据此宣称 EK79007 已显示、DCS read 已可用或 LVGL 已可运行。

## 2. 已验证的硬件与软件约束

| 项目 | 约束 | 设计含义 |
| --- | --- | --- |
| DSI Host 数量 | ESP32-P4 仅有 1 个 DSI Host | Host API 的 bus 参数首期只能接受 bus 0，仍保留字段以避免接口重写。 |
| 数据 lane | P4 Host 最多 2 条 data lane | 板级配置必须确认面板工作在 1/2 lane 模式；不得按 FPC 引脚数量假设 4 lane。 |
| D-PHY 电源 | D-PHY 需要独立、稳定的 2.5 V 电源 | 使用 P4 可用 LDO 通道供电；具体 VO 通道、上电时序以 P4X 原理图为准。 |
| 面板 | AML070JGI50-07403L，1024 x 600，EK79007AD + EK73217BCGA | 分辨率与 video timing 是面板/板级参数，不能硬编码为通用芯片配置。 |
| 通用框架 | NuttX 已提供 `mipi_dsi_host`、DCS 与 packet 抽象 | Host 必须实现并注册 `mipi_dsi_host_ops`，不复制通用 packet 编解码。 |
| Vendor HAL | P4 HAL 已提供 DSI 寄存器定义和部分 HAL | 仅在芯片层封装；上层不得包含 ESP-IDF 私有类型或头文件。 |

硬件资料参考：[ESP32-P4 Function EV Board 用户指南](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32p4/esp32-p4-function-ev-board/user_guide.html)、[ESP-IDF MIPI-DSI LCD 文档](https://docs.espressif.com/projects/esp-idf/en/latest/esp32p4/api-reference/peripherals/lcd/dsi_lcd.html)。开始 M1 前应将面板的 lane 数、lane bit rate、像素格式、水平/垂直 porch、同步极性和实际 LDO 通道记录到板级硬件连接表。

## 3. 分层与责任边界

```text
NuttX MIPI-DSI 通用 API
  mipi_dsi_host_register() / packet / DCS
              │
ESP32-P4 芯片层
  esp_mipi_dsi.c              Host、PHY、命令传输、M2a pattern、M2b GDMA scanout
  esp_mipi_dsi_dpi_panel.c    DPI panel 生命周期、持久帧缓冲与 draw_bitmap
  esp_ldo.c                   LDO vendor API 到 errno 风格的薄封装
              │
ESP HAL / 寄存器层
  mipi_dsi_hal.c / mipi_dsi_periph.c / DSI LL / GDMA HAL
              │
P4X 板级层
  2.5V D-PHY 电源、reset、背光、面板时序、实例装配
```

芯片层只理解 P4 的硬件能力，不应包含面板初始化命令、面板分辨率或 GPIO 编号。
板级层选择面板参数并持有设备实例；通用 EK79007 驱动通过 DCS API 控制面板。

## 4. Host 接口与状态机

芯片私有头文件已提供不暴露 ESP-IDF 类型的接口。M1 实际接口如下：

```c
struct esp_mipi_dsi_host_config_s
{
  uint8_t bus;                 /* P4 首期仅接受 0 */
  uint8_t lane_num;            /* 1 或 2，由板级传入 */
  uint32_t lane_bit_rate_mbps; /* 80--1500 Mbps */
  uint32_t phy_ref_clock_hz;   /* 5--40 MHz */
  uint32_t timeout_ms;
  struct esp_ldo_config_s phy_ldo; /* 板级指定的 2.5 V D-PHY LDO */
};

int esp_mipi_dsi_host_initialize(
  FAR const struct esp_mipi_dsi_host_config_s *config,
  FAR struct mipi_dsi_host **host);
int esp_mipi_dsi_host_shutdown(FAR struct mipi_dsi_host *host);
```

返回值统一转换为 NuttX errno 负值；vendor `esp_err_t`、寄存器地址和 HAL 私有
对象停留在 `.c` 文件内部。M2a 保留 framebuffer-free 的 DPI pattern 配置结构；M2b
提供 `esp_mipi_dsi_video_dma_start()`，接受调用方持有的 RGB565/RGB888 buffer 地址和
长度，不暴露 GDMA handle、LLI 或 ESP HAL 私有类型。该 API 的 buffer 必须持续有效至
`esp_mipi_dsi_video_stop()` 返回。M2c 在其上增加
`esp_mipi_dsi_dpi_panel_{create,initialize,draw_bitmap,stop,destroy}()`：DPI panel
对象持有一块持续有效的 PSRAM 单帧，应用仅经 `draw_bitmap()` 提交整帧数据。

NuttX 当前只有 `mipi_dsi_host_register()`，没有对应 unregister API。因此 Host
结构是静态单例：首次 initialize 注册一次；`shutdown()` 只关闭硬件并释放 LDO；
再次 initialize 重用同一 Host。板级代码不得释放 Host 指针。

目标 Host 生命周期如下，其中粗体部分已实现：

```text
OFF -> **LDO_READY** -> **PHY_READY** -> **COMMAND_READY**
  -> VIDEO_CONFIGURED       （M2a：DPI pattern）
  -> VIDEO_RUNNING          （M2a：DPI pattern；M2b：GDMA scanout；M2c：DPI panel）
  -> FAULT

shutdown：先停 video/Bridge 输出，再释放 M2b 的 GDMA channel/LLI，最后关闭 DPI、
PHY、时钟并释放 LDO。
```

当前只有 `ready` 布尔状态，只有为真时允许 DCS transfer。PLL/FIFO 超时和 HAL
调用失败返回 errno；DSI error IRQ、首错保存、显式 `FAULT` 与恢复状态机是 M1
验收闭环前必须补齐的后续工作。

## 5. M1：命令 Host 实施方案

### 5.1 初始化顺序

已实现的初始化顺序：

1. 校验 bus 为 0、`lane_num` 为 1 或 2、bit rate 为 80--1500 Mbps、PHY 参考
   时钟为 5--40 MHz，并要求板级传入的 LDO 电压严格为 2.5 V。
2. 申请并配置 D-PHY LDO channel。
3. 开启 DSI bus clock；启用 PHY configuration/PLL reference source，在 P4 rev3
   选择 XTAL reference source、设置 divider，并查询实际参考频率。
4. 在 RCC 原子区启用 PHY gate、初始化 vendor HAL、配置 PHY PLL，依次等待
   PLL lock 与有效 lane stop state。
5. 固定到 command mode，配置 clock lane、switch time、CRC/ECC、EoTP、
   escape/timeout clock 与最大读时间，并建立 `mipi_dsi_host_ops`。
6. 首次成功初始化时调用 `mipi_dsi_host_register()`；再次初始化只重启硬件，
   不重复注册 Host。

`transfer()` 通过 `mipi_dsi_create_packet()` 复用 NuttX packet 编码：long packet
按 32 bit 写 FIFO，short packet 写 header；读请求先发送 Maximum Return Packet
Size，打开 BTA、设置 RX VC，再读取 RX FIFO。PLL、command FIFO、write FIFO、
read busy 和 read FIFO 均在超时后返回 `-ETIMEDOUT`，每次轮询调用
`nxsig_usleep(100)` 让出 CPU。是否进入 BTA/read 路径由 DSI packet data type
决定，而不是由写消息中无协议语义的接收字段决定。

已完成一次 M1 实板测试：`dsi_probe` 在面板 reset 后完成 generic short/long 与
EK79007 初始化写序列。`DCS GET_POWER_MODE` 请求完成但未收到 payload，故其为
可选诊断，不作为 command-write 通过标准。重复十次 initialize/shutdown、DSI
error IRQ/首错记录仍待实现。

`attach`/`detach` 负责 DSI device 的 VC、lane、format 约束；`transfer` 负责
packet 生命周期和超时。一次 transfer 的 buffer 在函数返回前必须完成使用，或由
Host 明确复制，禁止异步持有调用方临时内存。

### 5.2 并发与中断

- 已实现：Host 初始化、attach、transfer 与 shutdown 共用互斥锁；配置切换期间
  不得并发发送 DCS。DCS 命令默认串行，首期不追求多请求吞吐。
- 已实现：线程上下文完成超时判定和 errno 转换；等待期间以短暂 sleep 让出 CPU。
- 待实现：DSI error IRQ 必须只记录状态并投递 work，禁止在 ISR 分配内存、打印
  大量日志、等待锁或调用面板代码。
- 待实现：引入 `FAULT` 后，shutdown 需要停止新请求、等待在途 transfer 结束或
  超时，再释放 IRQ/PHY/LDO。

## 6. M2：视频输出、DMA 与内存

### 6.1 M2a：无 framebuffer 的 Host 内建色条

P4 DSI Host 提供 vertical/horizontal bar 与 BER video pattern generator。M2a 将
video 配置逻辑继续放在 `esp_mipi_dsi.c/.h`，通过
`esp_mipi_dsi_video_pattern_start()` 配置 DPI 时钟、timing、DSI bridge 和
packetizer；P4X 板级层提供 1024×600 timing 与 GPIO26 静态背光。

`dsi_probe video [seconds]` 是唯一测试入口。它不接入 LVGL、framebuffer、DMA
或 `/dev/fb0`，用于把“DSI video Host 配置是否完成”与“内存/DMA/图形栈是否正确”
分开。

此命令返回 `HOST PASS` 仅说明 D-PHY、Host、bridge 和内建 pattern generator 的
寄存器配置已由 CPU 侧接受；**不等价于面板已显示**。M2a 的显示通过必须由操作者在
色条持续期间目视确认，并留存屏幕照片或视频。若屏幕没有色条，即使命令返回零，也
应记录为“Host 配置完成、视觉显示待修复”，不得写作 DPI 显示通过。

为缩小 Host-only 与 ESP-IDF 正常 DPI pipeline 的差异，M2a 使用 RGB888、frame ACK、
LP timing、burst with sync pulses，并在启动后打印 Host/bridge 的 mode、timing、format、
flow-control 和 interrupt 快照。M2a 没有 framebuffer，因此 flow controller 保持
bridge；M2b 接入 GDMA 后才切换到 DMA controller。

### 6.2 M2b：固定 framebuffer、DMA 与内存

DSI DBI/DCS 命令只用于控制面板；1024 x 600 的持续像素输出需要 DPI video
pipeline。为将 M2a 的“Host 已启动但未显示”与物理链路问题区分，当前 M2b 实现采用
**固定 RGB888 垂直色条**，不引入 LVGL。该路径已经证明 GDMA 计数可以递增，
但实板仍黑屏，因而只保留为底层诊断路径，不再作为首选显示验收路径：

1. 板级仍传入完整 video timing；芯片层不写死 EK79007 的分辨率、porch 或 GPIO。
2. 板级通过芯片层公开的 `esp_mipi_dsi_dma_buffer_*()` 请求 64-byte 对齐的
   1024×600 RGB888 PSRAM 单帧（1,843,200 B），填充八段色条后执行 C2M cache
   clean。ESP HAL 的 heap/cache 头文件、capability 位和错误类型只留在芯片层，
   不能泄漏到 Board.mk 编译的板级源码。
3. 芯片层创建一个 DW-GDMA circular LLI，源为该 buffer、目的为
   `MIPI_DSI_BRG_MEM_BASE`；使用 64-bit transfer width、DSI 硬件握手和 DMA flow
   controller，随后才打开 Bridge DPI output 与 Host video mode。
4. 停止顺序是 Host video off -> Bridge output off -> GDMA disable/release -> Bridge/
   DPI clock off；帧缓冲最后由板级释放，避免 DMA 使用已释放内存。
5. 本轮只验证静态 RGB888 scanout。它不注册 framebuffer 设备、不处理 vsync IRQ，
   也不构成 RGB565/LVGL 的最终内存模型；当前视觉结果为未显示。

1024 x 600 RGB565 单缓冲为 1,228,800 B（约 1.17 MiB），双缓冲约 2.34 MiB。
这是一项显示流水线预算，不能从任务栈或普通 small-heap 中零散分配。

### 6.3 M2c：EK79007 DPI Panel 与 `draw_bitmap()`

M2c 不再由板级函数直接创建“色条 DMA”。它将与官方 EK79007 组件同构的对象
生命周期明确拆分：

1. 板级提供固定的 P4X DPI profile：1024×600、RGB565、52 MHz、H 10/160/160、
   V 1/23/12、2 lane/1000 Mbps。
2. `ek79007_panel_setup()` 在传入 DPI config 时创建 caller-owned DPI panel；
   此时尚未分配帧缓冲、未启动 video。
3. `ek79007_panel_initialize()` 先写 EK79007 vendor DCS 序列并 sleep-out，随后
   调用 DPI panel initialize；后者分配 1,228,800 B、64-byte 对齐的 PSRAM RGB565
   帧缓冲，进行 cache clean，配置 GDMA/bridge 并启动连续 scanout。
4. Probe 用 `ek79007_panel_draw_bitmap()` 提交整帧八段 RGB565 色条，然后发送
   display-on 并打开 GPIO26 背光。
5. shutdown 严格按“背光关 -> display-off -> video/GDMA 停止 -> 释放帧缓冲”执行。

该对象当前只支持单帧、整屏 `draw_bitmap()`，不提供 `/dev/fb0`、局部刷新、双缓冲、
vsync callback 或 LVGL flush。它的目的是先将成功 ESP-IDF 示例的
“DPI Panel + draw_bitmap”视频路径搬入 NuttX 分层，而不是提前建立通用图形子系统。

## 7. Kconfig 与构建接入

Kconfig 应表达硬件能力，而不是把某个面板参数提升为全芯片默认值。建议分层：

```text
CONFIG_ESPRESSIF_LDO                 # M1 已实现；由 DSI Host 自动选择
CONFIG_ESPRESSIF_MIPI_DSI            # M1 已实现，默认关闭
CONFIG_ESPRESSIF_MIPI_DSI_TIMEOUT_MS # M1 已实现，默认 100，范围 1--1000 ms
CONFIG_ESPRESSIF_MIPI_DSI_VIDEO      # M2a/M2b：DPI video 基础
CONFIG_ESPRESSIF_MIPI_DSI_VIDEO_DMA  # M2b：依赖 SPIRAM 的 GDMA scanout
CONFIG_ESPRESSIF_MIPI_DSI_DPI_PANEL  # M2c：DPI panel 对象与 draw_bitmap 生命周期
CONFIG_LCD_EK79007                   # 通用面板
CONFIG_INPUT_GT911                   # 通用触摸
```

`lane_num`、lane bit rate、video timing、framebuffer 数量与格式属于 P4X 板级
Kconfig 或板级静态配置。所有新增 C 源必须同时更新对应 `Kconfig`、`Make.defs`
和 `CMakeLists.txt`；P4 vendor HAL 增量源也必须在 `hal_esp32p4.mk` 与
`hal_esp32p4.cmake` 保持一致。

建议的首批文件边界：

| 文件 | M1/M2 | 职责 |
| --- | --- | --- |
| `chips/esp32p4/common/espressif/esp_ldo.c/.h` | M1，已实现 | LDO 生命周期、errno 转换 |
| `chips/esp32p4/common/espressif/esp_mipi_dsi.c/.h` | M1/M2a/M2b | Host、PHY、DCS transfer、DPI pattern、私有 GDMA/LLI 生命周期与 DMA scanout API |
| `chips/esp32p4/common/espressif/esp_mipi_dsi_dpi_panel.c/.h` | M2c | P4 DPI panel 对象、PSRAM 单帧分配、整帧 `draw_bitmap()` 与 scanout 生命周期 |
| `chips/esp32p4/common/espressif/{Kconfig,Make.defs,CMakeLists.txt}` | M1/M2，M1 已实现 | 芯片层开关和构建 |
| `chips/esp32p4/hal_esp32p4.{mk,cmake}` | M1/M2，已实现 | 条件纳入 vendor DSI HAL 源。M2b 不编入 ESP-IDF 的 `upper_hal_dma/src/dw_gdma.c`，因为其依赖 FreeRTOS；由 `esp_mipi_dsi.c` 直接使用已纳入构建的 DW-GDMA HAL/LL 完成专用 scanout。 |
| `board/.../src/esp32p4_lcd.c` | M1/M2a/M2b/M2c | P4X D-PHY LDO、GPIO27 reset、GPIO26 静态背光和 RGB565 DPI profile；旧 RGB888 色条仅作诊断回退。 |
| `drivers/nuttx/drivers/lcd/ek79007.c/.h` | M2c | EK79007 DCS 与可选 DPI panel 生命周期衔接；无 DPI Kconfig 时仍保持 command-only 可构建。 |
| `app/dsi_probe/`、`configs/dsi_probe/defconfig` | M1/M2/M2c | 与 LVGL 解耦的 command Host 与 RGB565 `draw_bitmap()` 色条验证入口；video 模式主动跳过不可靠的 DCS read。 |

## 8. 验收矩阵

| 阶段 | 最小测试 | 通过标准 |
| --- | --- | --- |
| M1 编译 | `dsi_probe` 构建，Make 入口 | 已通过：无 DSI 新增编译/链接告警；CMake 双入口仍待回归 |
| M1 启动 | USB console 打印 Host 状态 | 已通过一次：LDO、P4 rev3 clock source、PLL lock、lane stop 与 Host ready 全部成功 |
| M1 DCS 写 | generic short/long 与 EK79007 初始化写序列 | 已通过一次：不死锁，所有命令由 Host 接受；重复十次 initialize/shutdown 待验收 |
| M1 DCS 读 | `GET_POWER_MODE` BTA/RX FIFO | 已诊断：Host 完成读请求但面板未返回 payload；非 M1 command-write 阻塞项 |
| M2a Host 配置 | `dsi_probe video 60` 内建垂直色条 | 日志为 `HOST PASS`，且无 Host/bridge timeout；这不是显示通过 |
| M2a 显示 | `dsi_probe video 60` 内建垂直色条 | 1024 x 600 稳定，能清晰区分色条，无花屏或 Host/bridge timeout，并留存屏幕照片/视频 |
| M2b DMA 启动 | `dsi_probe video 60` | 日志包含 `DMA colour bars ready`、`DPI DMA started` 和 `dma-started` 寄存器快照；无 DMA 分配、cache 或 GDMA 建链错误。 |
| M2b 显示 | `dsi_probe video 60` | 肉眼可见八段 RGB 色条并留存照片/视频；未看到画面时只能记录为“DMA scanout 软件已启动、面板链路待排查”。 |
| M2c 构建 | `dsi_probe` Make/CMake | 新增 DPI panel Kconfig、Make/CMake、EK79007 生命周期均可链接；关闭 video Kconfig 后 command-only 路径仍可链接。 |
| M2c 显示 | `dsi_probe video 60` | 日志为 RGB565 / `draw_bitmap()` / 1,228,800 B，且肉眼可见八段色条；否则记录为“DPI panel 已启动、视觉显示待修复”。 |
| M3 framebuffer | RGB565 framebuffer 色条/纯色 | 1024 x 600 稳定，无撕裂、花屏或 DMA abort |
| M2 压力 | 背光、sleep/wake、重启、连续刷新 | 无资源泄漏，异常后可从 `FAULT` 完整恢复 |

每次验收至少留存构建命令、`git diff --check`、USB console 日志和屏幕照片/视频。

## 9. 实施顺序与提交粒度

```text
feat(esp32p4): 增加 LDO 与 MIPI-DSI 命令 Host 基础
build(esp32p4): 接入 MIPI-DSI vendor HAL 与双构建入口
test(esp32p4x): 新增 dsi_probe 配置与命令 Host 实板证据
feat(esp32p4): 增加 MIPI-DSI DPI 内建色条验证
feat(esp32p4): 增加 MIPI-DSI framebuffer 与 DMA 管理
feat(lcd): 接入 EK79007 面板与 P4X 显示装配
feat(esp32p4): 增加 DPI panel 和 draw_bitmap 色条验证闭环
config(esp32p4x): 新增 LVGL 显示与触摸验证配置
```

每个提交只跨越一个层次。未完成 M1 时不提交声称可显示的 LVGL 配置；未确认
DMA 内存属性时不将 PSRAM framebuffer 作为默认事实。

## 10. 风险清单

| 风险 | 控制措施 |
| --- | --- |
| 面板/adapter 并非 1/2 lane 兼容 | P0 核对面板资料和原理图；P4 Host 不支持 4 lane 时不靠软件绕过。 |
| D-PHY 供电通道选择错误 | 用原理图确认实际 LDO 通道和电压；启动日志记录 LDO acquire/enable 结果。 |
| Vendor HAL 源未纳入构建 | 先做 M1 最小链接验证；mk/cmake 双入口同时检查。 |
| 将 DCS 控制误认为视频显示 | M1/M2 分离验收，M2 前不注册 `/dev/fb0`。 |
| DMA/PSRAM 不一致或 cache 未同步 | 单缓冲色条验证起步，记录内存区域、对齐和 cache 操作。 |
| DSI 错误 ISR 与面板线程竞态 | ISR 最小化，状态机和互斥锁只在任务上下文完成恢复。 |
