# ESP32-P4 竞赛参考差异对照：345 芯片层与 430 显示链路

## 1. 目的与结论

本文对照两个公开竞赛成果与本项目当前实现，目的是为 ESP32-P4X
Function EV Board 的后续移植建立**可选择性吸收**的依据，而不是把外部
代码整体合入。

| 对照对象 | 主要价值 | 对本项目的定位 |
|---|---|---|
| 345 队 / open-vela NuttX PR #340 | P4 SoC 启动、时钟、中断、UART、GPIO、I2C 等基础移植与真机证据 | 芯片层基线与回归测试参考 |
| 430 队 / open-vela NuttX PR #347 | P4X 板级 DSI、EK79007、GT911、LVGL、音频和相机的装配结构 | 显示/触摸的 NuttX 分层参考 |
| 本项目 | out-of-tree P4 芯片层、标准 MIPI-DSI Host、EK79007 通用面板层、DW-GDMA DPI probe | 继续定位真实黑屏并最终形成可上游化实现 |

截至本文编写时，两个 PR 均为公开的 Open 状态。345 的 P4 rev3.2
真机启动、GPIO loopback、BOOT 中断、I2C 等证据可作为已验证基础；430
的 PR 已覆盖 DSI/LVGL 代码和全量构建，但其 PR 说明仍把 EK79007 真机时序
调优列为待验证事项。因此，430 不能作为“已验证点屏”的唯一依据。

**总策略：**

```text
P4 启动 / IRQ / 外设基础 ──────> 345 / PR #340
P4X 板级组织、GT911、FB/LVGL ──> 430 / PR #347
真实屏幕的点屏时序与像素输出 ──> 本地 ~/Project/espidf_lcd
                                      + Espressif 官方 BSP
```

本项目不能直接用任一竞赛仓库替换当前实现：它们的 NuttX 基线、Kconfig
符号、HAL 版本和目标边界均不同。

## 2. 对照基线与方法

### 2.1 固定的参考版本

| 名称 | 固定版本 / 文件根 | 用途 |
|---|---|---|
| 345 芯片层 | PR #340 head `6ac30d6cf0fdb1e465539f59c67b398b7635be18` | `arch/risc-v/src/esp32p4/` |
| 430 显示链路 | PR #347 head `ac12f091b62c0297af857c014fbd48b4de73d0c0` | `boards/risc-v/esp32p4/esp32p4-function-ev-board/` |
| 本项目 P4 芯片层 | `chips/esp32p4/` | 当前 out-of-tree 芯片层 |
| 本项目 P4X 显示链路 | `board/esp32p4/esp32p4-function-ev-board/`、`drivers/nuttx/drivers/lcd/ek79007.c` | 当前 Host、面板与 probe |

外部来源：

