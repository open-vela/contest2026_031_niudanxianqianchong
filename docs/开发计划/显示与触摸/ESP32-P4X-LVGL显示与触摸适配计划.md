# ESP32-P4X Function EV Board：LVGL 显示与触摸适配计划

## 1. 目标与边界

本计划为 `ESP32-P4X-Function-EV-Board` 增加可验证的本机图形显示和触摸
能力，最终使 LVGL 应用能够在官方 7 英寸屏上显示，并接收触摸坐标。

目标链路：

```text
ESP32-P4 MIPI-DSI host
  -> EK79007 面板驱动
  -> framebuffer / 显示设备
  -> LVGL display port

I2C master
  -> GT911 触摸驱动
  -> /dev/inputX
  -> LVGL input device
```

本计划不包含业务 UI、摄像头、音频或网络应用集成。它们必须在显示、触摸的
最小独立验证完成后再进入下一阶段。

## 2. 已确认的硬件基线

| 项目 | 结论 |
| --- | --- |
| 开发板 | ESP32-P4X-Function-EV-Board |
| 芯片修订版 | ESP32-P4 revision v3.1 及以上；当前实板识别为 v3.2 |
| 当前配置 | `CONFIG_ESP32P4_REV_MIN_301=y`，满足 P4X 修订版要求 |
| LCD 模组 | AML070JGI50-07403L，7 英寸，1024 x 600 |
| LCD 控制器 | EK79007AD + EK73217BCGA |
| 显示接口 | MIPI-DSI |
| P4 DSI 能力 | 1 个 Host，最多 2 条 data lane；P4X command probe 固定 2 lane、1000 Mbps |
| D-PHY 供电 | P4X 参考配置使用内部 LDO channel 3、2.5 V；视频上电顺序仍以实板复核为准 |
| 触摸控制器 | GT911，I2C 接口 |
| GT911 总线 | I2C0，SCL=GPIO8、SDA=GPIO7，400 kHz，默认地址 0x5d |
| GT911 RST / INT | 官方 P4X adapter 标为 `GPIO_NUM_NC`；首版使用 20 ms 轮询，不控制这两根线 |
| 面板复位 | 主板 GPIO27 -> LCD Adapter `RST_LCD` |
| 背光 PWM | 主板 GPIO26 -> LCD Adapter `PWM` |

LCD 是配套可选组件。开始软件适配前，必须确认 LCD adapter 已使用反向 FPC
线缆接至主板 MIPI-DSI 接口，并完成 GPIO27、GPIO26、5V 与 GND 接线。DSI
D-PHY 的 2.5 V 供电必须按参考设计配置；不得在未核对原理图前假定使用某一
固定 LDO VO 通道，否则面板可能保持黑屏。

## 3. 当前代码状态与缺口

当前仓库已有 `drivers/video/mipidsi/` 通用 MIPI-DSI 协议框架，包括 host 和
device 的抽象、DCS 命令封装以及设备注册接口。

当前仓库的关键组件状态如下。“命令写已实板验证”只覆盖 D-PHY/Host、板级
reset 与 DCS 写命令；**不**表示完成 P4X video、画面、触摸或 LVGL 验证。

