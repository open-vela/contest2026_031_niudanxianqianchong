# ESP32-P4X MIPI-CSI 摄像头适配方案

## 1. 文档目的

本文档定义 `ESP32-P4X-Function-EV-Board` 在 OpenVela/NuttX 下的
MIPI-CSI 摄像头适配方案，并明确区分两种传感器驱动布局：

1. **板级私有驱动**：用于竞赛期间快速完成 SC2336 真机验证；
2. **NuttX 通用 driver**：用于形成可复用、可上游的摄像头驱动实现。

两种方案只改变 **SC2336 传感器控制驱动的存放位置和构建归属**。
ESP32-P4 的 CSI Host、DMA 和 ISP 始终属于芯片层；P4X 的排线、I²C 总线和
`/dev/video0` 实例装配始终属于板级层。

本文以乐鑫官方验证过的 SC2336 MIPI-CSI 模组为首个目标，初始
profile 为 2 lane、RAW8、1280×720@30fps。实际的 I²C 地址、lane bitrate、
Bayer 顺序和寄存器表必须以实物模组和已点亮的 ESP-IDF 基准工程为准。

## 2. 当前基线与缺口

| 能力 | 当前状态 | 结论 |
| --- | --- | --- |
| P4X USB Console / NSH | 已通过 | 可用于摄像头 Probe 命令与诊断日志 |
| EK79007 `/dev/fb0` | 已通过 | 可作为后续预览输出，不是摄像头采集设备 |
| GT911 `/dev/input0` | 已通过 | 与摄像头 SCCB 共用 I²C0，需保持总线引用计数平衡 |
| PSRAM | 已启用 | 可容纳 LCD 与摄像头双缓冲，但必须预分配并做 cache 同步 |
| NuttX V4L2 upper-half | 已存在 | 可复用 `imgsensor_s`、`imgdata_s` 和 `capture_register()` |
| ESP32-P4 CSI/ISP NuttX adapter | 缺失 | 需在竞赛芯片层新增 |
| SC2336 传感器驱动 | 缺失 | 当前 HAL 工作树中没有可直接使用的 SC2336 驱动 |
| `/dev/video0` | 缺失 | 必须在 sensor 与 CSI `imgdata` 都可用后注册 |

P4 HAL 工作树已存在 `esp_hal_cam/mipi_csi_hal.c`、P4 CSI peripheral
描述和 ISP HAL，但当前 `hal_esp32p4.mk/.cmake` 没有将摄像头源文件加入
构建。因此不能只增加一个 board 开关就得到摄像头能力。

## 3. 总体调用链

```text
csi_probe / nxcamera / Smart Home
                |
                v
        NuttX V4L2 /dev/video0
                |
        +-------+-------+
        |               |
        v               v
 SC2336 imgsensor   P4 CSI imgdata
 SCCB/I2C 控制      CSI/DMA 数据采集
        |               |
        +------ board --+
                |
       P4 CSI PHY/Host/Bridge
                |
          PSRAM 帧缓冲
                |
        P4 ISP / 格式转换
                |
        LVGL / 图像文件 / AI
```

`/dev/video0` 是采集端点，`/dev/fb0` 是显示端点。摄像头 DMA 不应直接
覆盖 LVGL 正在使用的 DSI 扫描页，预览时应使用独立采集缓冲，在帧完成后
再由应用层转换、缩放并提交给 LVGL。

## 4. 共同芯片层：两种方案都必须实现

### 4.1 新增文件

```text
chips/esp32p4/common/espressif/
  esp_mipi_csi.c
  esp_mipi_csi.h
  esp_mipi_csi_imgdata.c
  esp_isp.c                 # ISP 阶段再引入
  esp_isp.h
```

