# ESP32-P4X SMP 启动复位修复方案

状态：基础修复已提交，扩展验收待完成；2026-10-29。本文保留原始故障基线、参考实现取舍及后续验收要求。

## 1. 基线与故障证据

| 项目 | 已确认结果 |
| --- | --- |
| 开发板 | ESP32-P4 Function EV Board，芯片 revision v3.2 |
| 故障基线 | `f99980ffa45f32fc885eb507cfc87ce6156224ca`，实验性 SMP 已合入主分支 |
| 依赖 | NuttX `106bf012365ed035c6d6c717e73448e76089d721`；HAL `b90b1837cb5ad24747deb4c895246037cc206ce5` |
| 构建配置 | `configs/smp`；`CONFIG_SMP=y`、`CONFIG_SMP_NCPUS=2` |
| 故障镜像 | 334076 字节，入口 `0x4ff46178`；ELF/BIN 对照及镜像 checksum 检查通过 |
| 烧录 | 应用写入 `0x2000`；esptool 输出 `Hash of data verified` |
| 原始启动现象 | ROM 跳转入口后重复出现 `HP_SYS_HP_WDT_RESET`，未获得 NSH 或 CPU1 ready 日志 |

故障样本 SHA256：`3670af58f615032455268704dd599b67f35debcb91a38a6faf399e5db1f8121d`。后续构建必须另行保存配置、ELF 和镜像摘要。

ROM 的 `Calculated` 与该镜像 RAM 部分的 SHA256 一致；`Expected` 对应 RAM 校验边界后的填充段头。当前 `--ram-only-header` 不追加 SHA 摘要，不能仅凭该提示认定烧录损坏或启动根因。

早期 HAL 打印可能转向 UART0，NSH 则使用 USB Serial/JTAG。USB 无后续输出不足以确定卡点。

当前修复提交为 `4b4bf44e5f41e9702eed2adeae556b33c184611d`，已包含早期看门狗处理、HAL 临界区、RTC 校准初始化、UART 早期使能、CPU1 入口发布顺序及 HAL 补丁固化。

[修复日志](ESP32-P4X-SMP启动修复日志_20261028.md)记录双核 NSH、`smp`、`atomic`、`ostest` 的板端结果。以下状态引用该记录；本次文档整理未重跑构建或板端测试。单核回归、长时稳定性和 CMake 仍待验收。

## 2. 参考实现与取舍

参考 359 队提交 `28254e10606697d82b18964210f34078d5337f83`，分别核对根目录芯片实现和 v3 overlay。

| 差异 | 修复方向 |
| --- | --- |
| 早期处理 HP/LP flashboot 看门狗 | 核对本芯片复位来源及启动时序，建立启动阶段到系统运行阶段的看门狗接管 |
| NuttX 专用 Simple Boot 路径跳过部分二级引导初始化 | 逐项确认 ROM 已完成的硬件、时钟、Flash、Cache 初始化，避免重复配置；不直接照搬提前返回 |
| 显式初始化 Timer Group0 校准资源 | 在首次时钟校准前调用幂等初始化，不依赖较晚执行的构造函数 |
| HAL 临界区区分系统就绪前后 | 核对早期锁与运行期抢占保护；CPU1 上线后必须保留跨核互斥 |
| v3 overlay 先发布 CPU1 入口再复位、释放 | 按当前 HAL/ROM 契约核对顺序，保留独立栈、屏障、有限等待及失败处理 |

359 队根目录 HAL 为 `8d0a8989…`，与本项目不同；其通用 RISC-V 启动接口也经过修改。保留本项目自定义 SMP 接口及任务式 HAL IPC，先迁移必要的启动兼容处理。

359 队的 CPU1 运行状态记录证明已有启核成果，但同一记录包含访问异常，不作为本项目的稳定性验收。

## 3. 修复顺序

以下为定位与修复流程；基础改动已提交，剩余验证见第 5 节。

### R0：确定卡点

增加少量、可关闭的阶段标记：C 入口、BSS 清零后、`bootloader_init()` 前后、XIP 映射前后、时钟初始化前后、`nx_start()` 前、CPU1 释放前及握手完成。

输出使用不依赖调度器的早期通路，核实实际监听接口；避免普通日志锁、动态分配和无限等待。保留原始复位原因，每轮只改变一个初始化环节。

### R1：打通 CPU0 启动

1. 核对 HP Timer Group、LP 看门狗的 flashboot 状态，调整喂狗或接管时机；超时应能报告阶段，不以永久关闭监控代替修复。
2. 根据 R0 结果收敛 Simple Boot 初始化职责，保留必需的 Flash 访问、MMU/Cache 配置及内存保护。
3. 将 Timer Group0 校准资源初始化前移至首次使用前，保证重复调用不复位正在使用的定时器。
4. UART 模块使能在 `nx_start()` 前使用仅 CPU0 运行时可用的保护，避免提前请求任务互斥量；调度器就绪后恢复正常同步路径。

### R2：核对 HAL 与双核启动

1. HAL 临界区明确单核早期、双核上线、正常调度三个阶段的锁策略，成对维护 IRQ 状态及必要的抢占保护；不能仅靠 `OSINIT_OS_READY()` 长期跳过锁。
2. 核对 CPU1 入口发布、复位、时钟、unstall 顺序，清除启动前残留的 FROM_CPU 中断。
3. 核对 CPU1 的栈、GP、异常向量、每核中断、在线状态和 IPI；仅在需要时调整，保留现有超时及失败停止机制。

