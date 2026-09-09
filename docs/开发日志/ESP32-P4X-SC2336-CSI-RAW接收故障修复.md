# ESP32-P4X SC2336 CSI RAW 接收故障修复记录

> 记录日期：2026-09-08
> 范围：ESP32-P4X Function EV Board、SC2336、`csi_probe raw`

## 1. 结论

SC2336 的 RAW8 视频链路已经在真机打通。`csi_probe raw` 成功接收一帧，
`csi_probe raw 10` 连续接收十帧；SCCB、CSI Host、ISP RAW 旁路、CSI Bridge、
GDMA、缓存同步和帧内容检查均通过。

本次不是单一故障，而是接收通路遗漏与缓存同步参数错误依次暴露：

1. 接收侧未完整配置 ISP RAW 旁路，CSI Host 的数据不能按 P4 的硬件通路送入
   CSI Bridge。
2. 首帧启动必须在使能 Bridge 前使能 GDMA Channel 0；只在完成中断中重启 DMA
   无法启动第一帧。
3. DMA 已经完整收帧后，CPU 缓存失效调用把 `DIR_M2C` 与 `UNALIGNED` 组合使用。
   ESP32-P4 的 `esp_cache_msync()` 不接受这个组合，导致应用错误地报告
   `buffer_sync ret=-5`。

最终真机日志的错误计数均为零，且帧缓冲不是全零或恒定值。

## 2. 故障现象与定位过程

### 2.1 初始现象：首帧等待超时

最初执行 `csi_probe raw` 时，SC2336 产品 ID 可读、RAW8 模式表和
`stream on` 写入均成功，CSI 和 GDMA 也完成软件初始化；但等待首帧一秒后返回
`-ETIMEDOUT`。这只能说明没有收到 GDMA 完成通知，不能据此直接断言传感器
没有输出。

P4 的实际数据路径如下，GDMA 的源是 CSI Bridge FIFO，而不是 CSI Lane：

```text
SC2336 CSI-2 RAW8
  -> P4 D-PHY / CSI Host
  -> ISP Tail（CSI RAW 旁路）
  -> CSI Bridge FIFO
  -> GDMA Channel 0
  -> 614400-byte RAM buffer
```

因此增加了 `csi_probe rawdiag`，在不改变运行中通路的前提下记录 ISP 事件、
Bridge FIFO 和 DMA 目标地址进度，以区分故障位置。

### 2.2 `rawdiag` 证明接收和 DMA 已完成

修正 ISP 旁路、Bridge 和首帧 DMA 启动后，诊断命令得到以下关键结果：

```text
diag: result=0 samples=2 hs_samples=0 non_stop_samples=0
      isp_events=0x18000000 header_seen=1 tail_seen=1
      bridge_fifo_max=384 dma_dst_advance_max=323520
csi_probe: FAIL step=buffer_sync ret=-5
```

这些字段的含义如下：

| 观察 | 结论 |
| --- | --- |
| `header_seen=1` | CSI Host 已将一帧输入送达 ISP 输入端。 |
| `tail_seen=1` | ISP RAW 旁路已经输出完整帧边界。 |
| `bridge_fifo_max=384` | Bridge FIFO 曾实际收到数据。 |
| `dma_dst_advance_max=323520` | DMA 目标地址曾在调用者帧缓冲中推进。 |
| `diag: result=0` | GDMA 完成中断已发出 `frame_sem`，等待帧成功。 |

所以此时物理链路、CSI 包接收、ISP/Bridge 通路和 DMA 完成都已有正向证据。
失败点已经缩小为 DMA 完成后的 CPU 缓存同步。

## 3. 根因

### 3.1 ISP 旁路和 Bridge 配置不完整

ESP32-P4 的 RAW 直通仍会经过 ISP Tail。复位后的 ISP 默认状态不能假定符合
CSI RAW 直通要求。原实现只初始化 CSI Host 和 Bridge，未在访问 ISP 寄存器前
完成 ISP 时钟、复位及 RAW 旁路配置，Bridge 的每行宽度单位也没有按硬件要求
使用 64-bit word。

这使首帧超时阶段无法区分数据是否停在 Host/ISP/Bridge 的哪一段，也不能保证
Bridge 得到正确帧边界。

### 3.2 首帧 GDMA 通道没有显式启动

完成中断处理会为下一帧重新装载 LLI 并使能 DMA。旧路径依赖这段“下一帧”逻辑，
却没有在首次使能 Bridge 前显式使能 Channel 0。没有首个 DMA 请求，就不会有
DMA 完成中断来启动后续循环。

### 3.3 CPU 缓存失效标志组合非法

GDMA 收到帧后，应用调用：

```c
esp_cache_msync(buffer, bytes,
                ESP_CACHE_MSYNC_FLAG_DIR_M2C |
                ESP_CACHE_MSYNC_FLAG_UNALIGNED);
```

`DIR_M2C` 表示将 DMA 写入的数据对 CPU 可见。ESP32-P4 的
`esp_cache_msync()` 对该方向不允许 `UNALIGNED`；它返回参数错误。驱动将
Espressif 错误码映射为 NuttX `-EIO`，因此应用日志表现为：

```text
csi_probe: FAIL step=buffer_sync ret=-5
```

这不是 DMA 写入失败。诊断日志的 Header、Tail、Bridge FIFO、DMA 地址推进和
完成信号量已经证明帧到达了内存路径。

