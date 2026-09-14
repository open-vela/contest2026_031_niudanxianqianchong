# ESP32-P4X LCD 测试调用全链路

## 1. 文档目的与范围

本文档说明当前 ESP32-P4X-Function-EV-Board 上执行以下命令时的
LCD 完整调用链路：

```text
nsh> dsi_probe video 10
```

该测试用于验证 EK79007、MIPI-DSI DPI video、PSRAM framebuffer、
DW-GDMA 和 P4 DSI Bridge 的最小显示通路。它不启动 LVGL，也尚未注册
`/dev/lcdN` 或 `/dev/fbN`。

当前实板状态为：`dsi_probe pattern 10` 与 `dsi_probe video 10` 均已在
同一块 P4X、EK79007 面板和 FPC 上显示色条。本文保留此前的诊断边界，便于
后续 framebuffer/LVGL 回归时区分 Host 路径和视觉结果；黑屏根因及修复见
[P4X DSI 黑屏 DBI 配置排障闭环](../../开发日志/编译/2026-08-24-ESP32-P4X-DSI黑屏DBI配置排障闭环.md)。

显示验收仍区分以下两个层次：

- **Host 路径完成**：函数返回成功、DMA 帧计数增长、未捕获错误位；
- **视觉显示通过**：屏幕真实显示 1024 x 600 RGB565 八段色条。

只有第二项才能记录为 LCD 显示 PASS。

## 2. 分层与主调用链

```text
NSH: dsi_probe video 10
  |
  v
app/dsi_probe/dsi_probe_main.c                 测试编排、色条生成、时长控制
  |
  +--> board/.../src/esp32p4_lcd.c             P4X 板级参数、RESET、背光、Host 装配
  |
  +--> drivers/nuttx/drivers/lcd/ek79007.c      EK79007 DCS 命令和面板生命周期
  |
  +--> NuttX drivers/video/mipidsi              通用 MIPI-DSI device/packet 框架
  |
  +--> esp_mipi_dsi_dpi_panel.c                 DPI Panel 对象、持久 framebuffer、draw_bitmap
  |
  +--> esp_mipi_dsi.c                           LDO/PHY/Host/Bridge/DW-GDMA 芯片适配
  |
  v
PSRAM framebuffer -> DW-GDMA -> DSI Bridge -> DSI Host -> D-PHY -> EK79007 -> LCD
```

各层的责任边界如下：

| 层次 | 关键文件 | 职责 |
| --- | --- | --- |
| 应用层 | `app/dsi_probe/dsi_probe_main.c` | 解析命令、编排面板生命周期、生成色条、打印诊断状态。 |
| 板级层 | `board/.../src/esp32p4_lcd.c` | 提供 P4X 的 lane、时钟、timing、GPIO27 reset 和 GPIO26 背光配置。 |
| 面板层 | `drivers/nuttx/drivers/lcd/ek79007.c` | 封装 EK79007 厂商 DCS 序列及 setup/initialize/draw/shutdown 语义。 |
| NuttX DSI 通用层 | `nuttx/drivers/video/mipidsi/` | 组建 DSI packet，通过 `host->ops` 将 attach/transfer 转发给芯片层。 |
| DPI Panel 层 | `esp_mipi_dsi_dpi_panel.c` | 管理扫描缓冲区、启动 DMA video、提供 `draw_bitmap()`。 |
| P4 芯片层 | `esp_mipi_dsi.c` | 配置 D-PHY、Host、Bridge、DPI 时序和 DW-GDMA，处理帧完成中断。 |

## 3. 阶段 A：命令入口与参数准备

NSH 启动 `dsi_probe` 应用后进入 `main()`。`dsi_probe_parse_video_request()`
将 `video 10` 解析为：

```text
video_requested = true
video_seconds   = 10
```

应用层面板配置为：

```text
virtual channel = 0
data lanes      = 2
HS rate         = 1,000,000,000 bit/s/lane
LP rate         = 10,000,000 bit/s
pixel format    = RGB565
command mode    = LPM
```

视频模式下，应用将一个 caller-owned `esp_mipi_dsi_dpi_panel_s`
对象和板级 DPI 配置交给 EK79007 驱动。

## 4. 阶段 B：DSI Host 与 D-PHY 初始化

调用链：

```text
main()
  -> board_mipi_dsi_initialize(&host)
       -> board_mipi_dsi_backlight_set(false)
       -> esp_mipi_dsi_host_initialize(&g_board_mipi_dsi_config, &host)
```

板级 Host 配置为：

```text
DSI bus             = 0
data lanes          = 2
lane bit rate       = 1000 Mbps/lane
PHY reference clock = 40 MHz
D-PHY LDO           = channel 3 / 2.5 V
```

`esp_mipi_dsi_host_initialize()` 内部执行：