| 文件 | 职责 |
| --- | --- |
| `esp_mipi_csi.c/.h` | D-PHY 供电和 PLL、CSI Host/Bridge、lane 与 datatype、DMA、中断、错误状态和 cache 同步 |
| `esp_mipi_csi_imgdata.c` | 将 CSI 采集封装为 NuttX `struct imgdata_s`，实现 buffer/start/stop/callback |
| `esp_isp.c/.h` | 封装 P4 ISP，完成 RAW Bayer 到 RGB565/YUV 的数据通路，后续再加 AE/AWB |

芯片层不应包含 SC2336 寄存器表、传感器 I²C 地址、P4X 插座接线或
Smart Home 逻辑。

### 4.2 修改构建文件

```text
chips/esp32p4/common/espressif/Kconfig
chips/esp32p4/common/espressif/Make.defs
chips/esp32p4/common/espressif/CMakeLists.txt
chips/esp32p4/hal_esp32p4.mk
chips/esp32p4/hal_esp32p4.cmake
```

建议的芯片层开关：

```text
CONFIG_ESPRESSIF_MIPI_CSI
CONFIG_ESPRESSIF_MIPI_CSI_DMA
CONFIG_ESPRESSIF_MIPI_CSI_IMGDATA
CONFIG_ESPRESSIF_MIPI_CSI_TIMEOUT_MS
CONFIG_ESPRESSIF_ISP
```

首期只有条件地编译已确认必需的低层 HAL：

```text
components/esp_hal_cam/mipi_csi_hal.c
components/esp_hal_cam/esp32p4/mipi_csi_periph.c
```

接入 ISP 时再加：

```text
components/esp_hal_cam/isp_hal.c
components/esp_hal_cam/esp32p4/isp_periph.c
```

首期不直接整包编译 `upper_hal_cam`、`upper_hal_isp` 或 ESP-IDF `esp_video`。
这些组件包含 FreeRTOS/ESP-IDF OS 依赖，应先识别其 HAL 调用和时序，再由
NuttX adapter 接入，避免引入第二套任务、队列和 V4L2 抽象。

## 5. 方案 A：板级私有 SC2336 驱动

### 5.1 定位

方案 A 用于最快完成 P4X + SC2336 真机闭环，不要求修改 NuttX 主仓
`drivers/video` 的 Kconfig 和构建文件。它可以继续使用 NuttX `imgsensor_s` 和 V4L2
API；“源码在 board 下”并不等于“绕过 NuttX 框架”。

### 5.2 文件布局

```text
board/esp32p4/esp32p4-function-ev-board/
  Kconfig
  include/board.h
  src/
    esp32p4_camera.c
    esp32p4_sc2336.c
    esp32p4_sc2336.h
    esp32p4_bringup.c
    Make.defs
    CMakeLists.txt
```

`esp32p4_sc2336.c` 负责：

- SCCB/I²C 寄存器读写；
- 产品 ID 检测；
- 传感器 profile 寄存器表；
- 曝光、增益、翻转和 stream on/off；
- `struct imgsensor_ops_s` 的板级私有实现。

`esp32p4_camera.c` 负责：

- 获取共用 I²C0；
- 构造 SC2336 与 P4 CSI 配置；
- 连接 `imgsensor_s` 和 `imgdata_s`；
- 调用 `capture_register("/dev/video0", ...)`；
- 失败时按相反顺序释放资源。

### 5.3 Kconfig 归属

传感器开关定义在 P4X board Kconfig，不定义成
`CONFIG_ESPRESSIF_SC2336`：

```kconfig
config ESP32P4_FUNCTION_EV_BOARD_CAMERA
	bool "Enable P4X MIPI-CSI camera"
	depends on ESPRESSIF_I2C0 && ESPRESSIF_I2C0_MASTER_MODE
	select ESPRESSIF_MIPI_CSI
	select DRIVERS_VIDEO
	select VIDEO_STREAM

config ESP32P4_FUNCTION_EV_BOARD_CAMERA_SC2336
	bool "Use the P4X SC2336 camera module"
	depends on ESP32P4_FUNCTION_EV_BOARD_CAMERA
	default y
```

