# ESP32-P4X：ESP-IDF LCD 参考实现对比与收敛计划

## 1. 范围、证据与结论

本文将当前 OpenVela/NuttX 的 `dsi_probe` 显示链路，与同一块
ESP32-P4X-Function-EV-Board、同一块 1024 x 600 EK79007 面板、同一条 FPC 上
**已经实测显示色条**的 ESP-IDF 工程进行对照：

```text
参考工程：~/Project/espidf_lcd（提交 1f057d1）
当前工程：contest2026_031_niudanxianqianchong
```

当前结论必须分为两层：

1. OpenVela 的 D-PHY、MIPI-DSI Host、EK79007 DCS 写入、RGB565 PSRAM
   framebuffer、GDMA 描述符和 Bridge 已在实板执行；DMA 帧计数可从 0 增长到
   约 61 frame/s，当前快照没有记录 GDMA error 或 Bridge underrun。
2. 面板只有背光、没有色条。因此状态只能记为**“Host/DMA 软件路径通过，视觉
   显示未通过”**。`frames > 0` 不能证明像素已经以正确格式、正确时序送达面板。

ESP-IDF 示例可见色条，已经排除“面板、FPC、adapter 主供电或背光硬件必然损坏”
这类基础假设。后续应收敛到 DPI panel、GDMA 启动顺序和 Host/Bridge 寄存器差异，
不得随机调整 lane 数、像素格式或 LVGL 配置。

## 2. 对照对象

| 层次 | ESP-IDF 参考实现 | 当前 OpenVela 实现 |
| --- | --- | --- |
| 应用入口 | `~/Project/espidf_lcd/main/main.c` | `app/dsi_probe/dsi_probe_main.c` |
| 板级装配 | `components/esp32_p4_function_ev_board/esp32_p4_function_ev_board.c` | `board/esp32p4/esp32p4-function-ev-board/src/esp32p4_lcd.c` |
| EK79007 | ESP-IoT-Solution `esp_lcd_ek79007` | `drivers/nuttx/drivers/lcd/ek79007.c` |
| DSI/DPI | `esp_lcd_new_dsi_bus()`、`esp_lcd_new_panel_dpi()` | `esp_mipi_dsi.c`、`esp_mipi_dsi_dpi_panel.c` |
| 图像提交 | `esp_lcd_panel_draw_bitmap()` | `ek79007_panel_draw_bitmap()` |
| 实板结果 | RGB565 色条可见 | 背光可见，RGB565 色条不可见 |

共同硬件基线为：P4 revision 3.x、DSI bus 0、2 data lane、1000 Mbps/lane、
D-PHY LDO channel 3/2.5 V、GPIO27 低有效复位、GPIO26 背光，以及 EK79007
1024 x 600 RGB565 profile。因此 2 lane/1000 Mbps 是本次对照的已知工作基线，
不能因为黑屏直接切换到 4 lane。

## 3. 已显示的 ESP-IDF 完整视频链路

```text
app_main()
  ├─ bsp_display_new()
  │   ├─ brightness init + D-PHY LDO3/2.5 V
  │   ├─ esp_lcd_new_dsi_bus()                 # bus0, 2 lane, 1000 Mbps, XTAL
  │   ├─ esp_lcd_new_panel_io_dbi()            # VC0, 8-bit DCS command/parameter
  │   ├─ esp_lcd_new_panel_ek79007()
  │   │   └─ esp_lcd_new_panel_dpi()           # framebuffer、Bridge、GDMA descriptor
  │   ├─ esp_lcd_panel_reset()
  │   └─ esp_lcd_panel_init()                  # EK79007 DCS + DPI panel init
  ├─ bsp_display_brightness_set(100)
  ├─ PSRAM 分配 1024 * 600 * 2 B RGB565 临时图像
  ├─ 填充色条
  └─ esp_lcd_panel_draw_bitmap(panel, 0, 0, 1024, 600, frame)
```

参考实现的关键语义在 `esp_lcd_panel_dpi.c`：创建持久 framebuffer、建立 GDMA
link-list 后，**先 enable GDMA channel**，再 enable Host video mode 与 Bridge
DPI output，最后通过 `draw_bitmap()` 更新扫描缓冲区。这是可显示工程的行为基准。

官方实现参考：

