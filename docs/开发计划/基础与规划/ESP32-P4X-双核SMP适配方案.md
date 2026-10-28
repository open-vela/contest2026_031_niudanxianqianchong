# ESP32-P4X 双核 SMP 适配方案

状态：已实施实验性双核启动、P4 IPI、每核栈、中断初始化及 HAL IPC 接入；实板验收待完成。

## 1. 目标

使两个 HP RISC-V 核运行同一个 NuttX 系统，支持双核任务执行、跨核唤醒和任务亲和性，
再逐项验证驱动并发。首期保持现有主频和单核配置，不包含 LP Core、动态调频、
休眠及业务线程分核优化。

## 2. 适配前的基础与缺口

| 层次 | 已有基础 | 待完成 |
| --- | --- | --- |
| 配置 | `ARCH_HAVE_MULTICPU`、`ESPRESSIF_NUM_CPUS=2`，现有配置仍为单核 | 独立 SMP 配置、两核栈与每核状态核对 |
| 启动 | `esp_head.S` 使用同一启动栈；`__esp_start()` 清 BSS、初始化全局资源并进入 `nx_start()` | CPU1 专用入口、启动地址、解除复位/停核、就绪握手 |
| 中断 | IRQ handle 表按 CPU 分组，采用 Espressif 中断分配与分发 | 每核本地初始化、跨核中断、IRQ 所属核管理 |
| 调度 | 工作区已有通用 RISC-V SMP 调度代码 | 适配 P4 IPI；通用 `riscv_ipi.h` 当前依赖 SBI 或 `RISCV_IPI` 的 MSIP 寄存器模型 |
| HAL | 固定 HAL 的 `sdkconfig.h` 已有 SMP/IPC 配置及 NuttX IPC 适配源码 | 核对实际构建接入、启动顺序、Flash 停核及缓存保护 |

硬件核数、按核分配的数组和 HAL 条件编译，均不能代替 CPU1 启动及调度验收。

## 3. 启动与调度设计

```text
CPU0：全局初始化 → nx_start → 创建两核 IDLE → up_cpu_start(1) → 正常调度
                                                  ↓
CPU1：专用入口 → 独立栈/每核初始化 → 就绪握手 → IDLE → 正常调度
                         两核通过 P4 跨核中断请求调度
```

- CPU0 独占 BSS 清零、堆、时钟、PSRAM、板级及全局驱动初始化；CPU1 不重入 `__esp_start()`。
- CPU1 设置独立的 SP、GP、异常向量、每核中断状态及配置所需的 TLS/MPU；接入 NuttX
  每核 IDLE 与在线时序。启动栈、握手状态和跨核锁置于内部 SRAM。
- 采用 P4 对应 revision 的 ROM/HAL 启核流程与跨核中断硬件，接入本树的
  `up_cpu_start()`、`up_send_smp_sched()`、`up_send_smp_call()` 和调度处理路径。
  不把 ECALL 当 IPI，不直接套用通用 MSIP 地址公式。
- `esp_irq.c` 拆分一次性的全局初始化和每核初始化，CPU1 不清空 CPU0 已注册的 IRQ。
  跨核中断每核独立安装、确认和清除；CPU 编号以实测 hart/core 映射核对。
- 首期外设 IRQ 和系统时基由 CPU0 承担，CPU1 通过 IPI 被唤醒；避免两核重复推进系统时间。
  IRQ 申请、释放及启停在所属核执行，跨核请求通过受控接口转交。
- 启核握手有明确超时；失败停止 SMP 启动并输出阶段信息，不在双核调度配置下静默退回单核。

## 4. 共享资源规则

- 跨核共享状态使用 NuttX 自旋锁、原子操作和内存屏障；仅关本核中断或使用 `volatile`
  不足以互斥。自旋锁内不等待信号量、不调用阻塞 I/O，不执行跨核同步等待。
- 核对 HAL 锁、堆、IRQ 分配器、GDMA、I2C/I2S、SDMMC 和驱动单例的初始化及清理路径；
  DMA 缓冲保持单一所有者，继续执行缓存同步，不能用 CPU 间可见性代替 DMA 一致性。
- Flash 擦写前，确认另一核进入内部 RAM 的安全等待区，完成双核缓存与中断协调；
  操作结束后对称恢复。停核代码、回调、栈及数据不能依赖 Flash/PSRAM。