| 组件 | 当前状态 | 影响 |
| --- | --- | --- |
| ESP32-P4 MIPI-DSI command Host | 命令写已实板验证 | P4 rev3 XTAL reference clock、D-PHY PLL、lane stop、命令模式控制器和 NuttX Host 已通过；generic packet 与 EK79007 初始化写序列通过。 |
| P4 DSI/LDO 构建与封装 | Make 构建及实板验证通过 | 已形成 NuttX errno 风格 LDO 封装，并在 Make/CMake 中条件纳入 DSI vendor HAL；CMake 回归仍待执行。 |
| ESP32-P4 MIPI-DSI video pipeline | M2b/M2c 已实板运行但未显示 | M2b RGB888、M2c RGB565 DPI Panel + `draw_bitmap()` 的 DMA 帧计数均可递增，屏幕仍黑。当前状态是 Host/DMA 软件路径通过、视觉显示未通过；与同硬件 ESP-IDF 可显示工程的逐项差异及收敛顺序见 [ESP-IDF LCD 参考实现对比与收敛计划](ESP32-P4X-ESP-IDF-LCD参考实现对比与收敛计划.md)。 |
| EK79007 通用面板驱动 | 已接入 M2c 验证链路，待视觉验收 | 驱动已在 DCS/sleep-out 后创建并启动可选 DPI panel；尚未注册 `/dev/fb0` 或连接 LVGL。 |
| GT911 通用触摸驱动 | P3.1 单指真机验收通过 | 已接入 touchscreen upper-half，`gt911_probe` 可读取稳定的 `DOWN/MOVE/UP`、坐标和 size；多点待验收。 |
| P4X 板级 DSI command 装配 | 命令写已实板验证；M2c 待验收 | `esp32p4_lcd.c` 已实测 LDO3/2.5V、2 lane/1000 Mbps 与 GPIO27 reset；M2c 固化官方 EK79007 RGB565 timing profile 与 GPIO26 静态背光。 |
| P4X 板级触摸装配 | P3.1 真机验收通过 | `esp32p4_touch.c` 以 I2C0、0x5d、400 kHz、20 ms 轮询注册 `/dev/input0`。 |
| `lvgl` defconfig | 未实现 | 没有可复现的显示、触摸与 LVGL 配置组合。 |

### 3.1 本次已落地的 M1 芯片层

新增的 `esp_ldo.c/.h` 与 `esp_mipi_dsi.c/.h` 已完成以下最小闭环：

1. `esp_ldo_*()` 将 ESP HAL LDO channel 的申请、释放和可调电压操作转换为
   NuttX 负 errno；ESP HAL handle 不暴露给板级或面板驱动。
2. `esp_mipi_dsi_host_initialize()` 只接受 P4 的 bus 0、1/2 条 lane、80--1500
   Mbps lane rate、5--40 MHz PHY 参考时钟，以及 2.5 V D-PHY LDO 配置。
3. 初始化路径依次申请 LDO、选择 P4 rev3 的 PHY clock source、开启/复位 DSI
   时钟、初始化 vendor HAL、配置 PHY PLL、等待 PLL lock 与 lane stop、切入
   command mode，最后注册 NuttX `mipi_dsi_host`。
4. `transfer()` 复用 NuttX packet 编码，支持 short/long packet 和带 Maximum
   Return Packet Size 的读回；命令/读写 FIFO 与 PLL 等待都使用受限轮询和超时，
   不含无界 busy-wait。
5. Host、attach 与 transfer 使用同一互斥锁串行化。由于 NuttX 当前没有 Host
   unregister API，Host 结构为静态对象；`shutdown()` 仅关闭硬件和释放 LDO，
   之后可由下一次 initialize 重新启用同一 Host。
6. `ESPRESSIF_MIPI_DSI` 同时选择 `MIPI_DSI` 与 `ESPRESSIF_LDO`；Make 和 CMake
   入口均条件编译芯片层代码，并条件加入 `mipi_dsi_hal.c`、
   `mipi_dsi_periph.c`。

当前 M1/M2b **尚未**实现 DSI 错误中断、显式 `FAULT` 状态机、vsync、通用
framebuffer/LVGL port，也未完成重复初始化压力测试或能返回 payload 的 DCS read
验证；这些项目不能被一次 command-write 或 DMA 启动通过替代。

现有 `esp32p4_buttons.c` 中的 `CONFIG_ESPRESSIF_TOUCH` 是芯片内部触摸
传感器（touch-pad）支持，**不是** LCD 上 GT911 电容触摸屏驱动。

## 4. 分层设计与文件归属

### 4.1 归属原则

通用协议不得复制到单一板级目录。EK79007 和 GT911 未来可能被其他板卡复用，
应以 NuttX 通用驱动形式实现；板级代码只描述 P4X 的电源、引脚、总线和设备
装配关系。

