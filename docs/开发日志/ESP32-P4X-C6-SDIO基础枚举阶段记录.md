# ESP32-P4X 板载 C6 SDIO 枚举与 Function 1 验证记录

记录日期：2026-09-13。当前为 Wi-Fi 接入的基础枚举阶段，用户要求先整理并提交当前工作，再推进下一步。

## 当前交付范围

| 层级 | 文件（项目根目录下） | 职责 |
| --- | --- | --- |
| 板级 | `board/esp32p4/esp32p4-function-ev-board/src/esp32p4_hosted.c`、`include/board.h` | P4 引脚表、C6 复位与启动等待、装配传输层 |
| 芯片控制器 | `chips/esp32p4/common/espressif/esp_hosted_sdio.c`、`chips/esp32p4/include/esp_hosted_sdio.h` | GPIO Matrix 信号路由、SDMMC 时钟/复位、CMD52/CMD53 同步原语、错误快照与失败回收 |
| 芯片私有传输 | `chips/esp32p4/common/espressif/esp_hosted_transport.c`、`chips/esp32p4/include/esp_hosted_transport.h` | CMD0/CMD5/CMD3/CMD7/CMD52 枚举、Function 1 启用、块大小配置及受限 CMD53 访问 |
| 构建接入 | 板级及芯片层 Kconfig、Make.defs、CMakeLists.txt、HAL 包含路径 | 编译上述模块，由板级配置选择传输层，再选择控制器 |
| 启动配置 | `configs/smart_home/defconfig`、`src/esp32p4_bringup.c`（板目录下） | 开启板载 C6 探测，失败记录错误后继续其他外设启动 |

P4 侧 CLK=18、CMD=19、DAT0～3=14～17、C6 RESET=54，使用 SDMMC slot 1。
当前仅验证 1-bit 模式，请求时钟 400 kHz，按分频计算并打印为 384 kHz；未使用示波器确认实际频率。
板级不再编排 SDIO 枚举命令，也不直接包含 HAL 寄存器头文件。

## 问题与修正

1. 新增传输层后，曾出现符号未链接的问题；现在同步维护 Make/CMake 源文件列表及 Kconfig 依赖链。
2. 早期枚举从 CMD5 直接跳到 CMD52，缺少 CMD3 获取 RCA、CMD7 选卡。现在完整执行这两个步骤，使用从机返回的地址，并检查 R6/R1/R5 错误位。
3. 实板曾分别停在 CMD5、CMD3。CMD5 失败快照为 `raw=0x100 status=0x176`，表明硬件响应超时，命令状态机等待响应起始位。不能将此归因于软件轮询超时，也不能仅凭 CMD0 完成证明从机在线。
4. 原来释放 C6 复位后仅忙等 100 ms。现保留 10 ms 复位脉冲，改为任务休眠 1000 ms 后探测。随后用户提供一次完整枚举成功日志。这支持启动裕量不足的假设，但未证明所有历史故障只有这一原因；固定等待也不代表已检测到 C6 就绪。
5. 控制器保留命令完成日志和清中断前的寄存器快照，区分硬件响应超时、软件等待超时、响应错误及 HLE。只清除本次命令相关事件，避免吞掉其他事件。

## 实板证据

以下为用户提供的成功启动日志节选，并非完整串口归档：

```text
INFO: ESP-Hosted C6 boot wait complete; probing SDIO
INFO: ESP-Hosted SDIO: cmd=0 arg=0x00000000 raw=0x00000004 elapsed_us=1110
INFO: ESP-Hosted SDIO: cmd=5 arg=0x00000000 raw=0x00000004 elapsed_us=900
INFO: ESP-Hosted SDIO: cmd=5 arg=0x00ff8000 raw=0x00000004 elapsed_us=900
INFO: ESP-Hosted SDIO: cmd=5 arg=0x00ff8000 raw=0x00000004 elapsed_us=890
INFO: ESP-Hosted C6 probe: stage=cmd5 R4=0xa0ffff00
INFO: ESP-Hosted SDIO: cmd=3 arg=0x00000000 raw=0x00000004 elapsed_us=890
INFO: ESP-Hosted C6 probe: stage=cmd3_rca R6=0x00010000
INFO: ESP-Hosted SDIO: cmd=7 arg=0x00010000 raw=0x00000004 elapsed_us=900
INFO: ESP-Hosted C6 probe: stage=cmd7_select R1=0x00001e00
INFO: ESP-Hosted SDIO: cmd=52 arg=0x00000000 raw=0x00000004 elapsed_us=930
INFO: ESP-Hosted C6 SDIO: slot=1 width=1 clock=384kHz revision=0x5342270a hardware=0x03c44c83
INFO: ESP-Hosted C6 probe: CMD5 R4=0xa0ffff00 RCA=0x0001 CCCR=0x32
INFO: ESP-Hosted Function 1 ready: io_ready=0x02 block_size=512
INFO: ESP-Hosted C6 CMD53 probe: function=1 address=0x050 int_raw=0x00484000
```

`io_ready=0x02` 表明从机已报告 Function 1 就绪；随后 P4 通过 CMD53 从 Function 1 的 `INT_RAW` 寄存器读到四字节数据。它验证了命令响应和 PIO 数据相位，`int_raw` 的位语义仍须在 ESP-Hosted 协议阶段按 C6 固件版本解释。

用户完成固件编译、烧录及本次板端运行。开发过程中曾通过临时主机桩测试：16 个枚举/错误场景和 8 个命令处理场景；这些不是实板稳定性证据，也不是仓库内持久化回归测试。

## 验证边界和下一步

- 已确认：一次启动完成 CMD0 → CMD5 → CMD3 → CMD7 → CMD52，读取 CCCR=0x32；启用 Function 1、配置并回读 512 字节块大小，及 CMD53 读取 `INT_RAW=0x00484000`。
- 待确认：10 次 RESET、5 次断电重启，以及失败后重新初始化的实板恢复能力；当前不能宣称稳定性验收通过。
- 未验证：4-bit/高速模式、CMD53 FIFO 连续收发、数据 DMA、Function 1 中断、ESP-Hosted 版本握手和 Wi-Fi 联网。接口存在不等于这些能力可用。
- C6 固件来源、版本及构建配置尚未确认。CCCR 读取成功不能证明 ESP-Hosted 协议兼容。

下一阶段按以下顺序推进，每个阶段独立验证后再提交：

1. 固定 C6 固件基线，补充启动稳定性记录。
2. 固定 C6 固件版本后，配置 Function 1 中断与数据通路；用 CMD53 从 SLC FIFO 接收 ESP-Hosted 初始化事件。
3. 对齐 C6 固件的 ESP-Hosted 包格式和版本握手，验证连续控制包收发及复位恢复。
4. 实现 NuttX 网络接口和 Wi-Fi 控制映射，注册 `wlan0`，依次验收关联、DHCP、DNS，再接入 Smart Home 云端调用。

这次提交是明确授权的阶段交付，不代表接入方案中的整个 W1 或 Wi-Fi 功能完成。
