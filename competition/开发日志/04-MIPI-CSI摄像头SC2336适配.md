# MIPI-CSI 摄像头 SC2336 适配（ESP32-P4X）

> 本文档由仓库内 11 份源文档整合而成，只引用源文档事实与数字，供评委直接评审。

| 元信息 | 内容 |
| --- | --- |
| 传感器 | SC2336，产品 ID `0xcb3a`（`0x3107=0xcb`、`0x3108=0x3a`），RAW8/BGGR，1024×600@30fps，2 lane，DT `0x2a`，模式表串行速率 288 Mbps/lane；P4 CSI Host PHY 速率选择 200 Mbps/lane |
| 接口与数据流 | 控制面：SCCB（I2C0，SDA=GPIO7，SCL=GPIO8，100 kHz，从地址 `0x30`）。数据面：SC2336 → MIPI CSI-2 → D-PHY/CSI Host → ISP（RAW8/BGGR→RGB565）→ CSI Bridge → GDMA ch0 → 三块 PSRAM 暂存帧 → `esp_mipi_csi_video`（`imgdata_s`）→ V4L2 capture → `/dev/video0`（RGB565，单帧 1228800 byte） |
| 当前真机状态 | RAW 通路 `csi_probe raw`/`raw 10` 通过（错误计数全零）；`/dev/video0` 端到端 RGB565：`video_test 300` 应用帧率 30.02 fps、`sequence_gaps=0`、`acceptance=30fps-app`。每帧延迟与硬件全过程零丢帧证据不足（见第六节） |
| 关键代码入口 | `chips/esp32p4/common/espressif/esp_mipi_csi.c`、`esp_mipi_csi_video.c`、`esp_isp.c`；`board/esp32p4/esp32p4-function-ev-board/src/esp32p4_sc2336.c`、`esp32p4_camera.c`；`app/csi_probe`、`app/video_test`；`drivers/nuttx/patches/0001-v4l2-number-completed-frames.patch`、`0002-imgdata-set-buf-return-error.patch`（均已经 `ls` 核实存在） |

## 一、适配背景与目标

本项目以 Route A（custom chip + custom board）方式把 openvela（NuttX 内核）适配到
ESP32-P4X-Function-EV-Board。摄像头域的目标是把板载 SC2336 MIPI-CSI 模组接入
NuttX 标准视频框架，首期固定输出 1024 × 600、30 fps、`V4L2_PIX_FMT_RGB565`。

目标通路：

```text
SC2336 → MIPI CSI-2（2 lane，DT=0x2a）
       → ESP32-P4 D-PHY / CSI Host（PHY 速率档 200 Mbps/lane）
       → ISP（在线，RAW8/BGGR → RGB565，PLL160/2=80 MHz）
       → CSI Bridge（32-bit → 64-bit 打包，每行 256 word64，异步 FIFO 跨时钟域）
       → GDMA ch0（LLI，153600 × 64-bit/帧）
       → 三块 64-byte 对齐 PSRAM 暂存帧（环形复用，dma_paused 背压）
       → esp_mipi_csi_video（imgdata_s，HPWORK 中 memcpy 到 V4L2 交付缓冲）
       → V4L2 capture upper-half → /dev/video0 → video_test
```

适配遵循两条原则：

1. **控制面与数据面分离**。SCCB 读到产品 ID 只证明传感器能响应寄存器访问，
   不能证明 MIPI 视频数据已经输出；两链路必须分别验收。
2. **分阶段验收**。先 SCCB 识别，再 RAW8 旁路接收，再 ISP RGB565 与 V4L2，
   最后应用吞吐。`csi_probe`（RAW 旁路诊断）与 `video`（V4L2 正式通路）经
   Kconfig 互斥，不争抢唯一的 CSI Host、ISP、Bridge、GDMA 与 SC2336 会话。

帧大小：RAW8 `1024×600×1 = 614400` byte；RGB565 `1024×600×2 = 1228800` byte。
30 fps 下各段流量：sensor→MIPI 18.4 MB/s，ISP→GDMA 36.9 MB/s，HPWORK memcpy
再 +36.9 MB/s，内存合计约 111 MB/s。三块 DMA 暂存帧约 3.52 MiB，三块 V4L2
交付帧约 3.52 MiB，合计约 7.03 MiB（PSRAM）。

