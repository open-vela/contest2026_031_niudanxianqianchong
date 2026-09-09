# ESP32-P4X SC2336 CSI RAW 接收交接

> 更新日期：2026-09-08
> 适用范围：`ESP32-P4X Function EV Board`、SC2336、OpenVela/NuttX

## 1. 当前结论

SC2336 的 SCCB 控制面和 RAW8 数据面均已打通。真机 `csi_probe raw` 已成功
接收一帧，`csi_probe raw 10` 已成功接收十帧；帧缓冲为 614400 byte、非全零且
非恒定，CSI、Bridge 和 DMA 错误计数均为零。

本文保留首帧 `-ETIMEDOUT` 阶段的交接和 `rawdiag` 定位过程，供后续复现时
比对。最终根因、代码修改和通过日志见
[`CSI RAW 接收故障修复记录`](../开发日志/ESP32-P4X-SC2336-CSI-RAW接收故障修复.md)。

```text
已验证：SC2336 SCCB / 模式配置 / stream on / CSI Host / ISP 旁路 /
Bridge / GDMA / 缓存同步 / 单帧与十帧 RAW 接收
```

## 2. 已确认的硬件和 profile 参数

| 项目 | 当前值 | 证据与边界 |
| --- | --- | --- |
| SCCB | I2C0，SDA=GPIO7，SCL=GPIO8，100 kHz，地址 `0x30` | 真机读写通过 |
| 产品 ID | `0xcb3a`，寄存器 `0x3107/0x3108` | 真机多次读取通过 |
| MIPI D-PHY LDO | channel 3，2500 mV | 软件申请成功；未测量实际波形 |
| 图像模式 | RAW8、BGGR、1024 x 600、30 fps | SC2336 官方模式表 |
| CSI-2 | 2 data lane，DT=`0x2a` | 模式表和接收配置一致 |
| 传感器串行速率 | 288 Mbps/lane | SC2336 模式表参数 |
| P4 Host PHY 速率选择 | 200 Mbps/lane | 同板 ESP-IDF 基线使用值；待 OpenVela 收帧验证 |
| MCLK/XCLK | 24 MHz 输入 | ESP-IDF 基线不由 P4 GPIO 输出；本项目尚无实际波形证据 |
| RESET/PWDN | 未由 P4 GPIO 驱动 | ESP-IDF 基线配置为 `-1`；尚未从原理图确认模组侧电平与时序 |
| 单帧长度 | 614400 bytes | `1024 * 600 * RAW8` |

`288 Mbps/lane` 是传感器模式表的串行输出参数，`200 Mbps/lane` 是 P4 Host
PHY 的 HS 频率选择输入；二者属于不同层，不应将其视为同一寄存器配置。

## 3. 已完成工作

### 3.1 SCCB/I2C 缺陷已经修复

根因是 ESP32-P4 I2C 下层构造硬件命令时，`ack_exp` 等字段未整体初始化。该
字段进入硬件命令后会使 ACK 检查结果依赖栈残留值，造成同一传感器在不同调用
路径下表现不一致。

修复后，`pointer`、`split`、`sensor`、`sccb`、`write` 已在真机通过：

```text
SC2336 detected: address=0x30 product_id=0xcb3a
csi_probe: PASS sccb_write product_id=0xcb3a
```

因此，当前 RAW 超时不能再归因于原先的 SCCB ACK 初始化缺陷。

### 3.2 SC2336 模式配置与启动

`esp32p4_sc2336.c` 使用 Espressif `esp_cam_sensor` 中的
`MIPI_2lane_24Minput_RAW8_1024x600_30fps` 模式表。写表后，应用执行
`0x0100=0x01` 并打印：

```text
SC2336 profile configured: RAW8 1024x600 30fps, 2-lane 288Mbps
SC2336 stream on
```

上述日志只证明 SCCB 写操作成功返回，尚不能证明 24 MHz MCLK 存在、传感器
像素阵列已运行或 MIPI Lane 已输出 HS 数据。

### 3.3 CSI Host、Bridge 与 GDMA 启动

`esp_mipi_csi_initialize()` 已完成 D-PHY 时钟源、Host/Bridge 时钟与复位、HAL
初始化和 Bridge 参数配置。`esp_mipi_csi_start()` 已完成帧缓冲 Cache Clean、
GDMA 描述符、GDMA IRQ 和 Bridge 使能。

此前发现首帧路径没有使能 GDMA 通道，只有 DMA 完成 ISR 才会为下一帧重新使能，
首帧因此无法开始。该缺陷已修复：首帧在启用 Bridge 前显式使能 Channel 0，
最新真机日志确认回读有效：