### 5.4 优缺点

| 优点 | 限制 |
| --- | --- |
| 不直接修改 NuttX 主仓的 video driver 构建项 | SC2336 与 P4X board 名称和实例绑定 |
| 最适合先做产品 ID 和单帧验证 | 其他板卡复用时需要抽取 |
| 仍可以注册标准 `/dev/video0` | 不适合直接作为上游最终补丁 |
| 调试时文件数量少 | 如不控制边界，容易把 sensor 与 CSI 逻辑写成单一大文件 |

## 6. 方案 B：NuttX 通用 SC2336 driver

### 6.1 定位

方案 B 将 SC2336 建模为与 SoC 和板卡无关的 I²C image sensor。该方案
适合多板卡复用、代码评审与后续上游，是最终建议形态。

### 6.2 竞赛仓内的源码位置

```text
drivers/nuttx/drivers/video/
  sc2336.c
  sc2336.h

scripts/
  link_nuttx_camera_drivers.sh
```

竞赛期间源码仍由竞赛仓管理，脚本只将其映射到当前 NuttX 工作树。
脚本必须具备 `--check` 模式，且不覆盖已存在的非本项目文件。

上游正式补丁对应为：

```text
nuttx/drivers/video/sc2336.c
nuttx/include/nuttx/video/sc2336.h
nuttx/drivers/video/Kconfig
nuttx/drivers/video/Make.defs
nuttx/drivers/video/CMakeLists.txt
```

### 6.3 通用驱动边界

`sc2336.c` 只接收由 board 提供的总线和配置，不主动初始化 ESP32-P4
I²C 控制器，不包含 P4X GPIO 号，不调用 CSI HAL。建议接口形式为：

```c
struct sc2336_config_s
{
  uint8_t  i2c_address;
  uint32_t i2c_frequency;
  uint8_t  lane_num;
  bool     reset_active_high;
  CODE int (*set_reset)(bool asserted);
  CODE int (*set_power)(bool enabled);
};

int sc2336_register(FAR struct i2c_master_s *i2c,
                    FAR const struct sc2336_config_s *config,
                    FAR struct imgsensor_s **sensor);
```

reset/power 回调可为 `NULL`。如官方 P4X 模组没有将 reset、pwdn 或 xclk
引出到 SoC，board 层不得虚构 GPIO 配置。

### 6.4 优缺点

| 优点 | 代价 |
| --- | --- |
| 传感器与 ESP32-P4/P4X 解耦 | 需要完整 Kconfig、Make 和 CMake 集成 |
| 可由其他 I²C + CSI 平台复用 | 公共头文件和生命周期必须稳定 |
| 适合 NuttX 风格评审和上游 | 第一次真机调试速度慢于板级私有方案 |
| 容易扩展曝光、增益和 V4L2 controls | 必须处理多实例、并发和失败回滚 |

## 7. 两种方案共用的 P4X 板级装配

无论选择哪一种 SC2336 布局，都建议保留独立的：

```text
board/esp32p4/esp32p4-function-ev-board/src/esp32p4_camera.c
```

同时修改：

```text
board/esp32p4/esp32p4-function-ev-board/Kconfig
board/esp32p4/esp32p4-function-ev-board/include/board.h
board/esp32p4/esp32p4-function-ev-board/src/Make.defs
board/esp32p4/esp32p4-function-ev-board/src/CMakeLists.txt
board/esp32p4/esp32p4-function-ev-board/src/esp32p4_bringup.c
```

### 7.1 `esp32p4_camera.c` 初始化步骤

```text
1. esp_i2cbus_initialize(0)
2. 读取 SC2336 product ID
3. 创建 imgsensor_s
4. 初始化 P4 CSI imgdata，但不立即开流
5. capture_register("/dev/video0", ...)
6. 应用 open/ioctl/streamon 时才分配帧缓冲并启动传感器
```