## 二、适配流程

按实际发生顺序（日期来自源文档维护记录）：

| 阶段 | 时间 | 内容 | 结果 |
| --- | --- | --- | --- |
| 1. 传感器 SCCB 识别 | 2026-09-07 | `csi_probe pointer/split/sensor/sccb/write` 打通 I2C0 读写闭环 | 修复 `esp_i2c.c` 命令初始化缺陷后各 1/1 PASS，读回 `0xcb3a` |
| 2. CSI Host/Bridge/GDMA 通路搭建 | 2026-09-07~08 | D-PHY LDO（ch3/2500 mV）、Host/Bridge 时钟复位、HAL 初始化、GDMA LLI/IRQ；遇到第二路动态 IRQ 不返回，临时改为单 GDMA IRQ + Bridge 粘滞 `int_raw` 轮询；Host PHY 速率对齐 ESP-IDF 基线 200 Mbps/lane | 软件启动链全部 `result=0`，但首帧 `-ETIMEDOUT` |
| 3. RAW 接收排障 | 2026-09-08 | 三个根因依次修复：ISP RAW 旁路未配置、首帧 GDMA 通道未显式使能、`esp_cache_msync` 非法标志组合；新增 `rawdiag` 分界诊断 | `csi_probe raw` 单帧 PASS、`raw 10` 十帧 PASS，CSI/Bridge/DMA 错误计数全零 |
| 4. RGB565/V4L2 接入 | 2026-09-09~10 | 新增 `esp_isp.c`（ISP RAW8/BGGR→RGB565）与 `esp_mipi_csi_video.c`（`imgdata_s` 三缓冲 + HPWORK 复制）；板级 `board_camera_initialize()` 注册 `/dev/video0`；懒初始化（首次 open 才上电） | `/dev/video0` 出现，`video_test` 首轮 10 帧内容验收通过 |
| 5. video_test 吞吐验收 | 2026-09-10 | 修复首帧 poll 超时（Bridge DT 过滤）、应用 3.7 fps 吞吐问题、首两帧序号重复 | `video_test 300`：`app_fps=30.02 sequence_gaps=0 acceptance=30fps-app` |
| 6. V4L2 patch 上游修复 | 2026-09-10 | 序号修复连带暴露 `IMGDATA_SET_BUF` 返回值问题，两处通用修改保存为 `drivers/nuttx/patches/0001`、`0002` | 主机桩验证覆盖 300 次环形完成/回绕等场景，旧实现失败、修复后通过 |

## 三、关键代码与配置

### 3.1 代码位置（全部已经 `ls` 核实存在）

| 位置 | 层级 | 职责 |
| --- | --- | --- |
| `chips/esp32p4/common/espressif/esp_mipi_csi.c` | 芯片层 | D-PHY/Host/Bridge/GDMA、ready/done 槽位环、`dma_paused` 背压、错误统计、诊断快照 |
| `chips/esp32p4/common/espressif/esp_mipi_csi_video.c` | 适配层 | `imgdata_s` 实现：三块 DMA 暂存帧、HPWORK 中 memcpy 与完成帧上报 |
| `chips/esp32p4/common/espressif/esp_isp.c` | 芯片层 | P4 ISP 原生适配：CSI 在线输入、BGGR、RAW8→RGB565、时钟/复位管理 |
| `chips/esp32p4/common/espressif/esp_i2c.c` | 通用 I2C | SCCB 所在总线驱动；本域修复过命令未整体初始化缺陷 |
| `board/esp32p4/esp32p4-function-ev-board/src/esp32p4_sc2336.c` | 板级（私有） | SC2336 SCCB 读写、产品 ID、约 150 条 RAW8 profile 写表、stream on/off |
| `board/esp32p4/esp32p4-function-ev-board/src/esp32p4_camera.c` | 板级 | 懒初始化装配：LDO/I2C0/CSI/ISP 生命周期、`board_camera_initialize()` 注册 `/dev/video0` |
| `nuttx/drivers/video/v4l2_cap.c` | 框架层 | V4L2 capture upper-half；本期经 patch 0001/0002 修改两处（见 3.2） |
| `app/csi_probe/csi_probe_main.c` | 应用层 | RAW 旁路诊断命令：`pointer/split/sensor/sccb/write/raw [N]/rawdiag` |
| `app/video_test/video_test_main.c` | 应用层 | V4L2 验收程序：`QBUF→STREAMON→poll→DQBUF→QBUF` 循环与统计 |
| `drivers/nuttx/patches/0001-v4l2-number-completed-frames.patch` | patch | 见 3.2 |
| `drivers/nuttx/patches/0002-imgdata-set-buf-return-error.patch` | patch | 见 3.2 |