```text
INFO: MIPI-CSI start: stage=dma_channel_enable channel=0
INFO: MIPI-CSI start: stage=dma_channel_enable channel=0 chen=0x00000001 enabled=1
INFO: MIPI-CSI start: stage=bridge_enable
```

## 4. 最近一次 RAW 失败的完整判断

最近一次 `csi_probe raw` 的关键状态为：

```text
stats: frames=0 dma=0 bridge(overrun=0 fifo=0 discard=0 size=0)
stats: csi(ecc=0 crc=0 phy=0 packet=0)
stats: host(main=0x00000000 phy_fatal=0x00000000 packet_fatal=0x00000000 phy=0x00000000)
stats: phy(rx=0x00010000 stopstate=0x00010003)
stats: bridge(raw=0x00000000 enable=0x00000001 buffer=0x000003c0)
stats: dma(channel=0x00000000 transferred_64bit=0 fifo_64bit=0 source=0x00000000)
csi_probe: FAIL step=wait_frame frame=1 ret=-110
```

| 观察项 | 含义 | 当前结论 |
| --- | --- | --- |
| `chen=1` | GDMA Channel 0 已使能 | 排除首帧 GDMA 未启动 |
| `transferred_64bit=0`（旧标签） | 上一块完成量为零，通道使能时清零 | 不能作为实时进度或无 DMA 请求的证据 |
| Bridge `enable=1` | Bridge 已开启 | 不是遗漏 Bridge 使能 |
| Bridge FIFO 深度为 0 | 采样时没有积累像素数据 | 不能单凭瞬时空 FIFO 排除之前有数据通过 |
| Host/Bridge 错误均为 0 | 未留存可报告的协议或 FIFO 错误 | 不能据此证明物理链路正常 |
| `stopstate=0x00010003` | clock、data0、data1 在采样时刻均为 Stop State | 当时无 HS 活动；这是瞬时状态，不能单独证明整秒从未进入 HS |

MIPI 数据路径为：

```text
SC2336 Sensor
  └─ CSI-2 RAW8, DT=0x2a, 2 lane
       └─ P4 CSI Host / D-PHY
            └─ ISP_Tail（选择 CSI 直通或 ISP Pipeline 输出）
                 └─ CSI Bridge FIFO
                      └─ GDMA Channel 0
                           └─ 614400-byte RAM buffer
```

DMA 不是直接从 MIPI Lane 取数；它在 CSI Bridge FIFO 有可读数据且握手有效时，
才会开始写入 RAM。应结合 ISP 输入/输出帧事件、Bridge FIFO 的多次采样和
DMA 目标地址推进判断位置，不能用块完成量为零直接排除 DMA 后半段。

## 5. 首帧超时时的原因假设与优先级（历史）

本次已修复的遗漏项：此前驱动复位 CSI Bridge 后没有显式配置 ISP 旁路；寄存器
定义中的复位值是 `ISP_EN=1`、`MIPI_DATA_EN=0`。ESP-IDF 对照工程则初始化
并启用 ISP。这个差异可能阻断 Host 到 Bridge 的数据，但需要真机回读与帧
事件确认，不能仅凭默认值认定根因。最新代码已补齐时钟与旁路，真机验证待完成。
先执行第 9 节，再按证据选择以下方向。

1. **MCLK 或模组上电/复位条件未满足。** SCCB 可访问不等于图像时钟和 MIPI
   发射器正常。SC2336 需要 24 MHz 输入；该时钟不由当前 P4 GPIO 驱动，必须从
   原理图或示波器确认模组输入端存在且稳定。
2. **SC2336 未实际进入输出状态。** `0x0100=1` 的写 ACK 只证明总线事务成功，
   应回读该寄存器及模式关键寄存器，确认状态确实锁存。
3. **接收侧时钟/复位时序仍与 ESP-IDF 有差异。** 当前软件已能启动 Host、Bridge
   和 DMA，但应继续逐项对齐 ESP-IDF 的 Host bus clock 与 PHY config clock 的
   关/开顺序，再判断是否存在接收器初始化差异。
4. **MIPI 电气连接或 Lane 对应问题。** 若确认 MCLK 与寄存器状态正确而 PHY
   采样始终从未出现 HS，检查时钟 Lane、data0/data1、极性、连接器和供电。

当前没有证据把根因定为 DMA、SCCB 或 RAW8 数据类型过滤错误。`DT=0x2a`、
2 Lane、模式表及 Host PHY 选择值均已与同板 ESP-IDF 基线对照。

## 6. 首帧超时后的验证顺序（历史）

1. 通过原理图、示波器或已通过的 ESP-IDF BSP，确认 SC2336 的 24 MHz MCLK、
   模组供电、RESET 和 PWDN 的实际连接、有效电平及延时。