```text
应用 / LVGL
    │
    ├─ /dev/fb0（或等价显示设备）
    └─ /dev/inputX
             │
NuttX 通用驱动层
    ├─ EK79007 MIPI-DSI 面板
    └─ GT911 I2C 触摸
             │
ESP32-P4 芯片层
    ├─ MIPI-DSI command Host、I2C
    └─ MIPI-DSI video/DMA/中断（在 command Host 通过后实施）
             │
P4X 板级装配层
    ├─ LDO、电源、GPIO27 Reset、GPIO26 PWM
    └─ I2C bus、设备地址、屏幕方向
```

### 4.2 计划修改的文件

| 层级 | 计划文件 | 职责 |
| --- | --- | --- |
| P4 芯片层 | `chips/esp32p4/common/espressif/esp_ldo.c/.h` | 将 P4 vendor LDO 生命周期封装为 NuttX 风格接口 |
| P4 芯片层 | `chips/esp32p4/common/espressif/esp_mipi_dsi.c/.h`、`esp_mipi_dsi_dpi_panel.c/.h` | MIPI-DSI Host、PHY、DCS、GDMA scanout，以及 M2c 的 DPI panel/整帧 `draw_bitmap()` 生命周期 |
| P4 芯片层 | `chips/esp32p4/common/espressif/Kconfig`、`Make.defs`、`CMakeLists.txt` | 建立 MIPI-DSI host 配置与构建入口 |
| P4 HAL 构建 | `chips/esp32p4/hal_esp32p4.{mk,cmake}` | 条件加入 MIPI-DSI 与 video 所需 vendor HAL 源，保持双入口一致 |
| 竞赛驱动覆盖层 | `drivers/nuttx/drivers/lcd/{ek79007.c,ek79007.h}` | EK79007 DCS 初始化、可选 DPI panel 生命周期、整帧提交、休眠与恢复 |
| 竞赛驱动覆盖层 | `drivers/nuttx/drivers/input/{gt911.c,gt911.h}` | GT911 I2C 寄存器访问、触点解析、输入事件上报 |
| NuttX 工作树映射 | `nuttx/drivers/{lcd,input}/` | 由 `scripts/link_nuttx_display_drivers.sh` 创建相对软链接，供 NuttX 正常构建 |
| NuttX 构建项 | 对应 `drivers/*/{Kconfig,Make.defs,CMakeLists.txt}` | 注册通用面板和输入驱动 |
| P4X 板级层 | `board/esp32p4/esp32p4-function-ev-board/src/esp32p4_lcd.c` | M1：LDO、reset、DSI Host；M2c：官方 RGB565 DPI timing、GPIO26 静态背光；M3 再注册显示设备 |
| P4X 板级层 | `board/esp32p4/esp32p4-function-ev-board/src/esp32p4_touch.c` | I2C0 获取与 GT911 轮询注册；RST/INT 未接 SoC，不在板级伪造回调 |
| P4X 板级层 | `src/esp32p4-function-ev-board.h` | 板级初始化接口、GPIO 常量 |
| P4X 板级层 | `src/esp32p4_bringup.c` | 按 Kconfig 调用显示和触摸初始化 |
| P4X 板级层 | `src/{Make.defs,CMakeLists.txt}`、`Kconfig` | 加入板级源文件和开关 |
| P4X 配置 | `configs/lvgl/defconfig`（新增） | 固化 USB console、DSI、LCD、GT911、LVGL 配置 |
| P4X 配置 | `configs/dsi_probe/defconfig`、`app/dsi_probe/`（新增） | 在 LVGL 前独立验证 DSI command Host |

> 注：EK79007 与 GT911 的规范源码当前保存在竞赛目录的 `drivers/nuttx/`，并以
> 相对软链接映射至 NuttX 工作树。该脚本不修改 Kconfig、Make.defs、CMakeLists；
> 构建入口将在驱动 API 稳定后单独接入。若仅为短期原型，也不得在 board `src/`
> 中复制一套无法复用的 GT911/EK79007 协议实现。