```text
esp_ldo_acquire()
  -> 打开 DSI Host / Bridge / PHY 时钟
  -> esp_mipi_dsi_enable_phy_clock_sources()
  -> mipi_dsi_hal_init()
  -> mipi_dsi_hal_configure_phy_pll()
  -> esp_mipi_dsi_wait_pll()
  -> esp_mipi_dsi_wait_lanes_stopped()
  -> esp_mipi_dsi_configure_command_mode()
  -> mipi_dsi_host_register()
```

完成后得到标准 NuttX `struct mipi_dsi_host *`。日志中的 `LDO ready`、
`PHY PLL configured` 和 `Host ready` 对应此阶段。

## 5. 阶段 C：创建面板对象并挂接 DSI device

调用链：

```text
ek79007_panel_setup(&panel, &device, &panel_config)
  -> 保存 lanes / format / rates / mode flags
  -> esp_mipi_dsi_dpi_panel_create(&dpi_panel, host, &dpi_config)

mipi_dsi_attach(&device)
  -> device->host->ops->attach()
  -> esp_mipi_dsi_attach()
```

`esp_mipi_dsi_dpi_panel_create()` 只验证参数、拷贝 timing 并计算帧缓冲区
大小；此时尚未分配 PSRAM，也没有启动 DMA。

当前 RGB565 帧大小为：

```text
1024 * 600 * 2 = 1,228,800 bytes
```

## 6. 阶段 D：LCD 硬件复位

调用：

```text
board_mipi_dsi_panel_reset()
```

复位时序：

```text
GPIO27 配置为输出
  -> GPIO27 拉低 10 ms
  -> GPIO27 拉高
  -> 等待 20 ms
```

GPIO27 对应 LCD adapter 的 `RST_LCD`，低电平有效。该复位是板级硬件
操作，不是 EK79007 DCS software reset。

## 7. 阶段 E：EK79007 DCS 命令通路

`ek79007_panel_initialize()` 默认发送：

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

每条 DCS 写命令通过如下链路发送：

```text
ek79007_write()
  -> mipi_dsi_dcs_write()
  -> mipi_dsi_dcs_write_buffer()
  -> mipi_dsi_transfer()
  -> host->ops->transfer()
  -> esp_mipi_dsi_transfer()
  -> mipi_dsi_create_packet()
  -> esp_mipi_dsi_write_packet()
  -> DSI Host command FIFO
  -> D-PHY
  -> EK79007
```

`dsi_probe video` 为了贴合已点亮的 ESP-IDF 参考工程，特意跳过：

- command-only probe 使用的 generic short/long packet；
- DCS `0x29` Display ON；
- DPI 活动期间的 DCS Power Mode 读取。

## 8. 阶段 F：DPI Panel 和持久 framebuffer

EK79007 DCS 初始化后，`ek79007_panel_initialize()` 继续调用：

```text
esp_mipi_dsi_dpi_panel_initialize()
  -> esp_mipi_dsi_dma_buffer_allocate()
  -> esp_mipi_dsi_dma_buffer_sync_for_device()
  -> esp_mipi_dsi_video_dma_start()
```

`esp_mipi_dsi_dma_buffer_allocate()` 使用 64-byte aligned `memalign()` 分配
1,228,800-byte PSRAM framebuffer，然后清零。

`esp_mipi_dsi_dma_buffer_sync_for_device()` 调用 `esp_cache_msync()`，将 CPU
cache 中的 framebuffer 数据同步到 DMA 可见内存：

```text
ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED
```

## 9. 阶段 G：DPI、Bridge 和 DW-GDMA 启动

核心入口为：

```text
esp_mipi_dsi_video_dma_start(host, &config)
```

执行顺序：

```text
esp_mipi_dsi_enable_dpi_clock()
  -> 打开 Bridge 寄存时钟和参考时钟
  -> esp_mipi_dsi_video_configure_host()
  -> esp_mipi_dsi_video_configure_bridge()
  -> 配置 Bridge multi-block / burst / empty threshold
  -> esp_mipi_dsi_video_dma_prepare()
  -> esp_mipi_dsi_bridge_interrupt_prepare()
  -> enable Bridge
  -> enable DW-GDMA channel
  -> enable DSI Host video mode
  -> enable Bridge DPI output
  -> enable Bridge underrun interrupt
```

### 9.1 DPI timing

| 参数 | 当前值 |
| --- | ---: |
| 有效分辨率 | 1024 x 600 |
| HSync / HBackPorch / HFrontPorch | 10 / 160 / 160 |
| VSync / VBackPorch / VFrontPorch | 1 / 23 / 12 |
| 请求像素时钟 | 52 MHz |
| 当前日志的实际时钟 | 48 MHz（240 MHz / 5） |
| Host/Bridge 输入输出格式 | RGB565 -> RGB565 |