2. 在 `stream on` 后增加 SCCB 回读：至少验证 `0x0100=0x01`、`0x301f=0xc7`、
   `0x3031=0x08`，将“写操作成功”提升为“目标寄存器已锁存”。
3. 在 Bridge 使能后的约 50 ms 内，以 1 ms 间隔采样 `phy_rx` 与
   `phy_stopstate`，统计是否曾出现 `rxclkactivehs=1` 或任一 Lane 脱离 Stop
   State。不要循环读取 Host 的 read-clear 中断状态寄存器。
4. 若从未观测到 HS，优先解决硬件时钟、复位、供电或 MIPI 连接；此时不应继续
   调整 DMA 描述符。
5. 若观察到 HS 而 DMA 仍为零，记录 Bridge 的 `data_type_cfg`、`frame_cfg`、
   `host_ctrl`、FIFO 深度和 GDMA 当前 LLI，再对齐 ESP-IDF 驱动的接收配置。
6. 单帧成功后，恢复并修复第二路动态 IRQ 分配，再验证连续 `raw 10`、缓存
   一致性和错误计数；最后才进入 ISP、`/dev/video0` 与预览链路。

## 7. 代码、文档与验证入口

| 位置 | 作用 |
| --- | --- |
| `app/csi_probe/csi_probe_main.c` | `csi_probe raw` 诊断入口、超时统计输出 |
| `board/esp32p4/esp32p4-function-ev-board/src/esp32p4_sc2336.c` | SC2336 SCCB、RAW8 表和 stream 控制 |
| `board/esp32p4/esp32p4-function-ev-board/src/esp32p4_camera.c` | 板级 profile、I2C 与 D-PHY LDO 生命周期 |
| `chips/esp32p4/common/espressif/esp_mipi_csi.c` | Host、Bridge、GDMA、首帧启动与硬件状态快照 |
| `docs/硬件适配/ESP32-P4X-SC2336摄像头适配.md` | 完整适配说明、历史真机日志和构建命令 |
| `docs/开发日志/SC2336-CSI-DMA数据流与调用链.md` | 数据流、调用链和寄存器状态解释 |

推荐最小回归顺序：

```text
nsh> csi_probe sensor
nsh> csi_probe write
nsh> csi_probe raw
```

`sensor` 和 `write` 通过只表示 SCCB 控制面正确。只有 `raw` 返回成功、帧数据
非全零且长度为 614400 bytes，才能宣告单帧 RAW 接收完成。

## 8. 修复前完成度（历史）

| 子项 | 状态 |
| --- | --- |
| I2C/SCCB 基础读写与软件复位 | 已完成并真机验证 |
| SC2336 ID 探测、RAW8 1024x600 配置、stream 控制 | 已完成；输出状态待硬件证实 |
| D-PHY LDO、CSI Host、Bridge、GDMA 软件装配 | 已完成启动级验证 |
| 首帧 RAW 到 RAM | 未完成，当前阻塞项 |
| 双 IRQ 正式错误处理 | 未完成，第二次动态 IRQ 申请会阻塞 |
| 连续多帧与数据质量验证 | 未开始 |
| ISP 转换、`/dev/video0`、显示/AI 上层集成 | 未开始 |

## 9. 最小故障分界验证：rawdiag

命令只接收一帧，观测初始化阶段配置好的接收通路，不再临时改动 ISP 时钟。
初始化会在任何 ISP 寄存器访问前启用 PLL160 / 2 = 80 MHz 工作时钟，复位
ISP 并配置 RAW 旁路。ISP 时钟保持到接收器反初始化，公共时钟源的释放遵循
现有 HAL。`rawdiag` 在访问 ISP 前检查接收状态和 HP_SYS_CLKRST 中的时钟门控；
未启动或时钟未开返回 `-EPIPE`。旧版“先读 ISP.clk_en 再开时钟”的实现已移除。

构建与真机命令：

```bash
./build.sh \
  contest2026_031_niudanxianqianchong/board/esp32p4/esp32p4-function-ev-board/configs/csi_probe \
  -j2
```

```text
nsh> csi_probe sensor
nsh> csi_probe raw
nsh> csi_probe rawdiag
```

请保存 ISP 初始化阶段及 `diag: enter` 到最终 PASS/FAIL 的完整输出。`rawdiag` 超时本身
仍会打印 FAIL；诊断的价值是超时前的分界观测，不是预先保证收帧成功。

