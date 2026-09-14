# ESP32-P4 上游成熟适配吸收计划

## 1. 目的

本计划定义如何将 Apache NuttX 与 OpenVela 已验证的 ESP32-P4 适配，**选择性**
吸收至本项目的 `chips/esp32p4/` 与
`board/esp32p4/esp32p4-function-ev-board/`。目标是降低基础芯片代码的维护成本，
同时不破坏已在 P4X 实板验证的 MIPI-DSI 命令链路。

“吸收”不是复制整个上游目录，也不是依赖未固定的上游 HEAD；它是一次带来源版本、
逐文件审查、逐提交回归的移植工作。

## 2. 当前判断与边界

上游 Apache NuttX 已具备 ESP32-P4 与 `esp32p4-function-ev-board` 的基础支持：
P4 revision 配置、启动、内存、时钟、中断、常用外设、USB Serial/JTAG，以及
Function-EV-Board 的基础配置。上游 P4 平台文档仍将 MIPI DSI 列为未支持状态，
故上游基础移植不能替代本项目的 `esp_ldo.c/.h`、`esp_mipi_dsi.c/.h`、P4X 面板
复位、背光与显示专项工作。

参考来源：

- [Apache NuttX ESP32-P4 平台文档](https://nuttx.apache.org/docs/latest/platforms/risc-v/esp32p4/index.html)
- [Apache NuttX ESP32-P4 Function-EV-Board 文档](https://nuttx.apache.org/docs/latest/platforms/risc-v/esp32p4/boards/esp32p4-function-ev-board/index.html)
- [上游 ESP32-P4 Kconfig](https://github.com/apache/nuttx/blob/master/arch/risc-v/src/esp32p4/Kconfig)

本项目采用 `ARCH_CHIP_CUSTOM` / custom board 接线，上游与本地路径不完全相同：

```text
Apache NuttX
  arch/risc-v/src/esp32p4/                    # 上游芯片层
  boards/risc-v/esp32p4/esp32p4-function-ev-board/

本项目
  chips/esp32p4/                              # custom chip 映射目标
  board/esp32p4/esp32p4-function-ev-board/    # custom board 映射目标
```

## 3. 基本原则

1. **先固定来源，再比较代码。** 每轮同步记录 remote、分支、完整 commit SHA、日期
   与 ESP HAL 版本；禁止仅记录“最新 master”。
2. **按能力单元吸收，不整目录覆盖。** 一个单元只包含一种目的，例如 revision
   Kconfig、USB console 或 Ethernet。
3. **本地 DSI 是 overlay。** `esp_ldo.*`、`esp_mipi_dsi.*` 和相关 Kconfig /
   Make / CMake 接线必须保留，除非上游有经代码对比和实板验证的等价实现。
4. **板级参数不盲从上游。** GPIO27 reset、GPIO26 背光、adapter 供电、EK79007
   timing、P4X revision v3.x 以实板和原理图为准。
5. **HAL 版本锁定而非直接修改。** 不提交、也不直接编辑 `esp-hal-3rdparty` 工作
   副本；兼容修复以项目补丁、版本记录或可复现同步流程表达。
6. **每项吸收必须有回归证据。** 至少完成配置和构建；涉及启动/外设必须有实板串口
   证据。

## 4. 吸收范围分级

| 分级 | 内容 | 策略 | 结论 |
| --- | --- | --- | --- |
| A：优先吸收 | P4 revision、cache、MSPI workaround、启动、heap、IRQ、USB Serial/JTAG、UART、I2C、SPI、I2S、GDMA、以太网、PSRAM | 逐文件对比后选择缺失的 bugfix 或配置 | 适合持续吸收 |
| B：按板核对 | Function-EV-Board 的通用初始化、USB console、Ethernet、Flash/PSRAM 配置、defconfig 组织 | 对照 P4X 实物和 adapter 后吸收 | 不覆盖 P4X 私有引脚 |
| C：仅作接口参考 | `drivers/video/mipidsi/` 的 Host / packet / DCS API | 芯片层对接公共接口，不复制 packet 编解码 | 当前 DSI 上层契约 |
| D：本地维护 | D-PHY LDO、DSI Host/PHY、DPI pattern、EK79007、GPIO27、GPIO26、后续 framebuffer/DMA/GT911 | 继续在竞赛项目实现和验证 | 上游尚不能替代 |
| E：禁止直接导入 | ESP-IDF 业务示例、未固定 HAL 目录、其他板 pinmux/时钟常量 | 仅用于理解寄存器和时序 | 不纳入产品源码 |

## 5. 推荐架构

```text
固定 SHA 的 Apache NuttX / OpenVela P4 基线
        │
        ├── 选择性吸收：启动、外设、Kconfig、构建规则、通用 board 改进
        │
本项目 custom chip / custom board
  chips/esp32p4/                 board/esp32p4/.../
        │
        ├── 本地 DSI overlay：LDO、Host/PHY、DPI、P4X pin/timing
        └── 板级装配：EK79007、背光、触摸、LVGL
```

芯片层向上只暴露 NuttX 风格 API 与 errno；ESP HAL 私有类型、寄存器地址和 HAL
版本差异停留在芯片层 `.c` 文件内。板级层负责实例、GPIO 和面板时序，应用/LVGL
不得直接包含 ESP HAL 头文件。

## 6. 实施步骤

### S0：建立可追溯的上游基线

1. 在工作区外或独立 worktree 获取 Apache NuttX 与（如需要）OpenVela NuttX。
2. 记录 remote URL、分支、完整 commit SHA、同步日期和 ESP HAL 对应版本。
3. 保存只读比较结果；不向当前 `nuttx/`、`apps/`、`vendor/` 或 HAL checkout 写入
   试验性修改。
4. 每轮同步记录必须包含：

```text
来源：apache/nuttx
分支：master
提交：<40 位 SHA>
同步日期：YYYY-MM-DD
目标：P4 revision Kconfig / USB Serial-JTAG（示例）
```

### S1：生成差异清单

按四组分别比较，禁止把全部 diff 视为待合入项：

| 比较组 | 上游来源 | 本地目标 | 关键审查问题 |
| --- | --- | --- | --- |
| 芯片配置 | `arch/risc-v/src/esp32p4/Kconfig` | `chips/esp32p4/Kconfig` 与 common Kconfig | 是否适用于 P4 v3.x？是否影响已有 select？ |
| 芯片实现 | `arch/risc-v/src/esp32p4/`、common Espressif 层 | `chips/esp32p4/` | 是否依赖不同 HAL API、链接规则或目录结构？ |
| 板级实现 | 上游 Function-EV-Board `src/` | 本项目 board `src/` | 是否引用不同 revision、pinmux 或屏幕连接？ |
| 构建/配置 | `Make.defs`、`CMakeLists.txt`、defconfig | 本项目同类文件 | Make 与 CMake 是否同步？ |

每项差异必须标记为：`吸收`、`保留本地`、`仅参考` 或 `拒绝`，并写明原因。

### S2：最小能力单元移植

建议优先级：

1. P4 revision / cache / MSPI workaround 的 Kconfig 与启动 bugfix；
2. USB Serial/JTAG 与 `usbconsole` 稳定性改进；
3. Flash、PSRAM、heap 与 DMA 内存布局改进；
4. Ethernet、I2C、SPI、I2S、GDMA 等已使用外设的修复；
5. 与 P4X 实物相符的非显示板级初始化；
6. 最后才评估 DSI 周边依赖。DSI Host、P4X panel 代码和显示时序不能因“上游有
   类似目录”而被覆盖。

每个能力单元单独提交。提交说明必须包含上游 SHA、采纳文件、未采纳文件、适配原因
与测试结果。

### S3：构建与实板回归

每个单元至少执行：

```text
1. olddefconfig：确认没有新的 Kconfig warning
2. Make 构建：确认 custom chip / board 路径正常
3. CMake 构建：若改动触及 CMakeLists 或 HAL 注册，必须验证
4. 实板 usbconsole：确认重启后仍可进入 nsh>
```

| 改动范围 | 必须追加的验收 |
| --- | --- |
| 启动、revision、Flash、PSRAM | 冷启动、重复 reset、固件版本/heap 观察 |
| USB Serial/JTAG | 反复烧录、串口重枚举、稳定 NSH 输入 |
| 网络 | `ifconfig`、DHCP/静态地址、ping 或业务 socket |
| I2C/SPI/I2S/GDMA | 独立外设 probe 或既有硬件 demo |
| DSI 相关依赖 | `dsi_probe` command path，再到 `dsi_probe video`；不得跳过前序门直接归因给 LVGL |

### S4：归档与长期维护

1. 在开发日志记录每次同步的 SHA、结论、构建命令和实板证据。
2. 每月或在上游发布 P4 修复后执行一次 S0/S1；无明确收益不做机械同步。
3. 对已本地验证的 DSI 改动维护“可上游化候选”清单；待接口稳定且无 P4X 私有
   参数后，再准备独立上游 patch。

## 7. DSI 专项边界

上游的 `mipi_dsi_host` 是通用框架；P4 实际 Host/PHY 适配仍由本项目维护。因此：

- 可以吸收通用 MIPI-DSI API 变化，并让 `esp_mipi_dsi.c` 跟随公共接口演进；
- 不复制或替换 P4X 的 LDO channel、2 lane/1000 Mbps、GPIO27、GPIO26 和 EK79007
  timing；
- M2a 内建色条、M2b framebuffer/DMA、LVGL 和 GT911 分别保有独立 build 与实板
  验收门；
- 若未来上游新增 P4 DSI Host，先比较寄存器/HAL 版本、超时模型、锁策略、error
  IRQ、DMA/cache 模型，再决定迁移或吸收其中一部分。

## 8. 风险与控制

| 风险 | 典型表现 | 控制措施 |
| --- | --- | --- |
| HAL 版本不匹配 | 缺头文件、未定义符号、LL/HAL 字段不同 | 先固定 HAL revision；不编辑 HAL 工作副本 |
| revision 不匹配 | 固件无法启动、WDT reset、PLL/Flash 异常 | 以 P4X v3.x 实物 revision 和上游 Kconfig 为准，先回归最小启动 |
| Kconfig select 冲突 | `olddefconfig` warning、功能被意外打开 | 新增 select 前检查直接依赖；Make/CMake 同步审查 |
| 板级 pinmux 覆盖 | 串口、reset/背光或 I2C 失效 | Board 改动独立提交，逐引脚对照原理图和实测 |
| DSI 回归 | Host 可初始化但无画面或 command 超时 | 保留本地 overlay；依次跑 command、pattern、framebuffer、LVGL |
| 大范围合并难定位 | 一次改动后无法判断回归来源 | 一个能力单元一个提交，失败立即回退该单元 |

## 9. 提交与验收模板

```text
sync(esp32p4): 吸收上游 USB Serial/JTAG 初始化修复

上游来源：apache/nuttx <SHA>
采纳内容：...
保留本地：DSI overlay / P4X GPIO 配置
验证：olddefconfig、Make、usbconsole 实板 nsh>
```

一轮吸收完成的最低条件：来源 SHA 和差异决策可追溯；不修改第三方 HAL 工作副本；
新增符号同时接入 Kconfig、Make.defs、CMakeLists；`usbconsole` 实板仍可稳定进入
`nsh>`；若触及显示依赖，既有 `dsi_probe` 命令链路不退化；开发日志有可复现命令和
串口结论。

## 10. 首轮建议

首轮只做“上游 P4 baseline 审计”，不立即搬运代码：固定 SHA、列出差异、确认
custom chip 与上游目录映射，并筛出 revision/Kconfig/USB console 三类候选。完成
审计并通过最小 `usbconsole` 回归后，再选择其中一类进行首次小提交。这样先验证
同步流程本身，不会将 DSI、触摸或 LVGL 的复杂变量混入基础适配工作。