## 5. 实施阶段与验收门

### P0：硬件和资料冻结

1. 确认屏幕模组标签为 AML070JGI50-07403L，拍照存档。
2. 核对 LCD adapter 与 P4X 的反向 FPC、GPIO27、GPIO26、5V、GND 接线。
3. 已从 P4X 参考设计确认 GT911 使用 I2C0、GPIO8/7、默认地址 0x5d；`RST/INT`
   为 `GPIO_NUM_NC`。首版仅使用轮询，后续只有确认外接可控线路时才增加中断。
4. 核对 D-PHY 2.5 V 所使用的实际 LDO 通道、所需电压和上电顺序。
5. 核对面板支持的 lane 数（只能选 1/2 lane）、lane bit rate、像素格式和完整
   video timing；不得按 FPC 引脚数推断 P4 可用 lane 数。

通过标准：硬件连接表与可引用的原理图页码齐全；屏幕供电、复位和背光线路
可用。

### P1：ESP32-P4 MIPI-DSI command Host 最小验证（代码完成，验收待执行）

1. 已完成 LDO、host 初始化、时钟/复位、PHY PLL、command transport 和
   `mipi_dsi_host_register()` 接线；不在本阶段引入 framebuffer/DMA 视频扫描。
2. 已新增 `dsi_probe` 独立 defconfig 与测试命令，不引入 LVGL、面板 framebuffer
   或网络业务。
3. 验证 Host 能创建 DSI device，发送 generic short/long packet、读取 DCS power
   mode 响应并获得明确
   的 LDO/PLL/transfer errno 日志。
4. 在 M1 实板闭环后，补充 DSI error IRQ、首错记录与 `FAULT -> shutdown ->
   initialize` 恢复策略；不得把这项工作放入 M2 后再回补。

通过标准：DSI host 注册成功；失败路径可返回具体 errno；无时钟、PHY、传输或
中断异常。

P1 构建与操作命令：

```bash
# 使新 app/dsi_probe 的 manifest linkfile 生效
repo sync contest2026_031_niudanxianqianchong

./build.sh \
  contest2026_031_niudanxianqianchong/board/esp32p4/esp32p4-function-ev-board/configs/dsi_probe \
  -j2
```

烧录后在 USB console 运行 `dsi_probe`。通过证据必须包含 Host 初始化、两条
generic packet accepted、DCS power mode 和最终 `PASS`；若面板未接或链路异常，
保留对应步骤的负 errno 日志，不得将失败结果误记为显示失败。

### P2：ESP32-P4 MIPI-DSI video pipeline 最小验证

1. 在 command Host 已通过的基础上增加 DPI video、完整 video timing、DMA、
   cache 同步与 vsync/error 中断。
2. 以 RGB565 单 framebuffer 输出纯色或色条；DMA 描述符放置和 PSRAM 可访问性
   必须以实测为准。
3. 将 video 状态机与 command transfer 并发规则写入芯片层实现，避免 ISR 直接
   调用面板或 LVGL。

通过标准：稳定扫描 1024 x 600 测试画面；连续启动十次不花屏、不 DMA abort，
异常状态可完整停止并恢复。

### P3：EK79007 面板最小显示

1. 新增通用 EK79007 面板驱动，使用 P4X 确认过的初始化序列和 1024 x 600
   时序；寄存器常量和时序必须以芯片资料为准。
2. 在 `esp32p4_lcd.c` 中完成 LDO、GPIO27 复位、GPIO26 PWM 背光及 panel
   实例装配。
3. 建立 framebuffer 或等价的 NuttX 显示设备注册路径。
4. 先显示纯色、色条或静态测试图，不引入字体、复杂布局或网络。

通过标准：屏幕完成 reset、背光可控，稳定显示 1024 x 600 测试画面；连续重启
十次不出现黑屏、花屏或内存泄漏。

### P4：GT911 触摸最小验证