只注册 `/dev/video0` 不应立即进行持续 CSI 扫描，否则会在 NSH 启动前长期
占用 PSRAM 带宽和中断资源。

### 7.2 I²C0 共享约束

SC2336 SCCB 与 GT911 共用 GPIO8/SCL 和 GPIO7/SDA，必须遵守：

- 使用现有 `esp_i2cbus_initialize(0)` 引用计数；
- 每个成功 initialize 只能对应一次 uninitialize；
- sensor driver 不重新配置 I²C GPIO；
- 每组 `i2c_msg_s` 携带对应设备频率；
- 首次调试建议 100 kHz，产品 ID 稳定后再评估 400 kHz；
- 一个设备 NACK 时不重置整个总线，避免破坏另一设备的会话。

建议的 board bring-up 关系为：

```text
DSI framebuffer
    -> GT911 注册
    -> SC2336 产品 ID 与 /dev/video0 注册
    -> LittleFS
    -> NSH/应用
```

若摄像头不存在，应记录错误并继续启动 NSH、LCD 和触摸，不得使整机
bring-up 失败。

## 8. 内存、DMA 和 cache 设计

| 格式 | 单帧大小 | 双缓冲 |
| --- | ---: | ---: |
| RAW8 1280×720 | 921,600 B | 1,843,200 B |
| RGB565 1280×720 | 1,843,200 B | 3,686,400 B |
| RGB565 1024×600 LCD | 1,228,800 B | 2,457,600 B |

RAW8 1280×720@30fps 的有效 payload 约为 27.6 MB/s，还未包含 CSI packet 开销、
ISP 读写和 LCD 扫描。实现时必须：

- 在 stream on 前一次性预分配 2～3 个 PSRAM 缓冲；
- 缓冲地址和长度满足 CSI DMA 对齐限制；
- 禁止每帧 `malloc/free`；
- 明确 DMA 写完后 CPU invalidate 和 CPU 写完后 DMA clean 的方向；
- 帧完成中断只更新队列和计数，格式转换放在 worker 上下文；
- 先做单帧 RAW8 CRC，不与 LVGL、ISP 和 AI 同时启用。

## 9. 分阶段实施与验收

### P0：ESP-IDF 硬件基准

在同一块 P4X、同一摄像头、同一条 FPC 上运行乐鑫官方 MIPI-CSI/ISP
示例，记录：

- 模组型号和 board revision；
- I²C 地址与 product ID；
- lane 数和 lane bitrate；
- 输入像素时钟、RAW datatype 和 Bayer 顺序；
- 分辨率、帧率和寄存器 profile。

**通过条件**：ESP-IDF 能稳定采集并显示图像。

### P1：SC2336 SCCB Probe

新增独立 `csi_probe sensor` 模式，不初始化 CSI 和 ISP。

```text
nsh> csi_probe sensor
```

**通过条件**：

- 连续重启 10 次都可读取正确 product ID；
- GT911 同时保持可用；
- 缺少摄像头时在有界时间内返回 `-ENODEV`/`-EIO`，不卡住 bring-up。

### P2：CSI RAW8 单帧与连续帧

```text
nsh> csi_probe raw 10
nsh> csi_probe save /data/frame.raw
```

日志必须包含帧计数、帧字节数、CRC32、CSI ECC/CRC、FIFO overflow、DMA error
和超时阶段。

**通过条件**：10 秒内帧计数持续增加，帧长度稳定，不出现 CSI/DMA
错误；保存的 RAW 帧在主机端可见非全零、非固定值图像。

### P3：NuttX V4L2 `/dev/video0`

使用 `imgsensor_s + imgdata_s + capture_register()` 注册标准采集设备。

```text
nsh> ls /dev/video0
nsh> nxcamera
```