- [345 队仓库](https://github.com/open-vela/contest2026_345_daxueshiyoushigeiyincangsinianwoquehunranbuzhi)
- [ESP32-P4 SoC PR #340](https://github.com/open-vela/nuttx/pull/340)
- [430 队仓库](https://github.com/open-vela/contest2026_430_zuoyeyushufengzhou)
- [P4X Board PR #347](https://github.com/open-vela/nuttx/pull/347)
- [乐鑫 EK79007 组件说明](https://components.espressif.com/components/espressif/esp_lcd_ek79007/versions/2.0.2~1/readme)

### 2.2 判定原则

1. **真机证据优先于“已编译”声明。** 只有已记录板型、芯片 revision、
   操作命令和串口输出的部分，才列为可直接验证的参考。
2. **接口边界优先于源代码相似度。** 不能因两个实现都使用 MIPI-DSI
   HAL 就混用 Kconfig、私有结构或寄存器辅助函数。
3. **显示必须分成命令链路与像素链路。** DCS 写入成功、DMA frame 计数
   递增，不等价于面板已经接收到正确的 DPI 像素流。
4. 本文不建议直接 cherry-pick 外部竞赛提交；每个候选项都必须先做最小
   diff、独立构建、真机回归，再进入本项目。

## 3. 345 芯片层与本项目 `chips/esp32p4` 的差异清单

### 3.1 总体架构

| 项目 | 345 / PR #340 | 本项目 | 结论 |
|---|---|---|---|
| 放置位置 | NuttX in-tree：`arch/risc-v/src/esp32p4/` | out-of-tree：`chips/esp32p4/`，由 custom chip 映射接入 | 保持本项目 out-of-tree 边界，不整体迁移目录 |
| 通用 Espressif 层 | 复用 `arch/risc-v/src/common/espressif/` | 本地 `chips/esp32p4/common/espressif/` 提供较完整的 HAL 适配 | 当前层次更适合比赛仓独立迭代 |
| P4 专属文件 | `esp_chip_rev.c`、MIPI-CSI、I2S、MIPI-DSI | `esp_chip_rev.c` + 通用 ESP 外设实现 + LDO/DSI/DPI adapter | 两者有功能重叠，但接口并不兼容 |
| 构建入口 | `Kconfig`、`Make.defs`、`hal_esp32p4.mk` | 同时具备 `Kconfig`、`Make.defs`、`CMakeLists.txt`、`hal_esp32p4.mk/.cmake` | 本项目 CMake 覆盖更完整，保留 |

### 3.2 已验证底座：应优先核对，而非复制

| 子系统 | 345 的价值 | 当前项目状态 | 建议动作 |
|---|---|---|---|
| revision / linker / ROM ld | PR #340 针对 P4 3.x 选择对应 ROM 链接脚本 | 已支持 `ESP32P4_REV_MIN_301`，并已能烧录进入 NSH | 逐项核对 revision symbol、ROM ld 选择和 MSPI workaround；不替换现有 linker 流程 |
| 启动、UART、GPIO、IRQ | 有 P4 rev3.2 真机 NSH、GPIO loopback、BOOT IRQ 证据 | 本项目 USB console / NSH 已验证 | 将 345 的测试命令和日志格式纳入回归，不需要迁移其实现 |
| I2C | 有真机 I2C / ES8311 smoke 证据 | 已有通用 `esp_i2c.c`，后续 GT911/AHT 等会依赖 | 仅核对时钟、pin matrix 和错误恢复路径；以本项目 HAL API 为准 |
| 定时器 / 时钟树 | 是 P4 最小系统稳定性的关键基础 | 已有 timer/oneshot/tickless 等通用实现 | 作为黑屏排查的前置健康检查，避免把启动/时钟问题归因到 DSI |

### 3.3 MIPI-DSI 的接口分层差异

| 维度 | 345 / PR #340 | 本项目 | 影响 |
|---|---|---|---|
| Kconfig 主符号 | `CONFIG_ESP32P4_MIPI_DSI` | `CONFIG_ESPRESSIF_MIPI_DSI` | 两套 defconfig 不可直接复制 |
| 视频接口 | 单例式 `esp32p4_mipi_dsi_initialize()`、`set_framebuffer()`、`start_video()` | `esp_mipi_dsi_host_initialize()` + 标准 `mipi_dsi_host` + `esp_mipi_dsi_dpi_panel_*()` | 本项目更适合接通用面板驱动和未来多板级复用 |
| NuttX MIPI 框架 | 以芯片层私有 HAL/FB 为主 | Host 实现 `attach/detach/transfer`，上接 `mipi_dsi_device` | 本项目已采用更清晰的 Host/Panel 边界 |
| framebuffer | 芯片层直接持有 `fb_vtable_s`、分配/绑定 framebuffer | 当前 DPI adapter 持有 DMA scanout；尚未注册 `/dev/fb0` | 后续 LVGL 需增加独立 framebuffer glue，不应把现有 probe 直接当 FB 驱动 |
| DMA | `start_video()` 内部仅启用 video / bridge，具体数据供给路径仍需审阅 | DW-GDMA、PSRAM buffer、cache sync、错误/underrun 观测已显式实现 | 当前更适合继续做像素流诊断 |
| LDO | DSI 初始化中使用 P4 D-PHY 所需配置 | 独立 `esp_ldo.c/.h`，在 Host 初始化中申请 LDO3 2.5V | 保持独立 LDO 抽象，便于失败回收 |

### 3.4 345 中不应直接吸收的内容

1. 不将 `esp32p4_mipi_dsi.c` 整体复制到当前 `esp_mipi_dsi.c`。两者分别
   使用私有 framebuffer API 与标准 `mipi_dsi_host` API，合并会破坏既有
   panel 生命周期。
2. 不将 `CONFIG_ESP32P4_MIPI_DSI_*` 直接写入本项目 defconfig。它们与
   `CONFIG_ESPRESSIF_MIPI_DSI_*` 不同，混用会导致“配置看似开启但实际不
   编译”的问题。
3. PR #340 的 MIPI-DSI 是 SoC 初始支持的一部分；345 的真机强证据主要
   集中在启动、GPIO/IRQ、I2C 等，不能据此推导 DSI 已完成真实点屏。

### 3.5 345 对本项目的可执行吸收项

| 优先级 | 项目 | 验收方式 |
|---|---|---|
| P0 | 对齐 P4 rev3.2 的 Kconfig、ROM linker script 与 flash image 参数 | `nsh` 冷启动、连续复位、USB console |
| P0 | 将 GPIO loopback、BOOT IRQ、I2C scan/设备读写纳入板级回归 | 每次改 DSI/HAL 后分别运行 |
| P1 | 逐项核对 P4 clock/IRQ/DMA 引用的 HAL 文件及条件编译 | 构建日志中无缺符号、真机无异常复位 |
| P2 | 评估 PR #340 的 MIPI-CSI/I2S 独立模块 | 仅在启动摄像头/音频功能时进行，不阻塞 LCD 点屏 |

## 4. 430 的 EK79007 / DSI / LVGL 结构对照

### 4.1 430 的实现分层

PR #347 的显示路径可概括为：

```text
board Kconfig: ESP32P4_FUNCTION_EV_LCD
        │
        ├── esp32p4_display.c
        │     ├── PSRAM RGB565 framebuffer
        │     ├── NuttX fb_vtable_s / up_fbinitialize()
        │     ├── configure DPI / bind framebuffer / start video
        │     └── GPIO26 backlight
        │
        ├── esp32p4_ek79007.c
        │     ├── mipi_dsi_device_register + attach
        │     ├── GPIO27 low-active reset
        │     └── B2、80..86、Sleep Out DCS 序列
        │
        └── esp32p4_touch.c
              └── GT911 I2C0 polling，地址 0x5d
```

该 PR 的 LVGL defconfig 启用 `GRAPHICS_LVGL`、`LV_USE_NUTTX_FBDEV`、
`LV_USE_NUTTX_TOUCHSCREEN` 与 `LV_COLOR_DEPTH=16`，也即 LVGL 通过 `/dev/fb0`
和 NuttX touchscreen 路径工作。

### 4.2 与本项目的逐项差异

| 层次 | 430 / PR #347 | 本项目当前实现 | 判断 |
|---|---|---|---|
| DSI Host | 使用 `esp_mipi_dsi_host_get()` 等其 fork 的私有 API | `esp_mipi_dsi_host_initialize()`，实现标准 `mipi_dsi_host` | 不兼容，不能直接搬函数调用 |
| EK79007 驱动位置 | `boards/.../src/esp32p4_ek79007.c`，板级私有 | `drivers/nuttx/drivers/lcd/ek79007.c`，可配置的通用面板层 | 本项目的层次更符合可复用驱动目标，应保留 |
| EK DCS 序列 | B2=0x10、80..86、Sleep Out 120 ms | 默认序列同样包含 2-lane B2、80..86、Sleep Out | 命令集可相互交叉验证，不需复制 |
| 硬件 reset | GPIO27，低有效，20 ms + 120 ms | GPIO27，低有效，20 ms + 120 ms | 已一致 |
| backlight | GPIO26，静态高电平 enable | GPIO26，当前静态 high/low；PWM 暂未接入 | 已一致；PWM 是后续增强项 |
| 视频像素格式 | RGB565，1024×600，PSRAM framebuffer | `dsi_probe video` 已使用 RGB565 DPI panel、PSRAM DMA buffer | 已在相同目标格式上，不应回退到旧 RGB888 探针 |
| timing | H 10/160/160，V 1/23/12，目标 52 MHz | 同一 timing 与目标像素时钟 | 已一致；实际可得时钟仍需以寄存器/示波器确认 |
| `/dev/fb0` | 已实现 `up_fbinitialize()`、`up_fbgetvplane()` | 当前只验证 Host/DPI/panel/scanout，未创建 FB 设备 | 这是进入 LVGL 前必须补的应用层适配 |
| LVGL | 通过 fbdev + touchscreen | 尚未进入 LVGL；刻意保持 `dsi_probe` 与 LVGL 解耦 | 当前策略正确，须先得到可见色条 |
| GT911 | I2C0 polling，INT/RST 未接，地址 0x5d | 当前尚未接入 | 430 可作为 GT911 板级接线/轮询参考 |

### 4.3 430 中已发现的限制

1. 其 `esp32p4_ek79007.c` 的面板型号、函数名和若干日志中仍混有
   `ILI9881C` 描述；虽然其初始化命令表指向 EK79007，但这说明不能把它
   作为未经审查的量产级面板实现直接采用。
2. 其 `src/Make.defs` 会在 `CONFIG_ESP32P4_FUNCTION_EV_LCD=y` 时加入
   `esp32p4_display.c` 和 `esp32p4_ek79007.c`，但所检视 head 的
   `src/CMakeLists.txt` 没有对应的 display/touch/audio/camera 条件追加。
   因此 CMake 与 Make 路径存在不一致风险，不能直接作为本项目构建模板。
3. PR #347 自身把 EK79007 DSI 的 device timing fine-tuning 列为剩余真机
   验证项；其结构有参考价值，不构成“屏幕已稳定显示”的证据。
4. 430 的 DSI Host API 来自其 fork 的 P4 芯片层；即使板级 constants
   一致，也不能把 `configure_dpi()`、`bind_framebuffer()` 等调用直接嫁接
   到本项目。

### 4.4 对本项目应吸收的内容

| 优先级 | 吸收内容 | 落点 | 前置条件 |
|---|---|---|---|
| P0 | 以 RGB565 + 1024×600 + 2 lane / 1000 Mbps 为唯一 LVGL 初版 profile | 当前 DPI panel config | 先取得可见色条 |
| P0 | `/dev/fb0` 的 `fb_vtable_s`、`up_fbinitialize()`、`up_fbgetvplane()` 分层 | 新增 board framebuffer glue | DSI DMA scanout 已有视觉通过 |
| P1 | `FB_UPDATE` 对 dirty area 做 cache clean | framebuffer glue | cache / PSRAM 语义明确 |
| P1 | GT911 I2C0 polling、0x5d 地址与坐标变换 | 独立 `esp32p4_touch.c` | 先核对实际 adapter 跳线与 I2C 波形 |
| P2 | LVGL fbdev + touchscreen defconfig | 新建 `lvgl` defconfig | `/dev/fb0` 与 `/dev/input*` 独立可测 |

## 5. 当前黑屏问题的定位边界

当前 `dsi_probe video` 已观察到：D-PHY PLL 成功、Host ready、EK79007
DCS 写入被接受、PSRAM framebuffer 已分配、DMA frame 计数持续增长，且未
报告 Bridge underrun。这只能证明：

```text
CPU -> PSRAM framebuffer -> DW-GDMA -> DSI Bridge 的软件活动存在。
```

它**不能**证明：

```text
正确的 DPI stream -> D-PHY electrical output -> LCD adapter -> EK79007
-> AML070JGI50 像素扫描
```

因此，345 和 430 都不能直接消除当前黑屏。下一轮定位仍以同一硬件上已经
显示成功的 `~/Project/espidf_lcd` 为真值，逐项比对：

1. DPI panel 的创建、reset、init、display on、draw bitmap 的实际顺序；
2. RGB565 byte order、DMA descriptor/EOF 重装、cache writeback；
3. DPI clock 的实际频率与同步极性；
4. Host/Bridge 的 `enable_video_mode`、`enable_dpi_output`、GDMA enable
   顺序；
5. GPIO26 PWM/EN、GPIO27 reset 跳线、adapter 5V 供电与 FPC 方向。

## 6. 建议实施顺序

1. **冻结当前 probe 证据。** 保存目前串口日志，记录“Host/DMA 通过，视觉
   未通过”，避免误标为 display PASS。
2. **做 345 基线回归。** 每次 DSI 改动后运行 cold boot、USB console、
   GPIO/IRQ、I2C 验证，先排除 SoC 基础回归。
3. **继续与 `espidf_lcd` 做寄存器级差异。** 这是当前 P0，不先引入 LVGL
   和 GT911。
4. **色条视觉通过后再建 `/dev/fb0`。** 优先实现类似 430 的 board-level
   framebuffer glue，但复用本项目的 Host 与 EK79007 层，不复制其私有 API。
5. **最后接 GT911 和 LVGL。** 分开验证 I2C touch、fbdev、LVGL；不要把
   首次点屏、触摸和 UI 三个变量绑定在同一次调试中。

## 7. 验收矩阵

| 阶段 | 必须观察到的结果 | 不应作为通过条件 |
|---|---|---|
| SoC 基线 | NSH、USB console、重启、GPIO/IRQ、I2C 均稳定 | 单次能烧录 |
| DSI 命令 Host | LDO/PLL/Host ready；EK79007 DCS 写无错误 | DCS read 一定可用（当前面板可为 write-only） |
| DSI 像素流 | 真实屏幕可见并稳定显示 RGB565 色条 | DMA 计数增加、无 underrun |
| framebuffer | `/dev/fb0` 可枚举，应用写入后可见 | `up_fbinitialize()` 返回 OK |
| touch | `/dev/input*` 有稳定坐标、方向正确 | I2C probe 有 ACK |
| LVGL | LVGL 刷新、触摸、重绘持续稳定 | 静态首帧显示 |

## 8. 维护规则

- 本文只记录对照结论，不替代芯片层、板级或驱动的设计文档。
- 每次吸收外部实现时，在对应提交说明中记录来源 PR、commit SHA、吸收
  的文件/符号和真机验收证据。
- 对公开竞赛仓库只做设计参考和最小 diff；不得将其未验证的显示逻辑直接
  标记为本项目的硬件验证结论。