- [ESP-IDF DSI LCD 文档](https://docs.espressif.com/projects/esp-idf/en/latest/esp32p4/api-reference/peripherals/lcd/dsi_lcd.html)
- [ESP-IDF `esp_lcd_panel_dpi.c`](https://github.com/espressif/esp-idf/blob/master/components/esp_lcd/dsi/esp_lcd_panel_dpi.c)
- [ESP-IoT-Solution EK79007 组件](https://github.com/espressif/esp-iot-solution/tree/master/components/display/lcd/esp_lcd_ek79007)

## 4. 当前 OpenVela 调用链

当前 `dsi_probe video 10` 已经不是早期的寄存器写入试验，而是实际调用项目内的
EK79007 驱动和 DPI panel 对象：

```text
dsi_probe_main()
  ├─ board_mipi_dsi_host_initialize()
  │   └─ esp_mipi_dsi_host_initialize()        # LDO、PHY PLL、lane stop、NuttX Host
  ├─ board_mipi_dsi_panel_reset()               # GPIO27: low 20 ms, high 120 ms
  ├─ ek79007_panel_create() / initialize()
  │   ├─ EK79007 DCS initialization writes
  │   └─ esp_mipi_dsi_dpi_panel_initialize()
  │       ├─ 申请 RGB565 1,228,800 B PSRAM framebuffer
  │       └─ esp_mipi_dsi_video_dma_start()
  ├─ esp_mipi_dsi_dpi_panel_get_frame_buffer()
  ├─ 填充八段 RGB565 色条
  ├─ ek79007_panel_draw_bitmap()
  ├─ ek79007_panel_set_display(true)
  └─ board_mipi_dsi_backlight_set(true)         # GPIO26
```

因此日志中的 `EK79007 panel driver initialisation accepted` 表示
`drivers/lcd/ek79007.c` 已参与运行；`DMA PSRAM buffer allocated bytes=1228800`
表示单帧内存申请成功；`frames=61` / `frames=613` 仅表示 GDMA completion 软件路径
持续运行，均不能替代视觉验收。

## 5. 逐项差异与判断

| 项目 | ESP-IDF 参考实现 | 当前 OpenVela | 判断 |
| --- | --- | --- | --- |
| D-PHY 基线 | LDO3 2.5 V、XTAL、2 lane、1000 Mbps | 相同；实测 PLL lock、Host ready | 已基本对齐，不是首要变量。 |
| 面板 reset | GPIO27 低有效；默认约低 10 ms、高 20 ms | 低 20 ms、高 120 ms | 更长但合理；记录为 P2 A/B 项，不先认定根因。 |
| DCS 初始化 | EK79007 B2/B3/…、sleep out 后衔接 DPI panel | 项目 EK79007 driver 已发送相应写序列 | 写入被 Host 接受；DCS read 无 payload 是可观测性限制，不是唯一阻塞项。 |
| 像素/缓冲 | RGB565、单帧、1,228,800 B PSRAM | 相同 | 内存大小与格式已对齐，非“PSRAM 不够”。 |
| 时序 | 1024x600；52 MHz；H 10/160/160、V 1/23/12 | 相同目标 profile | 当前实际时钟日志为 48 MHz（240/5，约 55.7 Hz）；需同 ESP-IDF 寄存器实测对照，不能单独定性。 |
| Host/Bridge | VC、RGB565、sync-pulse、H/V timing、multi-block=1、burst=256、empty threshold=768 | 使用 vendor LL 填充同类字段 | 需要寄存器逐位比对。 |
| GDMA 构建 | `dw_gdma` wrapper 建 channel、LLI | 直接使用已纳入的 DW-GDMA HAL/LL | 正确的 NuttX 边界；不可直接引入依赖 FreeRTOS 的 `upper_hal_dma/src/dw_gdma.c`。 |
| **GDMA 启动顺序** | **DMA enable → Host video → Bridge output/update** | **Host video/Bridge output → DMA enable** | **P0 差异，必须先对齐。** |
| Bridge 观测 | 开启 underrun interrupt | GDMA 中断 + 轮询快照，尚无等价 Bridge ISR | **P0 差异，增加最小错误锁存。** |
| 图像 API | 临时色条复制入 DPI 持久扫描缓冲 | 色条写入内部 framebuffer 并 cache clean | 已具备等价最小入口，不需要先接 LVGL。 |
| 视觉结果 | 可见色条 | 背光亮、黑屏 | 唯一的最终失败点。 |

## 6. 当前日志应如何解释

| 证据 | 可以证明 | 不能证明 |
| --- | --- | --- |
| `PHY PLL configured`、`Host ready` | D-PHY/Host 基本启动 | 面板 video timing 正确。 |
| generic short/long accepted | command 写入 FIFO 可用 | 面板已接收或显示像素。 |
| EK79007 initialization accepted | 初始化写命令从 Host 发出 | 每个面板寄存器状态正确。 |
| 1.2 MiB PSRAM 分配成功 | framebuffer 可分配 | DMA 已正确扫描所有行。 |
| `frames=613`、errors=0 | GDMA completion 事件持续工作 | 像素格式、DMA handshake、Bridge 输出和面板显示正确。 |
| 背光亮 | GPIO26 / adapter 背光可用 | MIPI 数据链路正常。 |

## 7. 收敛实施顺序

### P0：先对齐启动顺序和错误中断（代码已实现，待实板复测）

修改 `esp_mipi_dsi_video_dma_start()`，严格按参考实现排序：

```text
1. 建立并绑定 GDMA descriptor/link-list
2. enable GDMA channel
3. enable Host video mode
4. enable Bridge DPI output，并 update DPI config
5. enable Bridge underrun interrupt
```

新增最小 Bridge underrun ISR：ISR 只锁存原始 status、帧计数和一次错误；
停止/恢复放在任务上下文，不能在 ISR 中调用面板或 LVGL。

当前代码已采用“Bridge ISR 锁存 + LPWORK 打印首个 underrun”的实现，避免在中断
上下文直接使用 `syslog()`；待用下述命令确认新的启动顺序是否改变视觉结果，并确认
日志中没有 `ERROR: MIPI-DSI Bridge first underrun`。

**验收门：** `dsi_probe video 10` 连续十次无 DMA error/underrun；若仍黑屏，必须
输出足以与 ESP-IDF 逐寄存器比对的状态快照。

### P1：同相位寄存器对照

在 ESP-IDF 成功工程和 OpenVela 的 `dma-started`、启动后 1 秒两个时点采集：

- Host video mode、color coding、packet size、H/V timing；
- Bridge enable、DPI config、flow、pixel FIFO 和原始中断状态；
- GDMA channel config、LLI、source/destination、block size、握手选择和事件；
- DPI 时钟源、divider、实际频率。

逐个不同位记录来源、含义和是否修改。没有比对证据时，不修改 lane 数和面板 timing。

### P2：按 DPI panel 语义补足 NuttX 适配

若 P0/P1 后仍黑屏，只在 custom chip overlay 内继续完善：复核 LLI owner/EOF、
burst/outstanding、Bridge DMA flow、descriptor 回绕、cache clean 范围和 PSRAM DMA
可见性。吸收 ESP-IDF 的硬件配置语义，但不把 FreeRTOS 依赖代码复制进 NuttX。

**通过标准：** `dsi_probe video 60` 肉眼持续显示八段 RGB565 色条并留存照片/视频，
连续十次启动无黑屏、花屏、DMA abort 或资源泄漏。

### P3：色条通过后再接 LVGL

在色条视觉通过前，不新增 `/dev/fb0`、LVGL port、触摸任务或业务 UI。色条通过后
再依次注册 framebuffer、接入 GT911 `/dev/inputX`、建立最小 LVGL 配置。

## 8. 测试记录约束

烧录后至少执行：

```text
nsh> dsi_probe
nsh> dsi_probe video 10
nsh> dsi_probe video 60
```

每轮记录固件 commit、屏/FPC 是否与 ESP-IDF 对照相同、LDO/PLL/Host 日志、framebuffer
地址和大小、1 秒及结束前 DMA/Bridge 快照、视觉结果和照片/视频文件名。

显示 PASS 的定义只有：**1024 x 600 面板肉眼持续显示八段 RGB565 色条，连续十次
启动无黑屏或 DMA abort。** `HOST PASS`、`draw_bitmap() submitted` 和 `frames > 0`
都只能表示软件路径通过。

## 9. 文档关系

- [ESP32-P4 MIPI-DSI Host 设计与实施方案](ESP32-P4-MIPI-DSI-Host设计与实施方案.md)：芯片层接口、状态机和边界；
- [ESP32-P4X LVGL 显示与触摸适配计划](ESP32-P4X-LVGL显示与触摸适配计划.md)：显示、触摸和 LVGL 的阶段门；
- 本文：以同硬件可显示的 ESP-IDF 工程为基线的收敛清单。
