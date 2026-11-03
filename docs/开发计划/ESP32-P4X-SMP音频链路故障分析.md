# ESP32-P4X SMP 音频链路故障分析

> 版本：v1.0（2026-11-01）
> 范围：`smart_home` 配置，`CONFIG_SMP=y`，ES8311 + I2S0 + GDMA
> 性质：静态链路分析 + 候选根因排序；结论以真机日志为准

## 一、结论摘要

| 项 | 状态 |
|---|---|
| 同配置关闭 ES8311/I2S0 | 可进 NSH，网络/LVGL/CSI/miloco 全部跑通，CPU1 有真实业务负载 |
| 同配置打开 ES8311/I2S0 | 无法进入 NSH |
| 已排除 | CPU1 启动链路、Flash/IPC 协同、PSRAM、DSI、GT911 —— 都在"关音频"的配置里跑通 |
| 当前判断 | 音频链路是唯一的 SMP 回归变量；根因未取证，本文给出 6 个候选与取证顺序 |

一句话：**不是 SMP 基线坏了，是音频驱动里几处"单核能凑合、双核会致命"的写法，在音频初始化第一次于双核环境下执行时被触发。**

## 二、链路全景

```text
音频命令/应用线程 (CPU0, 默认 cpuset)
  → /dev/audio/pcm0                  VFS
  → audio upper half                 nuttx/audio/audio.c（ioctl 持 upper->lock）
  → pcm_decode                       直通（CONFIG_AUDIO_FORMAT_RAW）
  → es8311 lower half                nuttx/drivers/audio/es8311.c
      ├─ worker 线程（SCHED_FIFO max-3）
      ├─ pendlock / doneq / mqueue
      └─ I2S_SEND → esp_i2s.c
  → GDMA 中断 → i2s_tx_worker (HPWORK)
  → ES8311 codec（I2C0，与 GT911 共享）→ NS4150B PA
```

| 层 | 上下文 | 关键资源 |
|---|---|---|
| audio upper | 应用线程（ioctl） | `upper->lock`（rmutex），跨整个 ioctl 持有 |
| es8311 | 独立 worker 线程 | `pendlock`、`doneq`、`inflight`、消息队列 |
| esp_i2s | worker 线程 + HPWORK + ISR | `slock`（spinlock）、`bufsem`、`priv->lock`（rmutex）、容器池 |
| GDMA | 中断 | 在注册时绑定到当前 CPU（bringup 时为 CPU0） |

## 三、SMP 与单核的差异点

### D1 自旋锁内做堆分配（P0）

```c
/* esp_i2s.c: i2s_buf_allocate() */
flags = spin_lock_irqsave(&priv->slock);
...
bfcontainer->dma_link[i] = (lldesc_t *)kmm_memalign(alignment, size);
```

- `kmm_memalign` 会取堆锁，而堆锁在 NuttX 里是 **rmutex**（`mm/mm_heap/mm.h:252`），不是自旋锁。
- **单核**：堆锁无竞争，虽然"关中断 + 拿互斥量"不合规，但能跑过去。
- **双核**：另一核持堆锁时，本核在**关中断状态**下阻塞；一旦形成锁序反转（对方在等本核持有的自旋锁），就是硬死锁；即使不死，也是一段超长的关中断窗口，可能触发看门狗。
- `i2s_buf_free()` 同样在自旋锁内 `kmm_free`（`esp_i2s.c:598-605`）。
- **触发点正是音频初始化**：`esp_i2sbus_initialize()` → `i2s_buf_initialize()` → 4 次 `i2s_buf_free()`。

### D2 没有超时兜底（P0）

- `i2s_buf_allocate()` 用 `nxsem_wait_uninterruptible(&priv->bufsem)`（`esp_i2s.c:540`）。
- `bfcontainer->timeout` 只在 `i2s_send` 里赋值（`esp_i2s.c:3220`），**全树没有任何地方消费它**。
- 单核：丢一次 DMA 完成，通常整机也快挂了，问题被掩盖。
- 双核：另一核的负载会改变中断时序，丢完成概率上升；一旦丢失，worker 永久阻塞，`es8311_stop()` 的 `pthread_join` 连带挂住调用者。

### D3 回调与 ioctl 跨核并发（P1）

- `audio_ioctl()` 获取 `upper->lock` 后**跨整个 switch 持有**（`nuttx/audio/audio.c:935` 起），期间调用 `lower->configure` / `enqueuebuffer`。
- 单核：lower 的回调只能等 ioctl 返回后再进。
- 双核：lower 的回调（DEQUEUE / IOERR / COMPLETE）可以在另一核立刻进入 `audio_callback`，与 ioctl 争同一把锁和同一份 `upper->status` / `apbs` 状态。
- 表现不是整齐的"挂"，而是引用计数错乱、DEQUEUE 与 STOP 交错、偶发丢缓冲。

### D4 中断与 worker 的亲和性（P1）

