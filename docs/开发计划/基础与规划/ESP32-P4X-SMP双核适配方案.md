# ESP32-P4X SMP 双核适配方案

> 版本：v1.0（2026-10-25）｜范围：ESP32-P4X Function EV Board / openvela
> 结论先行：**上游 NuttX 与本工程均为单核，SMP 是空白，不是配置开关。**本方案分四阶段，前两阶段用于低成本取证，后两阶段才动内核。

## 一、现状与差距

| 项 | 现状 |
|---|---|
| 运行核数 | 1（CPU0） |
| `CONFIG_SMP` | 未开启，`SMP_NCPUS=1`；43 个 defconfig 无一开启 |
| Kconfig | `ARCH_CHIP_ESP32P4` 有 `select ARCH_HAVE_MULTICPU`，**只代表选项可见** |
| 上游 apache/nuttx | 同样未实现：无 cpustart/smp/ipi 源文件，`riscv_ipi.h` 仍为 `#error "No IPI support"` |
| CPU1 | 从未释放（全树无 `esp_cpu_unstall()` 调用） |
| 已支持的"第二个核" | 仅 LP core（AMP 协处理器，跑独立固件，不参与 NuttX 调度） |

硬件能力齐备：`SOC_CPU_CORES_NUM=2`，HAL 已有 `esp_cpu_unstall()` / `cpu_utility_ll_*`。缺的是 OS 侧适配。

## 二、决定方案的硬件事实

| 事实 | 影响 |
|---|---|
| 无 CLINT MSIP 窗口 | 通用 `RISCV_IPI` 内存写路径不可用 |
| 有 `ETS_FROM_CPU_INTR0..3` | 跨核中断改用该中断源实现 IPI |
| 无 S-mode / 不走 OpenSBI | `CONFIG_ARCH_USE_S_MODE` 不可用 |
| L1 + L2 cache、PSRAM | SMP 下 cache 维护与 DMA 一致性风险上升 |
| PMU `cpu_sw_stall` | CPU1 释放入口，`hpcore1_stall_code` |

## 三、总体设计

```text
CPU0                                  CPU1
  up_cpu_start(1)                       riscv_cpu_boot(1)
    ├─ 释放 CPU1 (PMU unstall)   ──►      ├─ 初始化 per-CPU scratch
    └─ 发 IPI (FROM_CPU_INTR0)            ├─ 使能本地中断
                                          └─ nx_idle_trampoline()
  SMP 调度 / spinlock / smp_call  ◄──►  IPI 双向：
                                          CPU0→CPU1: FROM_CPU_INTR0
                                          CPU1→CPU0: FROM_CPU_INTR1
```

IPI 不满足 `riscv_ipi.h` 的两种既有形态，需要给通用层加一个弱符号钩子：

```c
#elif defined(CONFIG_ARCH_HAVE_CUSTOM_IPI)
  riscv_ipi_send_arch(cpu);      /* chip 实现，写 FROM_CPU_INTR 触发寄存器 */
```

该改动先是本地 patch，稳定后再向上游提 RFC。

## 四、阶段划分

| 阶段 | 目标 | 交付物 | 退出条件 | 预估 |
|---|---|---|---|---|
| S0 | 释放 CPU1，裸跑 | `esp32p4_cpu1.c` + 启动分流 | 串口打印 `CPU1 alive, hart=1` | 1–2 人日 |
| S1 | 单向 IPI | `esp32p4_ipi.c` + 测试命令 | 核 0 触发 10000 次，核 1 计数零丢失 | 2–3 人日 |
| S2 | 打开 SMP | 内核适配 + defconfig | `ostest`、`smp`、`sched` 全过 | 5–10 人日 |
| S3 | 域回归 | 七域验证记录 | 48 h 稳定，帧率/音频无回归 | 3–5 人日 |

**S0 的关键未知**：CPU1 释放后从哪个入口执行。两种可能：

1. 与 CPU0 共用复位向量 → 在 `esp_start.c` 开头按 `mhartid` 分流，CPU1 进 `riscv_cpu_boot()` 等待 IPI；
2. 进入 ROM → 需查 PMU/CLKRST 的核 1 入口寄存器或 ROM 接口，在 `up_cpu_start()` 中先写入口再释放。

S0 的第一件事就是把这个确认下来，再写后续代码。

## 五、改动清单（vendor 路线）