分层判据：芯片层回答"能不能"（换板可复用），板级层回答"接在哪"
（引脚/地址/LDO/寄存器表），应用层回答"做什么"。SC2336 驱动当前为板级私有，
待接口稳定后再评估上提 `drivers/` 层。

### 3.2 两个 nuttx V4L2 patch 的作用

- **0001-v4l2-number-completed-frames.patch**（改 `drivers/video/v4l2_cap.c`）：
  在 `complete_capture()` 中为每一个完成帧（含 QBUF 提供的首块缓冲）统一执行
  `buf.sequence = seqnum++`，并删除原先只在回调末尾给"下一块"缓冲编号的语句。
  修复 STREAMON 后首两帧 sequence 均为 0 的重复问题（详见第五节问题 5）。
- **0002-imgdata-set-buf-return-error.patch**（改 `include/nuttx/video/imgdata.h`）：
  `IMGDATA_SET_BUF` 宏在 ops 缺省时由返回 `NULL` 改为返回 `-ENOTTY`，
  消除整数返回值与 NULL 混用。该问题在应用 0001 后重新编译上层时暴露。

两 patch 保存在 `drivers/nuttx/patches/` 供跨工作区复现；迁移工作区时需应用。

### 3.3 defconfig 与构建

| 配置 | 路径 | 说明 |
| --- | --- | --- |
| `csi_probe` | `board/esp32p4/esp32p4-function-ev-board/configs/csi_probe/defconfig` | RAW 旁路诊断配置，启用 `CONFIG_LVX_USE_DEMO_CONTEST2026_031_CSI_PROBE` |
| `video` | `board/esp32p4/esp32p4-function-ev-board/configs/video/defconfig` | `/dev/video0` 正式通路：`CONFIG_ESP32P4_FUNCTION_EV_BOARD_CAMERA_SC2336_VIDEO=y` + `CONFIG_LVX_USE_DEMO_CONTEST2026_031_VIDEO_TEST=y` |
| `capture` | `board/esp32p4/esp32p4-function-ev-board/configs/capture/defconfig` | 已核实存在；源文档未描述其用途，本域证据均基于 `csi_probe` 与 `video` 两个配置 |

构建命令（源文档原文）：

```bash
./build.sh \
  contest2026_031_niudanxianqianchong/board/esp32p4/esp32p4-function-ev-board/configs/csi_probe \
  -j2
```

### 3.4 csi_probe / video_test 用法

`csi_probe`（RAW 基线，NSH 内按序执行）：

| 命令 | 验收目标 |
| --- | --- |
| `csi_probe sensor` | D-PHY LDO ready，稳定读出 `product_id=0xcb3a` |
| `csi_probe sccb` | 读 ID → 写 `0x0100=0x00`（停流）→ 再读 ID 全部成功 |
| `csi_probe write` | 软件复位 `0x0103=0x01` → 5 ms → 停流 → 读回 `0xcb3a`（会改 sensor 状态） |
| `csi_probe raw` | 至少一帧合法 RAW：614400 byte、非全零、非恒定、错误计数为零 |
| `csi_probe raw 10` | 不少于 10 帧，同上判据 |
| `csi_probe rawdiag` | 逐 tick 采样 PHY/ISP/Bridge/DMA 的分界诊断，仅用于排障 |

`video_test`（`video` 配置，与 `csi_probe` 互斥，运行前不得执行诊断程序）：

```text
nsh> video_test 10 --full     # 全帧内容诊断
nsh> video_test 300           # 吞吐验收
```