- GDMA 中断在 bringup 时 `esp_setup_irq()` 注册 → 落在 CPU0。
- I2S 完成回调投递到 HPWORK（单线程）。
- ES8311 worker 没有显式绑核，继承 `SMP_DEFAULT_CPUSET=0x1`（CPU0）。
- 目前"刚好都在 CPU0"，所以还能工作；一旦有人为了负载均衡把音频线程挪到 CPU1，就会变成跨核唤醒 + 优先级反转（worker 是 FIFO max-3）。

### D5 PSRAM 缓冲 + Flash 关 cache（P1）

- 音频 APB 用 `lib_umemalign()` 分配（`libs/libc/audio/lib_buffer.c:61`），走**用户堆**；本配置开了 PSRAM 用户堆，APB 可能落在 PSRAM。
- Flash 擦写会关闭**两个核**的 cache（`components/spi_flash/cache_utils.c`）。
- 若音频线程此时在另一核访问 PSRAM 缓冲 → 取指/取数异常。
- 目前音频线程在 CPU0，与执行 Flash 操作的是同一个核（调度器已停），所以暂未暴露；这属于"潜伏项"。

### D6 初始化时序（P0 触发条件）

```text
nx_start()
  ├─ up_cpu_start(1)          ← CPU1 先跑起来
  └─ init 线程 → board_late_initialize()
       └─ esp_bringup()
            └─ board_es8311_initialize()   ← 音频第一次在双核下初始化
```

音频是整个 bringup 里**唯一一个"在自己的初始化路径上大量做堆操作和中断注册"的子系统**，所以它对 D1/D4 最敏感。

## 四、为什么"关掉音频"就能启动

关掉 `ESP32P4_FUNCTION_EV_BOARD_AUDIO_ES8311` 后：

- `esp_i2sbus_initialize()` 不再执行 → `i2s_buf_initialize()` 这条 D1 路径不存在；
- codec 的 I2C 初始化、GDMA pair 申请、ES8311 worker 都不再创建。

这与"D1/D6 在初始化阶段触发"完全吻合。但**不能据此排除 D2/D3**——它们的触发点在 `AUDIOIOC_START` 之后，需要一次带日志的复现来区分。

## 五、取证方案（一次复现，四类证据）

前置：恢复 ES8311，但把 `board_es8311_initialize()` 从 bringup 移到 NSH 后手动触发（或直接用 `audio_smoke`），让系统先起来。

| # | 证据 | 用途 |
|---|---|---|
| 1 | 串口最后 5 行 | 区分"初始化挂"还是"START 后挂" |
| 2 | `ps` | 看 `es8311` worker 是否处于 WAIT/BLOCKED，卡在哪个信号量 |
| 3 | `dumpstack <pid>` | 取 worker / 主线程栈，定位到 `bufsem` / `upper->lock` / I2C |
| 4 | `irqinfo` | GDMA/I2S 中断计数是否还在增长，判断 DMA 是否真的停了 |

判定表：

| 最后日志 | 结论 |
|---|---|
| `ES8311 ready` 之前停住 | D1（I2S 容器初始化）或 GDMA 申请 |
| `ES8311 ready` 之后、`nsh>` 之前 | 后续域与音频的资源冲突（GDMA/中断） |
| `audio_smoke: play` 之后停住 | D2/D3（完成丢失或回调竞争） |

## 六、修复清单

| 优先级 | 项 | 做法 |
|---|---|---|
| P0-1 | 容器预分配 | `i2s_buf_initialize()` 一次性分配描述符；运行期 `i2s_buf_allocate/free` 只换链，不碰堆 |
| P0-2 | 等待可超时 | `bufsem` 改 `nxsem_tickwait`，超时走 IOERR 回收路径；`bfcontainer->timeout` 真正生效 |
| P0-3 | 去掉关中断内堆操作 | 与 P0-1 一起完成；`i2s_buf_free` 的 `kmm_free` 移出临界区 |
| P1-1 | 回调路径复核 | 评估 `upper->lock` 是否需要拆成"状态锁 + 引用计数原子操作" |
| P1-2 | APB 放内部 SRAM | 音频 APB 改用内核堆或显式内部 RAM 分配，规避 D5 |
| P1-3 | 音频亲和性显式化 | worker 与 GDMA 中断绑同一核，并在文档里写死这个约束 |

> 注：`CONFIG_AUDIO_BUFFER_NUMBYTES=4096` 是修 B-01（8192 > I2S DMA 8064 上限）用的，恢复音频时必须一并加回。

## 七、验收

| 项 | 标准 |
|---|---|
| 基本 | `audio_smoke play 3` / `record 10` 返回 OK，字节数正确 |
| SMP | 双核 100% 负载下播放 10 分钟无 xrun、无看门狗 |
| 并发 | 音频 + CSI 预览 + LittleFS 写 + WiFi 同时运行 30 分钟 |
| 回退 | `CONFIG_ESP32P4_SMP=n` 时行为与单核基线一致 |