### 9.2 DW-GDMA 数据方向

```text
source role       = memory
destination role  = DSI peripheral
source address    = PSRAM framebuffer
destination       = MIPI_DSI_BRG_MEM_BASE
source mode       = increment
destination mode  = fixed
transfer width    = 64 bit
source burst      = 512 items
destination burst = 256 items
LLI               = one terminal descriptor
```

启动的关键先后顺序为：

```text
dw_gdma_ll_channel_enable()
  -> mipi_dsi_host_ll_enable_video_mode()
  -> mipi_dsi_brg_ll_enable_dpi_output()
  -> mipi_dsi_brg_ll_update_dpi_config()
```

## 10. 阶段 H：色条生成与 draw_bitmap

`dsi_probe_run_video_pattern()` 执行：

```text
esp_mipi_dsi_dpi_panel_get_frame_buffer()
  -> board_mipi_dsi_backlight_set(true)
  -> dsi_probe_fill_rgb565_colour_bars()
  -> ek79007_panel_draw_bitmap()
  -> esp_mipi_dsi_dpi_panel_draw_bitmap()
  -> memmove()
  -> esp_mipi_dsi_dma_buffer_sync_for_device()
```

色条顺序为：

```text
白 -> 黄 -> 青 -> 绿 -> 品红 -> 红 -> 蓝 -> 黑
```

测试应用先取得 DPI Panel 内部的持久 framebuffer，再直接在这块内存
中生成色条。因此将同一地址传给 `draw_bitmap()` 时，内部 `memmove()`
实际上是同地址复制；此时真正必要的操作是随后的 cache C2M 同步。

## 11. 阶段 I：持续扫描与 DMA 中断

DMA 完成一帧后进入：

```text
esp_mipi_dsi_dma_interrupt()
  -> 读取并清除 DMA interrupt status
  -> 若存在 error event，累计 dma_error_events
  -> 若一帧完成，重新设置 LLI block markers
  -> 同步 LLI cache
  -> 重写 link-list head
  -> 重新使能 DMA channel
  -> dma_frame_count++
```

因此日志中的 `frames=61` 和 `frames=610` 证明 DMA completion ISR
持续运行。它们不能单独证明 Bridge 已经生成正确的 DSI video packet，
也不能证明 EK79007 已经显示这些像素。

最终像素通路为：

```text
CPU 生成 RGB565 色条
  -> PSRAM framebuffer
  -> Cache C2M
  -> DW-GDMA
  -> MIPI_DSI_BRG_MEM_BASE
  -> MIPI-DSI Bridge
  -> DPI video packet
  -> DSI Host
  -> D-PHY clock lane + 2 data lanes
  -> EK79007
  -> 1024 x 600 LCD 像素阵列
```

## 12. 阶段 J：停止与资源释放

10 秒结束后，调用链为：

```text
dsi_probe_panel_shutdown()
  -> board_mipi_dsi_backlight_set(false)
  -> ek79007_panel_shutdown()
       -> esp_mipi_dsi_dpi_panel_stop()
            -> esp_mipi_dsi_video_stop()
                 -> disable Host video mode
                 -> disable Bridge DPI output
                 -> stop/release DW-GDMA
       -> esp_mipi_dsi_dpi_panel_destroy()
            -> free framebuffer
  -> mipi_dsi_detach()
  -> board_mipi_dsi_shutdown()
       -> esp_mipi_dsi_host_shutdown()
            -> deinit DSI HAL
            -> disable PHY/Host clocks
            -> release LDO3
```

## 13. command-only 测试与 video 测试的区别

| 项目 | `dsi_probe` | `dsi_probe video 10` |
| --- | --- | --- |
| 目标 | 验证 command Host 和 DCS packet | 验证 EK79007 + DPI + framebuffer + DMA 视频链路 |
| generic packet | 发送 short/long packet | 跳过 |
| EK79007 初始化 | 执行 | 执行 |
| DPI Panel | 不创建 | 创建并初始化 |
| framebuffer | 无 | 1,228,800-byte RGB565 PSRAM buffer |
| DW-GDMA | 不启动 | 启动并循环扫描 |
| DCS 0x29 | 发送 | 跳过，贴合 ESP-IDF EK79007 参考路径 |
| DCS Power Mode 读 | 尝试，超时可降级 | 跳过 |
| 最终验收 | Host command PASS | 肉眼可见色条 |

## 14. 当前实际使用路径与未使用路径

`dsi_probe video 10` 当前实际使用：

```text
ek79007_panel_initialize()
  -> esp_mipi_dsi_dpi_panel_initialize()
  -> esp_mipi_dsi_video_dma_start()
  -> ek79007_panel_draw_bitmap()
```

它不调用板级早期兼容测试入口：

```text
board_mipi_dsi_video_pattern_start()
```