设备路径固定 `/dev/video0`，每帧 poll 上限 1500 ms；吞吐模式每帧均匀检查 4 KiB
采样窗口（`sample_crc32` 不代表整帧 CRC）。至少 30 帧、28.5~31.5 fps 且序号缺口
为零才输出 `acceptance=30fps-app`；这不是逐帧实时期限保证。

## 四、适配证据（真机验收）

按"枚举成功 ≠ 数据有效 ≠ 时序达标"三级整理。只有逐级通过后才可宣告对应能力。

### 4.1 L1 枚举成功（设备与格式协商）

`video_test 300` 日志（出处：`docs/开发日志/应用/logs/2026-09-10-video_test-300-partial.txt`，
真机原始输出，逐字摘录）：

```text
video_test: device=/dev/video0 driver=SC2336 capabilities=0x04000001
video_test: format=RGB565 1024x600 size=1228800 interval=1/30
INFO: MIPI-CSI initialize: stage=complete frame_bytes=1228800
```

传感器识别（同上日志，逐字摘录）：

```text
INFO: MIPI-CSI D-PHY LDO ready channel=3 voltage_mv=2500
SC2336 detected: address=0x30 product_id=0xcb3a
SC2336 profile configured: RAW8 1024x600 30fps, 2-lane 288Mbps
SC2336 CSI profile ready: 2 lane(s), dt=0x2a, 1024x600 RAW8 sensor_rate=288Mbps/lane phy_rate=200Mbps/lane
INFO: MIPI-CSI revision: chip=3.2 build_min=301 bridge_color_conversion=1
```

### 4.2 L2 数据有效（真实像素到达应用内存）

RAW 通路（出处：`docs/开发日志/ESP32-P4X-SC2336-CSI-RAW接收故障修复.md` 第 5 节，
真机日志逐字摘录）：

```text
nsh> csi_probe raw
frame: bytes=614400 crc32=0xb347221c nonzero=yes nonconstant=yes
stats: frames=1 dma=0 bridge(overrun=0 fifo=0 discard=0 size=0)
stats: csi(ecc=0 crc=0 phy=0 packet=0)
csi_probe: PASS raw frames=1 crc32=0xb347221c
```

```text
nsh> csi_probe raw 10
frame: bytes=614400 crc32=0x2df774f1 nonzero=yes nonconstant=yes
stats: frames=10 dma=0 bridge(overrun=0 fifo=0 discard=0 size=0)
stats: csi(ecc=0 crc=0 phy=0 packet=0)
csi_probe: PASS raw frames=10 crc32=0x2df774f1
```

V4L2 通路首轮 10 帧内容验收（出处：`docs/开发日志/应用/ESP32-P4X-video_test实现方案.md`
第 8.1 节，逐字摘录）：

```text
nsh> video_test
video_test: format=RGB565 1024x600 size=1228800 interval=1/30
video_test: frame=0 sequence=0  bytes=1228800 ... nonzero=yes nonconstant=yes
...
video_test: frame=9 sequence=73 bytes=1228800 ... nonzero=yes nonconstant=yes
video_test: PASS frames=10
```

（注：该轮为修复吞吐问题前的旧实现，序号 0→73 反映 RING 覆盖，见第五节问题 4。）

### 4.3 L3 时序达标（应用侧吞吐）

300 帧真机日志逐字摘录。出处一（`docs/开发日志/应用/logs/2026-09-10-video_test-300-partial.txt`，
用户原始附件原样保存；逐帧记录截至 frame=43，其后无补写）：

```text
nsh> video_test 300
request: frames=300 timeout=1500ms
INFO: MIPI-CSI start: stage=dma_irq_setup result=0 cpuint=4
INFO: MIPI-CSI start: stage=dma_channel_enable channel=0 chen=0x00000001 enabled=1
INFO: MIPI-CSI start: stage=bridge_enable
SC2336 stream on
SC2336 stream off
CSI delivery: delivered=300 no_buffer=0 requeue_errors=0 copy_avg_us=20000 copy_max_us=20000
video_test: frame=0 sequence=0 timestamp=8.040000 sample_crc32=0x33e556ba
video_test: frame=1 sequence=1 timestamp=8.070000 sample_crc32=0x6e47c4e1
...（中间逐帧记录，序号连续递增，每帧 bytesused 均为 1228800）...
video_test: frame=43 sequence=43 timestamp=9.470000 sample_crc32=0x397bf868
```