1. 已接入独立 `CONFIG_INPUT_GT911`，而非复用旧 VFS 型 `gt9xx` 驱动；当前实现通过
   NuttX touchscreen upper-half 上报多点事件。
2. `esp32p4_touch.c` 固定提供 P4X I2C0、0x5d、400 kHz 和 20 ms 轮询配置。
3. 新增 `gt911_probe`，先验证产品 ID、`/dev/input0`、单指/多指及抬起事件。
4. 仅在原始坐标与方向验收后，才为 LVGL 指定 `/dev/input0` 并做点击验证。

通过标准：`/dev/inputX` 注册成功；单指、多指、抬起事件可重复读取；坐标范围
与 1024 x 600 面板一致。

### P5：LVGL 最小应用

1. 新增 `configs/lvgl/defconfig`，基于已验证的 `usbconsole`，保留
   `/dev/ttyACM0` 作为故障诊断通道。
2. 启用 framebuffer、输入、LVGL 及最小 LVGL 示例。
3. 先显示标签、按钮和触摸坐标；验证触摸点击可改变标签或背景色。
4. 将 LVGL 绘制和输入处理置于独立任务，避免在中断上下文调用 LVGL。

通过标准：冷启动进入 LVGL 画面；按钮可被触摸点击；串口日志可报告显示/触摸
初始化状态；故障时仍可从 USB console 进入 NSH。

### P6：压力、恢复与上游准备

1. 验证背光开关、面板休眠/唤醒、连续重启和异常恢复。
2. 检查 DMA buffer 对齐、PSRAM 可访问性和 framebuffer 生命周期。
3. 对新增 C 文件执行 `nuttx/tools/checkpatch.sh -f`，完成 Kconfig、Make 和
   CMake 双构建入口检查。
4. 将通用 DSI host、EK79007、GT911 和 P4X board glue 拆分为独立提交，为
   后续上游贡献保留清晰历史。

通过标准：无编译警告；`git diff --check` 通过；实板有启动、显示、触摸、重启
的完整证据。

## 6. 配置与内存预算

最低建议使用 RGB565。1024 x 600 的 framebuffer 占用如下：

| 格式 | 单缓冲 | 双缓冲 | 建议 |
| --- | ---: | ---: | --- |
| RGB565 | 1,228,800 B（约 1.17 MiB） | 2,457,600 B（约 2.34 MiB） | 首期默认选择 |
| RGB888 | 1,843,200 B（约 1.76 MiB） | 3,686,400 B（约 3.52 MiB） | 仅在带宽和 PSRAM 验证后启用 |

首期采用 RGB565 单缓冲，优先将 framebuffer 放入满足 P4 DSI DMA 约束的内存
区域。是否可直接使用 PSRAM 必须以 P4 DSI DMA 实测为准；若 DMA 不支持或存在
对齐限制，应使用内部 SRAM 描述符/行缓冲加 PSRAM 图像缓冲的分层方案，而不是
假设 PSRAM 一定可直接扫描输出。

建议的关键 Kconfig 类别：

```text
CONFIG_MIPI_DSI=y                        # 由 ESPRESSIF_MIPI_DSI 自动选择
CONFIG_ESPRESSIF_LDO=y                   # 由 ESPRESSIF_MIPI_DSI 自动选择
CONFIG_ESPRESSIF_MIPI_DSI=y              # M1 已实现，默认关闭
CONFIG_ESPRESSIF_MIPI_DSI_TIMEOUT_MS=100 # M1 已实现，范围 1--1000
CONFIG_ESPRESSIF_MIPI_DSI_VIDEO=y       # 计划新增，依赖 command Host
CONFIG_LCD_EK79007=y                    # 计划新增
CONFIG_INPUT_GT911=y                    # 计划新增
CONFIG_I2C=y
CONFIG_FB=y 或等价显示接口
CONFIG_LVGL=y
CONFIG_ESPRESSIF_LEDC=y                 # GPIO26 背光 PWM
CONFIG_ESPRESSIF_USBSERIAL=y            # 保留 USB console
```

