# ESP32-P4 Function EV Board 最小 NSH 操作与测试

本文用于验证本项目已适配的 ESP32-P4 Function EV Board 基础启动链路：构建 P4 的 `nsh` 配置、烧录生成物，并通过串口确认 NuttShell（NSH）可用。

这是一份上板操作手册，不代表已经完成实际硬件验收。烧录地址、烧录文件组合和串口设备名必须以本机构建输出及连接的开发板为准。

## 1. 范围与配置

| 项目 | 内容 |
| --- | --- |
| 目标开发板 | ESP32-P4 Function EV Board |
| 最小验证配置 | `board/esp32p4/esp32p4-function-ev-board/configs/nsh` |
| 构建入口 | 工作区根目录的 `build.sh` |
| 串口控制台 | UART0（以最终生成的 `nuttx/.config` 为准） |
| 不在本次范围内 | 音频应用、网络、显示屏、摄像头及其外设验证 |

本项目已移除旧的 ESP32-S3 板级与 `audio_event` 路径；本手册只面向 P4 最小 NSH 基线。

## 2. 前置条件

1. 已取得完整 openvela 工作区，并位于包含 `build.sh` 的根目录。
2. 已检出竞赛仓的目标提交，并且 P4 芯片及板级源码目录完整。
3. 已安装 ESP32-P4 所需的 RISC-V 交叉工具链，且终端可调用。
4. 开发板已通过 USB 连接；确认下载口和 UART0 日志口。它们可能是不同的物理串口。

在工作区根目录检查关键目录和工具链：

```sh
cd /home/arongw/openvela

test -d vendor/espressif/chips/esp32p4
test -d contest2026_031_niudanxianqianchong/chips/esp32p4
test -d contest2026_031_niudanxianqianchong/board/esp32p4/common
test -d contest2026_031_niudanxianqianchong/board/esp32p4/esp32p4-function-ev-board

command -v riscv-none-elf-gcc
riscv-none-elf-gcc --version
```

以上 `test` 命令均应以零退出码结束，分别确认 P4 芯片、公共板级目录和 `esp32p4-function-ev-board` 板级目录存在。若工具链命令名称在本地环境不同，应先完成本地 openvela 工具链初始化，再继续构建。

## 3. 构建最小 NSH 固件

从工作区根目录执行：

```sh
./build.sh contest2026_031_niudanxianqianchong/board/esp32p4/esp32p4-function-ev-board/configs/nsh -j2
```

`nsh` 不启用乐鑫工具链，`./build.sh` 会选到 `riscv-none-elf`，可以直接构建。
启用 `CONFIG_ESP32P4_ESPRESSIF_TOOLCHAIN=y` 的配置（`smart_home_kws`、
`smp_smart_home`）必须改用 `scripts/build_p4x.sh <config> -j8`，因为
`envsetup.sh` 只把 `riscv-none-elf` 加进 PATH。

当前 P4 HAL 兼容补丁只在板级 `Make.defs` 构建链路中应用，因此本基线不要添加 `--cmake`。构建过程会按需获取 HAL 并应用该补丁。

构建完成后，检查实际生效的配置：

```sh
rg 'CONFIG_ARCH_RV32|CONFIG_ARCH_CHIP_ESP32P4|CONFIG_ESPRESSIF_CHIP_SERIES|CONFIG_UART0' nuttx/.config
```

最低预期包括：

```text
CONFIG_ARCH_RV32=y
CONFIG_ARCH_CHIP_ESP32P4=y
CONFIG_ESPRESSIF_CHIP_SERIES="esp32p4"
CONFIG_UART0_SERIAL_CONSOLE=y
```

构建成功的判断是命令以零退出码结束，且输出目录中出现本次构建的固件及烧录相关生成物。不要沿用其他芯片或旧构建目录中的二进制文件。

### 常见构建问题