- 复用并核验 HAL 的 IPC/Flash 保护机制，IPC 任务就绪前不进入需要它的擦写路径。
  相关修复以项目补丁或适配层保存，不直接修改 HAL 工作副本。
- 应用仍通过现有设备接口访问驱动，LVGL 保持单线程持有；本阶段不修改 cAGENT 或 UI 业务。

## 5. 改动范围

以下路径相对项目根目录；首期只接入最小 NSH 验证配置。

| 位置 | 工作内容 |
| --- | --- |
| `chips/esp32p4/common/espressif/esp_smp.c`、`esp_smp.h`、`esp_cpu1_start.S`、`esp_atomic64.c` | 次核入口、启核握手、P4 IPI 及 SMP 架构接口；RV32 的 64 位原子操作采用共享自旋锁 |
| `esp_start.c`、`esp_irq.c`、`esp_vectors.S`、`esp_idle.c` | 全局/每核初始化拆分、中断分发、IDLE 与唤醒核对 |
| `esp_spiflash.c`、`hal_esp32p4.mk/.cmake` 及项目 HAL 补丁目录 | IPC 构建与初始化接入、Flash/缓存双核保护 |
| 芯片 Kconfig、Make/CMake、板级链接脚本 | 两核配置、独立栈和内部 SRAM 放置；局部替换不适用的通用 SMP 实现，避免重复符号 |
| `board/esp32p4/esp32p4-function-ev-board/configs/smp/defconfig` | 最小 NSH 双核配置；启用 `CONFIG_SMP=y`、`CONFIG_SMP_NCPUS=2`，保留单核基线 |
| NuttX Apps 的 `smp`、`ostest`、`atomic`、`taskset` | 复用核编号、并行执行、跨核调用、亲和性及原子操作测试 |

复用工作区 `nuttx/arch/risc-v/src/common/` 与调度器的 SMP 契约；ESP32-S3
次核启动代码仅参考生命周期，寄存器、汇编及中断实现使用 P4 资料。

## 6. 实施与验收

| 阶段 | 工作 | 通过条件 |
| --- | --- | --- |
| M0 | 固定 NuttX、HAL、工具链、芯片 revision；核对通用 SMP 与 HAL 接口 | 明确启核/IPI/IPC 的实现归属及配置，单核基线保持可构建 |
| M1 | 最小双核启动、每核栈和中断；暂不启用 PSRAM 与复杂外设 | 两个 IDLE 就绪；分别绑定 CPU0/CPU1 的任务报告正确核号；冷启动及复位各 10 次通过 |
| M2 | IPI、抢占、跨核唤醒与亲和性 | 两方向信号量往返各 10000 次无丢失；CPU0 中断可唤醒 CPU1 高优先级任务；任务迁移与退出正常 |
| M3 | 原子、自旋锁、互斥锁、分配释放及系统时基 | 双核共享计数与数据校验一致；并行负载运行 30 分钟，无死锁、时间重复计数或栈越界 |
| M4 | PSRAM、IPC、LittleFS/Flash 擦写 | 另一核持续执行 XIP 代码并访问 PSRAM 时，擦写独立测试文件 100 轮校验一致；无缓存异常或持续内存增长 |
| M5 | 显示、摄像头、网络、音频逐项加入，再组合运行 | 与单核同功能配置比较吞吐、延迟、栈和堆；可用外设组合持续 1 小时无卡死、数据损坏或异常复位 |

每阶段保存配置、镜像校验值、核编号及串口结果。未完成单核验收的音频、microSD、BLE
先补齐各自基线，再加入 SMP 组合测试；双核加速比以同负载测量，不预设翻倍。

## 7. 依据

- 本地：`esp_head.S`、`esp_start.c`、`esp_irq.c`；工作区 `riscv_cpustart.c`、`riscv_smpcall.c`、`riscv_ipi.h`。
- [固定 HAL 的 P4 SMP 配置](https://github.com/espressif/esp-hal-3rdparty/blob/b90b1837cb5ad24747deb4c895246037cc206ce5/nuttx/esp32p4/include/sdkconfig.h)
- [乐鑫双核启动参考](https://github.com/espressif/esp-idf/blob/master/components/esp_system/port/cpu_start.c)与[跨核中断参考](https://github.com/espressif/esp-idf/blob/master/components/esp_system/crosscore_int.c)：实施时固定到匹配 HAL/revision 的版本。