| 文件 | 改动 | 阶段 |
|---|---|---|
| `chips/esp32p4/common/espressif/esp32p4_cpu1.c` | 新增：CPU1 入口、per-CPU 栈、idle 跳转 | S0 |
| `chips/esp32p4/common/espressif/esp_start.c` | 开头按 hartid 分流 | S0 |
| `chips/esp32p4/common/espressif/esp32p4_ipi.c` | 新增：`FROM_CPU_INTR0/1` 注册、发送、清除 | S1 |
| `chips/esp32p4/common/espressif/esp_irq.c` | IPI 中断源接入 per-CPU 分发 | S1 |
| `nuttx/arch/risc-v/src/common/riscv_ipi.h` | 增加 `CONFIG_ARCH_HAVE_CUSTOM_IPI` 分支 | S1 |
| `chips/esp32p4/Kconfig` | 新增 `ARCH_HAVE_CUSTOM_IPI`、SMP 依赖 | S1 |
| `chips/esp32p4/common/Make.defs` | 新文件仅 `CONFIG_SMP=y` 时编译 | S0 |
| `configs/*/defconfig`（新增 `smp`） | `SMP=y, SMP_NCPUS=2, SMP_IDLETHREAD_STACKSIZE` | S2 |

> 所有新增文件用 `#ifdef CONFIG_SMP` 包裹；`CONFIG_SMP=n` 时产物与现在完全一致，保证可回退。

## 六、关键技术点

1. **每核栈**：CPU1 使用 `CONFIG_SMP_IDLETHREAD_STACKSIZE`，栈放内部 SRAM，不放 PSRAM。
2. **中断亲和**：S2 阶段先把 DSI/GDMA/CSI/SDIO 中断固定到 CPU0；音频、LVGL 渲染线程再考虑迁到 CPU1。P4 的 ESP 中断矩阵本身不区分核，需在 `esp_setup_irq()` 层指定 CPU。
3. **定时器**：确认 `riscv_mtimer` 在 SMP 下 per-hart 初始化；系统 tick 只由 CPU0 产生，CPU1 走 IPI 广播。
4. **cache**：DSI/CSI 已用 `esp_cache_msync`，SMP 下需保证 clean/invalidate 覆盖两核共享的 PSRAM 区域，禁止在持 spinlock 时做整帧 clean。
5. **调度启动顺序**：CPU1 必须在 `nx_start()` 完成调度器初始化后由 `up_cpu_start()` 拉起，不能早于 idle 线程创建。
6. **既有缺陷**：S2 前必须修掉音频 B-03（自旋锁内堆分配）与 B-04（不可中断等待），否则双核下会放大成必现挂死。

## 七、验收矩阵

| 阶段 | 用例 | 通过标准 |
|---|---|---|
| S0 | 上电启动 | `CPU1 alive` 与 `nsh>` 均出现，系统不进 panic |
| S1 | IPI ping-pong | 10000 次往返，无丢、无重复、无看门狗 |
| S2 | `ostest` / `smp` / `sched` | 全部 PASS；双核 100% 负载 1 h 无异常 |
| S3 | 七域回归 | DSI 帧率、CSI 30 fps、音频无 xrun、WiFi TLS 正常 |
| S3 | 稳定性 | 48 h 连续运行，内存无泄漏 |

## 八、风险

| 风险 | 级别 | 应对 |
|---|---|---|
| CPU1 入口机制不明 | 高 | S0 先验证，不通过则方案暂停 |
| 需改通用 `riscv_ipi.h` | 中 | 先本地 patch，向上游提 RFC |
| 驱动并发缺陷被放大 | 高 | 先修 B-03/B-04，再开 S2 |
| 性能收益不及预期 | 中 | 先做带宽剖析；PSRAM 瓶颈不因核数改善 |
| 中断亲和配置错误 | 中 | 逐域单独回归，保留单核基线对比 |

## 九、结论与建议

- **比赛交付窗口内只做 S0/S1**：能拿出"CPU1 已启动 + IPI 打通"的真机证据，成本 3–5 人日，风险可控。
- **S2/S3 单独立项**：这是上游空白，工作量 2–3 周，且需要先清理现有驱动缺陷。
- 不建议把"SMP 已适配"写进当前作品描述；可写"完成双核启动与核间通信预研，SMP 调度未启用"。