实际 symbol 命名必须遵循当前 NuttX 对应子系统的既有 Kconfig 风格，不以本节
示例为最终接口定义。

## 7. 风险与控制措施

| 风险 | 控制措施 |
| --- | --- |
| 将 P4X 当作旧 P4 板处理 | 固定 revision >= 3.1；每次构建检查 revision Kconfig |
| LCD 未供电或未接 GPIO27/GPIO26 | 在软件排障前先完成硬件接线检查表 |
| 无 P4 DSI host 却直接写面板代码 | P1、P2 必须依序独立通过，P3 不得跳过底层验证 |
| 将 command DCS 误认为已支持视频显示 | P1/P2 分离验收；P2 前不注册 `/dev/fb0` |
| P4 DSI lane 与面板模式不匹配 | 固定 P4 最多 2 lane，P0 按面板资料确认实际模式 |
| D-PHY 2.5 V 或 LDO 通道错误 | 以 P4X 原理图确认通道和上电顺序，启动日志记录结果 |
| ESP-IDF 代码与 NuttX 模型混用 | ESP-IDF 仅用于寄存器/时序参考；NuttX 使用其 MIPI、LCD、input 框架 |
| framebuffer 超出 SRAM 或 DMA 不可访问 | 首期 RGB565 单缓冲；记录 DMA 可访问内存和对齐要求 |
| 触摸坐标方向错误 | P4 独立打印坐标并完成旋转/镜像校准后再接 LVGL |
| LVGL 在 ISR 或 bring-up 中阻塞 | 触摸 ISR 只采样/唤醒，LVGL 仅在任务上下文运行 |

## 8. 提交与测试策略

建议按以下顺序提交，避免将仍不可显示的 UI 变更和底层驱动混在一起：

```text
feat(esp32p4): 新增 LDO 与 MIPI-DSI command Host 支持
build(esp32p4): 接入 MIPI-DSI vendor HAL 与双构建入口
test(esp32p4x): 新增 dsi_probe command Host 验证配置
feat(esp32p4): 新增 MIPI-DSI video、DMA 与 framebuffer 管理
feat(lcd): 新增 EK79007 MIPI-DSI 面板驱动
feat(input): 新增 GT911 I2C 触摸驱动
feat(esp32p4x): 装配 LCD 与触摸设备
config(esp32p4x): 新增 LVGL 显示验证配置
test(esp32p4x): 补充显示与触摸实板测试证据
```

每个阶段至少保留以下证据：构建命令与成功末尾、`git diff --check`、USB console
日志、对应硬件现象或截图。P4X 的 Simple Boot 镜像仍应按已验证规则写入
`0x2000`，直到构建系统的正式 flash offset 修复完成。

ESP32-P4 MIPI-DSI Host 的状态机、接口约束、DMA/video 分期和验收矩阵见
[ESP32-P4 MIPI-DSI Host 设计与实施方案](ESP32-P4-MIPI-DSI-Host设计与实施方案.md)。

## 9. 驱动覆盖层与软链接约定

竞赛目录是显示、触摸驱动的唯一规范源码位置：

```text
drivers/nuttx/drivers/lcd/ek79007.c
drivers/nuttx/drivers/lcd/ek79007.h
drivers/nuttx/drivers/input/gt911.c
drivers/nuttx/drivers/input/gt911.h
```

执行以下命令创建或修复 NuttX 工作树链接：

```bash
contest2026_031_niudanxianqianchong/scripts/link_nuttx_display_drivers.sh
contest2026_031_niudanxianqianchong/scripts/link_nuttx_display_drivers.sh --check
```

脚本仅删除它自身旧版本创建、且目标完全匹配的两条 EK79007 悬空链接；遇到其他
普通文件或指向未知位置的软链接会拒绝覆盖。这样可以在目录重构后保持安全，且
避免把 NuttX 构建规则悄然改成不可追踪状态。