后者保留 RGB888 framebuffer/Host pattern 分支，不属于当前 RGB565
EK79007 `draw_bitmap()` 测试链路。分析当前串口日志时不应将两条路径
混合。

## 15. 历史黑屏的证据边界与已关闭结论

| 日志或现象 | 可以证明 | 不能证明 |
| --- | --- | --- |
| `PHY PLL configured` / `Host ready` | D-PHY PLL 和 Host 基本启动 | DPI timing 和面板解析正确 |
| EK79007 initialization accepted | Host 完成 DCS 写 packet | 面板确实接收并应用每条命令 |
| framebuffer 分配成功 | PSRAM 容量足够 | 该地址对 DW-GDMA 的可访问属性完全正确 |
| `frames > 0` | DMA completion 中断和软件重启逻辑运行 | 像素已经正确到达面板 |
| DMA errors = 0 | 已检查的 DMA 错误位没有置位 | Host/Bridge/PHY 每一个隐含参数均正确 |
| Bridge underrun = 0 | Bridge 未报告已启用的 underrun 事件 | Bridge 一定产生了可显示的 video packet |
| 背光亮 | GPIO26 和 adapter 背光路径可用 | DSI 像素链路正常 |

修复前，问题曾收敛在：

```text
DW-GDMA 描述符/握手与 PSRAM DMA 可见性
  -> DSI Bridge 实际像素流
  -> Host video packet 参数
  -> D-PHY 视频输出
  -> EK79007 对 video stream 的解析
```

后续通过与已点亮 ESP-IDF 工程对齐 DBI IO 创建配置，发现并修复了 Generic、
DCS 与 MRPS 命令默认走 HS 的问题；现在两条显示路径均已视觉通过。该表格用于
解释为何当时应优先排查共同的面板命令链路，而不是继续随机修改 EK79007 DCS 命令、
lane 数或色条内容。

### 15.1 D-PHY 高频状态采样

单次读取 `phy_status` 只能说明读取瞬间 lane 是否处于 LP11，不能据此
断言整个视频周期是否存在高速活动。Probe 因此在以下两个时点调用：

```text
board_mipi_dsi_video_sample_phy_status()
  -> esp_mipi_dsi_video_phy_sample_status()
  -> 连续读取 Host phy_status 2000 次
```

采样间隔为 50 us，每个窗口名义持续 99,950 us，覆盖多个 60 Hz 帧周期，
且不会强制切换 PHY 状态。日志示例：

```text
INFO: MIPI-DSI PHY sample stage=probe-start samples=2000 interval_us=50 ...
INFO: MIPI-DSI PHY activity stage=probe-start clk=... d0=... d1=... all=... any=...
```

结果按以下边界解释：

| 结果 | 判断 |
| --- | --- |
| `any=0`、`transitions=0`、`lock_lost=0` | PLL 保持锁定，但窗口内所有 lane 始终为 LP11；这是“视频没有离开 PHY stop state”的强证据。 |
| `any>0` 且 `transitions>0` | lane 确实周期性离开 LP11；应将病灶继续收敛到 DSI packet、video timing 或面板解析。 |
| `lock_lost>0` | 采样期间发生 PLL lock 丢失，优先检查 D-PHY 时钟、LDO 和 PLL 参数。 |

`clk/d0/d1` 的千分比是采样命中率，不等于协议带宽利用率；stop-state
清零也不等价于已经解析出有效 HS video packet。若需要最终物理层定论，
仍应测量 clock lane 和 data lane 的差分波形。

## 16. 测试与验收

固定色条回归建议依次执行：

```text
nsh> dsi_probe
nsh> dsi_probe video 10
nsh> dsi_probe video 60
```

记录：

- 固件 commit 和 defconfig；
- 屏幕、FPC、adapter 供电和 GPIO27/GPIO26 接线；
- LDO、PLL、Host 启动日志；
- framebuffer 地址、大小和像素格式；
- `dma-started`、`probe-after-1s` 和 `probe-before-stop` 快照；
- DMA error、Bridge underrun 和帧计数；
- 视觉结果及照片/视频文件名。

当前固定色条阶段的 PASS 标准：

```text
1024 x 600 面板持续显示八段 RGB565 色条，
连续 10 次启动无黑屏、花屏、DMA fault 或 Bridge underrun。
```

## 17. 关联文档

- [ESP32-P4 MIPI-DSI Host 设计与实施方案](ESP32-P4-MIPI-DSI-Host设计与实施方案.md)；
- [ESP32-P4X ESP-IDF LCD 参考实现对比与收敛计划](ESP32-P4X-ESP-IDF-LCD参考实现对比与收敛计划.md)；
- [ESP32-P4X LVGL 显示与触摸适配计划](ESP32-P4X-LVGL显示与触摸适配计划.md)。