| 现象 | 优先检查项 |
| --- | --- |
| 编译器提示缺少 `-mabi=` | 检查 `nuttx/.config` 中 `CONFIG_ARCH_RV32=y` 是否生效。 |
| HAL 补丁应用失败 | 检查 P4 HAL 目录是否被人工修改；不要在其中直接叠加临时补丁。 |
| HAL 获取或克隆失败 | 检查网络、Git 访问权限和本地缓存状态。 |
| 其他编译错误 | 从日志中第一条错误开始定位，不要只处理末尾的连带错误。 |

若需要清理旧配置，可执行：

```sh
./build.sh contest2026_031_niudanxianqianchong/board/esp32p4/esp32p4-function-ev-board/configs/nsh distclean
./build.sh contest2026_031_niudanxianqianchong/board/esp32p4/esp32p4-function-ev-board/configs/nsh -j2
```

`distclean` 会清理构建产物，并可能使 HAL 在下次构建时重新获取和打补丁；仅在配置残留或构建状态异常时使用。

## 4. 确认烧录生成物与端口

先查看本次构建实际生成的文件和烧录说明：

```sh
find nuttx -maxdepth 4 -type f \( -name 'nuttx.bin' -o -name '*.bin' -o -name '*flash*' \) -print
ls -l /dev/serial/by-id/
ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null
```

再确认下载口可识别为 ESP32-P4（将设备名替换为实际下载口）：

```sh
esptool.py --chip esp32p4 --port /dev/ttyACM0 chip_id
```

烧录命令中的偏移量、文件名和文件数量必须使用本次构建输出或其生成的烧录参数。不要手工套用 ESP32-S3、其他 P4 项目或网络示例的 `write_flash` 地址。

下面仅展示命令形式，不可直接照抄其中的占位符：

```sh
esptool.py --chip esp32p4 --port <下载串口> --baud <波特率> \
  write_flash <地址1> <文件1> [<地址2> <文件2> ...]
```

烧录完成后断开并重新上电，或按复位键，使新固件从正常启动路径运行。

## 5. 连接 UART0 并验证 NSH

从最终配置确认 UART0 波特率：

```sh
rg '^CONFIG_UART0_BAUD=' nuttx/.config
```

以该值打开 UART0 日志口。例如配置为 `115200` 时：

```sh
picocom -b 115200 /dev/ttyACM1
```

按复位键或重新上电，预期可以在启动日志后看到：

```text
nsh>
```

在 NSH 中依次执行以下最小检查：

```sh
help
uname -a
ls /dev
free
echo p4-nsh-ok
```

验收结果如下：

| 检查项 | 通过条件 |
| --- | --- |
| 启动 | 串口有正常启动日志，最终进入 `nsh>`。 |
| `help` | 能列出可用 NSH 命令。 |
| `uname -a` | 能返回当前 NuttX/openvela 系统信息。 |
| `ls /dev` | 能访问设备节点目录，至少不出现异常重启或卡死。 |
| `free` | 能输出内存统计信息。 |
| `echo p4-nsh-ok` | 串口回显 `p4-nsh-ok`。 |

若看不到日志，按以下顺序排查：串口是否选错、UART0 与下载口是否不同、波特率是否与 `nuttx/.config` 一致、板卡是否处于下载模式、供电和 USB 数据线是否正常。

## 6. 记录测试证据

建议每次上板测试记录以下信息，便于后续定位板级差异：

| 项目 | 记录内容 |
| --- | --- |
| 源码版本 | 竞赛仓提交 ID、工作区是否有未提交改动。 |
| 构建命令 | 完整 `build.sh` 命令及退出状态。 |
| 配置确认 | `CONFIG_ARCH_RV32`、芯片型号、UART0 波特率。 |
| 烧录依据 | 本次构建使用的文件、偏移量来源和下载口。 |
| 串口输出 | 从复位到 `nsh>` 的关键启动日志。 |
| NSH 结果 | 本文第 5 节各命令的输出或失败现象。 |

完成本手册中的构建、启动和五项 NSH 命令后，可将 P4 最小系统基线标记为“已上板验证”。音频和其他外设应在各自独立的测试计划中继续验证。