出处二（`docs/开发日志/应用/2026-09-10-SC2336-video_test问题与解决方案.md`
"最新真机日志与证据边界"节：完整终端末尾汇总由用户在会话中提供，记录如下，
逐字摘录）：

```text
video_test: app_fps=30.02 sequence_gaps=0 mode=throughput
video_test: PASS frames=300 acceptance=30fps-app
```

派生估算（该文档原文口径）：frame=0 至 frame=43 共 43 个间隔 / 1.43 秒
≈ 30.07 fps，是展示区间的交付时间戳估算，不是最终 300 帧 DQBUF 汇总；
最终以上方 `app_fps=30.02` 为准。

### 4.4 分级结论

| 级别 | 判据 | 状态 |
| --- | --- | --- |
| 枚举成功 | `/dev/video0` 存在，QUERYCAP/S_FMT 协商 RGB565 1024×600 | 通过（4.1） |
| 数据有效 | RAW：614400 byte 非全零非恒定、错误计数全零；RGB565：300 帧 bytesused=1228800、nonzero/nonconstant、sample_crc32 逐帧变化 | 通过（4.2、4.3） |
| 时序达标 | 应用侧连续交付 30.02 fps、sequence_gaps=0、delivered=300/no_buffer=0 | 应用侧通过（4.3）；每帧延迟与硬件全过程零丢帧证据不足（第六节） |

## 五、遇到的问题与解决

### 问题 1：SCCB 写事务在地址阶段间歇失败（split 第一段）

- **现象**：独立 `pointer` 持续成功；`split` 第一段 pointer 写在地址阶段返回
  `-5`，未进入读取阶段。首 IRQ 快照 `raw=00000400 bytes=0`。
- **定位**：`bytes=0` 把失败定位到地址命令阶段。对修复前本地 ELF 反汇编发现，
  地址命令构造等价于 `(s4 & 0xffffc600) | 0x00000901`，保留了 bit9（`ack_exp`）；
  `pointer` 路径 `s4=3` 得命令 `0x00000901`（期望 ACK），`split` 路径
  `s4=g_readytorun 地址` 得 `0x4ff44b01`（期望 NACK）——同一 helper 因栈残留
  表现不同。
- **根因**：`chips/esp32p4/common/espressif/esp_i2c.c` 的
  `esp_i2c_sendstart()` 中 `restart_cmd/write_cmd/end_cmd` 为未整体初始化的
  自动变量，`ack_exp` 等字段进入硬件命令后依赖栈残留值。
- **修复**：START 路径的 RESTART/WRITE/END 改为聚合初始化；地址与数据 WRITE
  明确 `ack_en=1`、`ack_exp=0`；`esp_i2c_startrecv()` 的 READ/END 同样处理。
  反汇编确认地址 WRITE 命令固定为 `0x00000901`。
- **复测**：2026-09-07 真机 `pointer/split/sensor/sccb` 各 1/1 PASS
  （`split` 读回 `0xcb`，`sensor`/`sccb` 读回 `0xcb3a`）；`write` 1/1 PASS。
  边界：各命令仅单次通过，重复运行与断电冷启动统计未完成。

### 问题 2：RAW 首帧 poll 超时 / GDMA 不推进（三个子根因）

- **现象**：`csi_probe raw` 启动链全部成功
  （`MIPI-CSI start: stage=complete`），但 `esp_mipi_csi_wait_frame()` 1 秒后
  返回 `-ETIMEDOUT`（`ret=-110`）；超时快照 `frames=0`、DMA/Bridge/CSI 错误
  与最近状态全零。
- **定位**：新增超时状态快照（Host/PHY/Bridge/GDMA 四级状态）与
  `csi_probe rawdiag` 分界诊断。修复 ISP 旁路与首帧 DMA
  使能后，`rawdiag` 得到（出处：`docs/开发日志/ESP32-P4X-SC2336-CSI-RAW接收故障修复.md`，
  逐字摘录）：