### R3：固化构建入口

Make 已通过 `0003`、`0004`、`0005` 三份项目 HAL 补丁固化临界区、启动及 IPC 修复，构建无需临时补丁目录。继续验收清理重建、对象失效及 CMake 的补丁应用与 HAL 版本一致性。

保持 Flash MTD、PSRAM、PM 和复杂外设的 SMP 限制；现有 SmartHome 配置继续保持单核。

## 4. 改动范围

| 位置 | 内容 |
| --- | --- |
| `chips/esp32p4/common/espressif/esp_start.c` | 阶段取证、Simple Boot 初始化顺序、时钟校准及看门狗接管 |
| `chips/esp32p4/common/espressif/esp_lowputc.c` | UART 早期模块使能与运行期同步的阶段区分 |
| `chips/esp32p4/common/espressif/esp_smp.c`、`esp_cpu1_start.S` | CPU1 启动顺序、每核初始化及握手核对 |
| `chips/esp32p4/common/espressif/patches/` | 固定 HAL 的启动、校准和临界区补丁 |
| `chips/esp32p4/common/espressif/Make.defs`、`CMakeLists.txt` | 补丁应用、对象失效和构建一致性 |
| `board/esp32p4/esp32p4-function-ev-board/configs/smp/` | 最小诊断配置及构建、验收说明 |

既有 NuttX 自定义 SMP 补丁继续按固定依赖维护。不涉及 UI、cAGENT、音频录放或外设并行优化。

## 5. 验收

| 阶段 | 通过条件 | 当前记录 |
| --- | --- | --- |
| 构建 | 干净输出生成 ELF/BIN；确认双核配置、CPU1/IPI 符号、每核栈和补丁应用结果 | Make 已记录成功；CMake 待验收 |
| 启动 | 进入 NSH，出现 CPU1 ready；挂载 procfs 后 `ps` 列出两个 IDLE；无重复看门狗复位 | 日志已记录通过 |
| 单核亲和性 | `taskset 1 smp` 仅观察到 CPU0，`taskset 2 smp` 仅观察到 CPU1；所有测试线程正常退出 | 待补充 |
| 双核调度 | `taskset 3 smp` 观察到两个核，barrier 及 join 均完成 | 日志已记录通过 |
| 内核测试 | `taskset 3 atomic` 无失败信息；`taskset 3 ostest` 完成，包含跨核调用成功记录 | 日志已记录基础用例结果 |
| 补充并发测试 | 两核固定任务双向唤醒各 10000 次；32/64 位共享原子加法和 CAS 结果准确，无超时 | 待实现与验收 |
| 稳定性与回归 | 冷启动、复位各 10 次；重复负载至少 30 分钟，无死锁、任务残留、持续内存增长或栈越界；单核 NSH 构建与启动通过 | 待验收 |

现有 `atomic` 仅覆盖单线程 8/16/32 位操作；`atomic test complete!` 不能覆盖失败信息，也不代表 64 位并发通过。补充并发测试需要单独实现；`ostest` 会修改部分测试的亲和性，应核对实际覆盖。

固件构建、烧录和板端测试由用户执行。每轮保存完整启动日志、最后阶段标记、配置、ELF、镜像摘要和测试结果；未通过当前阶段时不叠加后续外设。

## 6. 依据

- [本项目双核 SMP 适配方案](ESP32-P4X-双核SMP适配方案.md)
- [当前 SMP 构建与板端步骤](../../../board/esp32p4/esp32p4-function-ev-board/configs/smp/README.md)
- [359 队 HAL 启动补丁](https://github.com/open-vela/contest2026_359_dengfengzaojidecuipidaxuesheng/blob/28254e10606697d82b18964210f34078d5337f83/chip/esp32p4/src/patches/esp-hal-3rdparty/0002-openvela-esp32p4-smp-start.patch)
- [359 队 Simple Boot 兼容补丁](https://github.com/open-vela/contest2026_359_dengfengzaojidecuipidaxuesheng/blob/28254e10606697d82b18964210f34078d5337f83/chip/esp32p4/src/patches/esp-hal-3rdparty/0003-esp32p4-gcc15-simple-boot-compat.patch)
- [359 队 v3 启动与每核初始化](https://github.com/open-vela/contest2026_359_dengfengzaojidecuipidaxuesheng/blob/28254e10606697d82b18964210f34078d5337f83/app/espdl-quickapp/overlay/nuttx/arch/risc-v/src/common/espressif/esp_start.c)
- [359 队 v3 SMP 实现](https://github.com/open-vela/contest2026_359_dengfengzaojidecuipidaxuesheng/blob/28254e10606697d82b18964210f34078d5337f83/app/espdl-quickapp/overlay/nuttx/arch/risc-v/src/esp32p4/esp32p4_smp.c)
- [359 队双核运行及异常记录](https://github.com/open-vela/contest2026_359_dengfengzaojidecuipidaxuesheng/blob/28254e10606697d82b18964210f34078d5337f83/app/espdl-quickapp/evidence/follow-crash-state.txt)