**通过条件**：V4L2 format/buffer/stream ioctls 能完成一次完整
`REQBUFS -> QBUF -> STREAMON -> DQBUF -> STREAMOFF` 周期。

### P4：ISP 和 LCD 预览

将 RAW8 通过 P4 ISP 转换为 RGB565/YUV，再交给独立预览 worker。

**通过条件**：显示方向、色彩、亮度和帧率正常；无 DSI underrun、CSI overflow、
长时间花屏或擕裂。

### P5：Smart Home 业务接入

在采集通路独立通过后再增加：

- 摄像头预览页；
- `capture_camera` 本地工具；
- 单帧保存与上传；
- 视觉模型调用；
- 隐私授权、工具 policy 与采集指示状态。

## 10. `csi_probe` 工程文件

```text
app/csi_probe/
  Kconfig
  Make.defs
  CMakeLists.txt
  Makefile
  csi_probe_main.c
  README.md

board/esp32p4/esp32p4-function-ev-board/configs/csi_probe/
  defconfig
```

新配置必须与 `dsi_probe`、`fb_probe` 和 `smart_home` 分开，不在首次 CSI
验证中引入 LVGL、网络、cAGENT、MCP 或模型 API。现有 `configs/capture`
是 MCPWM capture 配置，不是摄像头配置，不应复用其名称。

## 11. 方案 A 向方案 B 的迁移

推荐实施策略是“A 完成真机闭环，B 完成正式收敛”：

1. 首先以 `esp32p4_sc2336.c/.h` 完成 P1～P3；
2. 在板级私有版本中保持严格边界，不让它访问 CSI/ISP 寄存器；
3. 将稳定的 sensor 源码移至 `drivers/nuttx/drivers/video/sc2336.c/.h`；
4. 将 P4X 常量改为 `sc2336_config_s` 参数或 board callback；
5. 保留 `esp32p4_camera.c` 作为板级装配，只替换 include 和 register API；
6. 补齐 NuttX Kconfig/Make/CMake 补丁和驱动文档；
7. 执行 P1～P4 全部回归，确认抽取没有改变寄存器序列和时序。

迁移时不重写寄存器表，只调整文件归属、公共类型、实例生命周期与
构建规则，从而降低回归风险。

## 12. 建议的实施决策

本项目当前处于 P4X 外设持续扩展阶段，建议选择：

```text
第一阶段：方案 A，板级私有 SC2336，快速建立硬件与 CSI 真机基线
第二阶段：保持芯片层与 V4L2 接口不变，抽取为方案 B 通用 driver
第三阶段：接入 ISP、LVGL 预览和 Smart Home 视觉工具
```

不推荐把 `sc2336.c` 放入 `chips/esp32p4/common/espressif`:
这会将外置传感器错误地建模为 SoC 内部外设，并使后续 OV5647 或其他
传感器接入变成芯片层修改。

## 13. 参考资料

- [ESP32-P4 Function EV Board 用户指南](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32p4/esp32-p4-function-ev-board/user_guide.html)
- [ESP-IDF MIPI CSI → ISP → DSI 示例](https://github.com/espressif/esp-idf/blob/master/examples/peripherals/camera/mipi_isp_dsi/README.md)
- [ESP-IoT-Solution Camera LCD Display 示例](https://github.com/espressif/esp-iot-solution/blob/master/examples/camera/video_lcd_display/README.md)
- [ESP Video Components 文档](https://docs.espressif.com/projects/esp-video-components/en/latest/esp32p4/Get_Started/index.html)
- [NuttX `imgsensor_s`](https://github.com/apache/nuttx/blob/master/include/nuttx/video/imgsensor.h)
- [NuttX `imgdata_s`](https://github.com/apache/nuttx/blob/master/include/nuttx/video/imgdata.h)
- [NuttX V4L2 capture upper-half](https://github.com/apache/nuttx/blob/master/drivers/video/v4l2_cap.c)