```text
diag: result=0 samples=2 hs_samples=0 non_stop_samples=0
      isp_events=0x18000000 header_seen=1 tail_seen=1
      bridge_fifo_max=384 dma_dst_advance_max=323520
csi_probe: FAIL step=buffer_sync ret=-5
```

  `header_seen=1`、`tail_seen=1`、`bridge_fifo_max=384`、`dma_dst_advance_max=323520`
  证明物理链路、CSI 包接收、ISP/Bridge 通路与 DMA 完成均有正向证据，失败点
  缩小为 DMA 完成后的 CPU 缓存同步。
- **根因**（三个，依次暴露）：
  1. 接收侧未完整配置 ISP RAW 旁路：复位默认 `ISP_EN=1、MIPI_DATA_EN=0`，
     且访问 ISP 寄存器前未开工作时钟（PLL160/2=80 MHz）；
  2. 首帧启动未显式使能 GDMA Channel 0——旧路径只在完成中断里为"下一帧"
     重装 LLI/使能通道，首帧永远等不到；
  3. `esp_mipi_csi_buffer_sync_for_cpu()` 把 `DIR_M2C` 与 `UNALIGNED` 组合传给
     `esp_cache_msync()`，ESP32-P4 不接受该组合，映射为 NuttX `-EIO`，应用
     误报 `buffer_sync ret=-5`。
- **修复**：初始化先开 ISP 时钟并复位，显式配置 `ISP_EN=0、MIPI_DATA_EN=1`，
  RAW8 输入宽度 256 word32/行、Bridge 宽度 128 word64/行（高 600、DT `0x2a`）；
  在 Bridge 使能前显式使能 GDMA Channel 0 并回读 `CHEN`（真机日志
  `chen=0x00000001 enabled=1`）；缓存失效改为仅 `DIR_M2C`，配合
  `memalign(64, 614400)` 的 64-byte 对齐约束。
- **复测**：见第四节 4.2，`raw` 单帧与 `raw 10` 全部 PASS，错误计数为零。

### 问题 3：RGB565 通路首帧 poll 超时（Bridge DT 过滤）

- **现象**：`video_test` 在 `VIDIOC_STREAMON` 后首帧 `poll()` 1500 ms 返回
  `errno=110`（`FAIL step=poll frame=0 errno=110`）。SCCB 与启流正常，CSI Host
  无 fatal，ISP `raw=0x1c884000` 表明流水线有帧与处理事件，Bridge `raw=0x21`，
  GDMA `done64=0 fifo64=0 offset=0`。
- **定位**：故障收敛到 ISP 输出 → CSI Bridge → GDMA 一段。曾尝试把 RGB565
  分支的 Bridge 行配置从 600 对齐到 ISP 末行索引 599（`vadr_num_gt_real`
  状态），未恢复传输——行索引差异不是唯一根因。板端诊断确认实测芯片
  revision 3.2、构建最低版本 3.1、Bridge 色彩转换寄存器真实可用，推翻了此前
  "低于 v3、色彩转换接口为空实现"的假设。
- **根因**：CSI Bridge 数据类型过滤原先固定为传感器输入 RAW8 的 `0x2a`，
  而 RGB565 路径上 Bridge 接收的是 ISP 输出数据面，单一 `0x2a` 过滤使 Bridge
  不向 GDMA 交付有效数据。
- **修复**：`chips/esp32p4/common/espressif/esp_mipi_csi.c` 将 Bridge DT 过滤
  恢复为 Espressif CSI HAL 标准范围 `0x12`~`0x2f`（寄存器读回
  `data_type=0x00002f12`），并增加 revision/host_cm_ctrl/dma_req_cfg/dmablk_size
  日志。`bridge_rows=599` 配置保留，但其必要性没有独立 A/B 证据；扩大 DT
  范围是恢复取帧的决定性改动。
- **复测**：端到端测试立即通过；随后完成 10 帧内容验收与 300 帧吞吐验收
  （第四节 4.2/4.3）。

### 问题 4：能取真实图像但应用仅约 3.7 fps