## 4. 修改方案

### 4.1 完整建立 P4 RAW 接收通路

在 `chips/esp32p4/common/espressif/esp_mipi_csi.c` 中完成以下初始化：

- 在访问 ISP 前使能 ISP 工作时钟（PLL160 / 2 = 80 MHz）、模块时钟并复位。
- 配置 ISP RAW 旁路：`ISP_EN=0`、`MIPI_DATA_EN=1`；禁用显式 line-start 和
  line-end packet 依赖。
- 按 RAW8 1024 × 600 设置 ISP 输入宽度：每行 `1024 * 8 / 32 = 256` 个
  32-bit word。
- 将 Bridge 每行宽度按硬件定义配置为 `1024 * 8 / 64 = 128` 个 64-bit word，
  同时保留高度 600、DT `0x2a`、FIFO 阈值和 burst 配置。
- 启用 Bridge 时钟，并在启用 Bridge 前显式使能 GDMA Channel 0。

这使硬件路径与 P4 的 `CSI Host -> ISP Tail -> Bridge -> GDMA` 架构一致。

### 4.2 修复缓存同步

`esp_mipi_csi_buffer_sync_for_cpu()` 改为只使用合法的 M2C 标志：

```c
return esp_mipi_csi_result(esp_cache_msync(
  buffer, bytes, ESP_CACHE_MSYNC_FLAG_DIR_M2C));
```

由于不再使用 `UNALIGNED`，该接口会检查帧缓冲地址和长度是否按 P4 的 64-byte
cache line 对齐。`csi_probe` 已通过 `memalign(64, 614400)` 分配缓冲；
`614400` 也是 64 的整数倍，因此满足该约束。

启动 DMA 前的 Cache Clean 仍使用 C2M 方向；该阶段与 DMA 完成后的 M2C
失效方向不同，不能混用其对未对齐范围的规则。

### 4.3 为失败位置增加最小诊断

新增 `esp_mipi_csi_wait_frame_diag()` 和 `csi_probe rawdiag`。诊断代码仅观察
已经由初始化拥有的 ISP 时钟和数据通路，输出以下分界证据：

- ISP Header/Tail 帧事件；
- Bridge FIFO 峰值、Host/Bridge 配置和错误状态；
- GDMA `CHEN`、LLP、SAR、DAR 和目标地址推进量。

此命令用于 bring-up 定位；常规回归使用 `csi_probe raw`。

## 5. 真机验证结果

### 单帧

```text
nsh> csi_probe raw
frame: bytes=614400 crc32=0xb347221c nonzero=yes nonconstant=yes
stats: frames=1 dma=0 bridge(overrun=0 fifo=0 discard=0 size=0)
stats: csi(ecc=0 crc=0 phy=0 packet=0)
csi_probe: PASS raw frames=1 crc32=0xb347221c
```

### 连续十帧

```text
nsh> csi_probe raw 10
frame: bytes=614400 crc32=0x2df774f1 nonzero=yes nonconstant=yes
stats: frames=10 dma=0 bridge(overrun=0 fifo=0 discard=0 size=0)
stats: csi(ecc=0 crc=0 phy=0 packet=0)
csi_probe: PASS raw frames=10 crc32=0x2df774f1
```

`raw 10` 使用一个可循环重装的 DMA LLI 和一个帧缓冲。它证明收到十次完整 DMA
完成事件，缓冲中保留最后一帧。两次独立命令的 CRC 不同是活动场景、自动曝光或
传感器噪声下的正常现象，不能据此判定数据错误。

`last(dma=0x00000002)` 是最后一次 DMA 完成中断状态的历史记录；它不是 DMA
错误计数。实际的 `dma=0`、CSI/Bridge 错误计数为零，说明本次十帧测试未记录
DMA、协议或 FIFO 错误。

## 6. 当前验收范围和后续建议

本次已验证从 SC2336 到 RAM 的 RAW8 接收闭环，包括控制面、帧边界、DMA、
缓存一致性和基础数据分布检查。`nonzero=yes` 与 `nonconstant=yes` 只证明
缓冲区包含变化的数据；尚未验证 Bayer 排列、画面方向、曝光、坏点和颜色质量。

后续建议按以下顺序扩展验证：

1. 执行 `csi_probe raw 300`，覆盖约十秒连续接收并观察错误计数。
2. 导出最后一帧 RAW8，按 SC2336 的 BGGR 格式进行离线预览，检查图像内容和
   行列是否正确。
3. 在此基础上接入多帧缓冲、ISP 格式转换和正式视频设备节点。

## 7. 相关文件

| 文件 | 作用 |
| --- | --- |
| `app/csi_probe/csi_probe_main.c` | `raw`、`rawdiag` 命令、帧校验和统计输出。 |
| `chips/esp32p4/common/espressif/esp_mipi_csi.c` | ISP 旁路、Bridge、GDMA、缓存同步和诊断实现。 |
| `chips/esp32p4/include/esp_mipi_csi.h` | CSI 诊断与缓存同步接口约束。 |
| `board/esp32p4/esp32p4-function-ev-board/src/esp32p4_camera.c` | SC2336 profile、上电与接收端配置。 |
| `docs/硬件适配/ESP32-P4X-SC2336-CSI-RAW接收交接.md` | 早期交接、首帧超时和 `rawdiag` 定位过程。 |