| 观测组合 | 支持的判断与下一步 |
| --- | --- |
| `isp_en=0 mipi_data_en=1` | 符合新版 RAW 旁路配置；继续看 Header/Tail 和 DMA |
| `isp_en=1 mipi_data_en=0` | 不符合新版旁路配置；检查是否烧入旧镜像或配置被后续复位覆盖 |
| `header_seen=1 tail_seen=0` | 本窗口已收到 Host 输入帧事件，但未见 Tail 输出完整帧；重点检查 ISP 门控、旁路和帧配置 |
| `tail_seen=1`，DMA 没完成 | 检查 Bridge 过滤/帧边界、DMA LLI 和握手；不能再只查传感器输出 |
| `bridge_fifo_max>0` | Bridge 至少在某次采样有积累数据；结合 DMA 状态判断是否排空受阻 |
| `dma_dst_advance_max>0` | 曾观测到 DMA 目标地址推进到帧缓冲内部或末尾；不等同于完整有效帧 |
| `hs_samples>0`，但无 Header 事件 | PHY 曾观测到 HS；继续检查包解码、帧边界与接收配置 |
| HS、Header、Tail 全零 | 本窗口未观测到活动；需结合时钟/复位回读及传感器寄存器、波形，不能直接断言硬件未输出 |
| 普通 `raw` 超时，`rawdiag` 成功 | 新版两者通路配置相同，需重复对照检查时序、偶发错误和日志差异 |

`diag: enter` 在锁和 ISP MMIO 访问之前输出；`diag: before` 保存当前 ISP
控制位和时钟；`diag: window` 标明采样配置；
`diag: result` 是窗口汇总；后续两行保存 Bridge 配置与 DMA 的 CHEN、LLP、
SAR、DAR、CFG、CTL。LLP 是原始寄存器，低位包含 master-port 等控制信息。
这些硬件读数是依次读取的，不是冻结硬件后的同一时刻快照。

当前 `CONFIG_USEC_PER_TICK=10000`，轮询间隔约 10 ms，加上调度延迟。
HS 次数和 FIFO 最大值只反映采样结果，可能漏掉短暂活动；Header/Tail 使用
粘滞原始事件的按位累计，不是帧计数。帧事件在启动 DMA 前清除，进入诊断
等待时不再清除，保留等待之前已经完成的帧。循环不读取 Host 的读清中断寄存器，
也不清除 DMA 中断，Host 状态只在退出边界采样一次。

旧日志中的 `transferred_64bit` 和 `fifo_64bit` 已分别改名为
`completed_block_64bit` 和 `post_block_fifo_64bit`，提醒它们是块结束状态。

依据：[ESP32-P4 TRM](https://documentation.espressif.com/esp32-p4_technical_reference_manual_en.pdf)
§38.4（ISP_Tail 选择）、§38.6（Header/Tail 帧事件）、§7 寄存器 7.27–7.28
（块完成量与块结束 FIFO）。硬件收帧结果尚待回传。

## 10. rawdiag 卡住后的修复

用户完整日志显示：`sensor` 通过，`raw` 首帧超时，`rawdiag` 停在
`INFO: MIPI-CSI start: stage=complete result=0`，没有任何 `diag:` 行。
这不是 Header/Tail 全零的结果，而是尚未拿到诊断输出。

源码检查发现上一版在开工作时钟之前读取 `isp->clk_en.clk_en`，之后首条日志
的参数还会读取 `cntl` 与 `int_raw`。访问未开时钟的外设寄存器可能令总线
事务无法完成，位置与日志停点相符；没有 PC/总线跟踪，不能证明停在某条指令。

最新修改：

1. 正常 RAW 初始化先经 HP_SYS_CLKRST 开 ISP 工作时钟和复位，再访问 ISP
   寄存器；新增 `isp_clock_enable`、`isp_bypass_configure` 阶段日志。
2. 显式配置 `ISP_EN=0`、`MIPI_DATA_EN=1`，保留 ISP 输入/Tail 的 RAW 直通。
3. RAW8 1024 像素每行使用 ISP 输入宽度 256 个 32-bit 单元（HAL 写入 255），
   Bridge 宽度 128 个 64-bit 单元，高度 600。按当前 SC2336 模式关闭显式行
   同步包选项，并显式启用 CSI CM 旁路。
4. 接收器统一管理 ISP 时钟，清理时先访问 ISP 寄存器，再关闭工作时钟和
   Bridge 总线时钟。诊断只读取已经启动的模块，不再自行关开时钟。

后续真机已确认正常初始化打印 `isp_en=0 mipi_data_en=1 input_words32=256` 和
`words64_per_line=128`。`raw`、`rawdiag` 和 `raw 10` 的最终结果见前述故障修复
记录；本文保留本节作为当时的修复设计说明。

参考：[ESP-IDF ISP 初始化与 bypass 配置](https://github.com/espressif/esp-idf/blob/v5.5.5/components/esp_driver_isp/src/isp_core.c)。
本轮按用户要求未编译、未烧录，由用户执行。