- **现象**：旧测试 10 帧应用速率约 3.7 fps，V4L2 序号从 0 跳至 73。
- **定位**：每帧 DQBUF 后同步扫描完整 1.2 MiB、计算 CRC 与最值并打印串口，
  最后才 QBUF，消费者处理时间过长。同时区分两种损失：V4L2 RING 模式覆盖
  未 DQBUF 的完成帧（表现为应用序号缺口）；芯片回调没有交付目标时丢弃 DMA
  帧（不增加 V4L2 sequence）。据此撤回"单槽位导致 7~9 帧固定丢弃"的确定性
  归因——`complete_capture()` 会立即 `SET_BUF` 补充下一块目标，不必等应用 QBUF。
- **修复**（`app/video_test/video_test_main.c`、
  `chips/esp32p4/common/espressif/esp_mipi_csi_video.c` 及头文件）：默认每帧
  仅均匀检查 4 KiB 采样窗口；QBUF 后不再读取像素；元数据独立保存、
  STREAMOFF/close 后才打印；增加 DQBUF 单调时钟平均速率与 `sequence_gaps`；
  驱动退出时报告 delivered/no_buffer/requeue_errors/copy 耗时；先关设备等待
  工作队列退出再释放 USERPTR。未把三块 DMA 暂存缓冲改为 DMA 直接写 V4L2 缓冲，
  仍有整帧 memcpy 成本。
- **复测**：`video_test 300` 达到 `app_fps=30.02 sequence_gaps=0`、
  `delivered=300 no_buffer=0 requeue_errors=0`（第四节 4.3）。边界：这是应用侧
  平均吞吐验收，不是逐帧实时期限保证。

### 问题 5：轻量模式首两帧 V4L2 sequence 重复为 0

- **现象**：`FAIL step=sequence frame=1 previous=0 actual=0`，同时
  `delivered=2 no_buffer=0 requeue_errors=0`——序号元数据错误，不是无缓冲丢帧。
- **根因**：`nuttx/drivers/video/v4l2_cap.c` 原实现只在完成回调末尾给"下一块"
  缓冲执行 `seqnum++`：首块缓冲沿用 QBUF 默认序号 0，而第一次给下一块赋值时
  计数器也为 0，导致重复。
- **修复**：编号统一移到当前帧完成时执行（即 patch 0001）。重新编译上层又
  暴露 `IMGDATA_SET_BUF` 整数返回值与 NULL 混用，缺省分支改为 `-ENOTTY`
  （即 patch 0002）。
- **验证**：主机桩环境执行实际 `complete_capture` 代码，覆盖 300 次环形缓冲
  完成、QBUF 初始序号覆盖、错误帧与 uint32 回绕；旧实现失败，修复后通过。
  主机验证不覆盖硬件 DMA、缓存或真实调度；真机复测见 300 帧日志序号 0→43
  连续递增且 `sequence_gaps=0`。

### 未确认修复：第二路动态 IRQ 分配

真机上第二次动态 IRQ 申请不返回（Bridge/GDMA 先后顺序互换后仍复现）。
当前视频路径只用 GDMA IRQ + Bridge 粘滞 `int_raw` 轮询，错误发现时机有延迟
（最多到下一次 DMA 完成、超时或读统计）。该问题未确认修复，双 IRQ 正式
设计仍待恢复。

## 六、当前验收状态与边界

已验收（有真机日志支撑）：

- SC2336 SCCB 基础读写闭环、软件复位序列（各 1/1 通过）；
- RAW8 单帧与十帧接收：614400 byte、非全零、非恒定，CSI/Bridge/DMA 错误
  计数全零；
- `/dev/video0` 注册与 RGB565 1024×600 格式协商；
- 端到端 10 帧内容验收（bytesused=1228800、nonzero/nonconstant、CRC 变化）；
- 300 帧应用侧吞吐：`app_fps=30.02 sequence_gaps=0 acceptance=30fps-app`、
  `delivered=300 no_buffer=0 requeue_errors=0`。

证据不足或待验证（如实保留）：

- **每帧延迟与硬件全过程零丢帧**：现有证据不足以单独验收。30 fps 是应用侧
  DQBUF 平均速率；帧完成恒以 `result=0` 上报，DMA/Bridge 错误只进统计，尚未
  映射到 `V4L2_BUF_FLAG_ERROR`。
- **时钟粒度**：30/30/40 ms 记录间隔与粗粒度时钟相符，不能归因于采集抖动；
  `copy_avg_us=20000/copy_max_us=20000` 不是每帧精确耗时，也不是 CPU 占用率。
- **300 帧日志为部分文件**：逐帧记录截至 frame=43，最终汇总行由用户在会话中
  提供（出处已在 4.3 注明），无补写或推测。
- **画质与 ISP 调优**：当前 ISP 仅 demosaic，CCM/Gamma/LSC/白平衡未调；
  Bayer 顺序、RGB565 字节序、黑电平、颜色矩阵等画质项待 `video_preview`
  或导出图像后验收。图像色彩调优是独立后续工作。
- **稳定性**：长时间内存稳定性、重复启停（第二次 STREAMON 统计）、实际
  显示/算法业务负载、SCCB 重复运行与断电冷启动统计均待验证；有限缓冲不能
  保证任意慢消费者永远不丢帧。
- **SCC/I2C 读法**：当前含 STOP 的两消息读为已通过基线；repeated START
  （`I2C_M_NOSTOP`）未验证，不能断言 SC2336 必须其支持。
- **架构文档行号**：函数级行号基于 `dev-ai-contest-2026` 的 `76dd9d3`，
  后续提交可能漂移，以函数名与源码为准。
- `csi_probe raw 10` 的单缓冲语义是"累计 10 次完成事件、缓冲保留最后一帧"，
  非多帧队列；两次运行 CRC 不同属活动场景/自动曝光/噪声下的正常现象。

## 附录：原始文档索引

| 源文档（仓库相对路径） | 内容 |
| --- | --- |
| `docs/开发计划/摄像头与视觉/ESP32-P4X-MIPI-CSI摄像头适配方案.md` | `/dev/video0` 实施计划：目标数据流、文件改动清单、三缓冲设计、V0~V5 验收阶段 |
| `docs/硬件适配/ESP32-P4X-SC2336摄像头适配.md` | 硬件配置表、SCCB/CSI 排障史、I2C 命令初始化缺陷修复与真机 PASS 日志 |
| `docs/硬件适配/ESP32-P4X-SC2336-视频通路架构与调用链.md` | video0 通路分层架构、逐段数据流、函数级调用链、关键设计决策与证据边界 |
| `docs/硬件适配/ESP32-P4X-SC2336适配交接记录.md` | 修复前排障快照：`ack_exp` 未初始化的源码证据、ELF 反汇编与候选原因分析 |
| `docs/硬件适配/ESP32-P4X-SC2336-CSI-RAW接收交接.md` | RAW 接收交接：`rawdiag` 分界验证设计、观察组合判读表、ISP 时钟修复 |
| `docs/开发日志/SC2336-CSI-DMA数据流与调用链.md` | RAW 诊断路径的控制面/数据面调用链、DMA/LLI/中断与错误统计字段解释 |
| `docs/开发日志/ESP32-P4X-SC2336-CSI-RAW接收故障修复.md` | RAW 三根因（ISP 旁路/首帧 DMA 使能/cache msync）与单帧、十帧 PASS 日志 |
| `docs/开发指南/ESP32-P4X-SC2336摄像头Demo接入指南.md` | RAW 原语 Demo 接入：板级接口、最小示例、Kconfig 依赖与错误处理（该指南成文于 RAW 阶段，其"当前不具备 /dev/video0"表述早于 V4L2 接入） |
| `docs/开发日志/应用/ESP32-P4X-video_test实现方案.md` | video_test 设计：V4L2 调用顺序、格式/内存约束、帧有效性检查、首轮验收与问题闭环 |
| `docs/开发日志/应用/2026-09-10-SC2336-video_test问题与解决方案.md` | 三个问题归档（首帧超时/3.7 fps/序号重复）、两 patch 说明、300 帧证据边界 |
| `docs/开发日志/应用/logs/2026-09-10-video_test-300-partial.txt` | 真机 300 帧运行原始日志（部分，逐帧至 frame=43） |
