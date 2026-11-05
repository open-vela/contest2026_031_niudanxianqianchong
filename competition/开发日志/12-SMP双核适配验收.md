# SMP 双核适配验收（ESP32-P4X · HP CPU0 + HP CPU1）

| 元信息 | 内容 |
| --- | --- |
| 适配对象 | ESP32-P4 HP 双核（RISC-V 400 MHz × 2）跨核调度 |
| 适配方式 | Route A：`chips/esp32p4/` 实现 CPU1 启动与跨核调度移植 + NuttX `ARCH_CUSTOM_SMP`；板级无特殊改动 |
| 当前真机状态 | **通过**：双核调度、CPU1 独占、真跨核并发、跨核调用（含中断上下文）、LittleFS 双核并发 50 轮、ostest 全量 |
| 关键代码入口 | `chips/esp32p4/Kconfig`（`ESP32P4_SMP` 系列）、NuttX `arch/risc-v` 自定义 SMP 补丁、配置 `configs/smp*` |
| 验证日期与固件 | 2026-11-04，`contest2026` @ `37bb1f0`，`configs/smp_fs` 与 `configs/smp_smart_home` |

## 一、适配背景与目标

ESP32-P4 有双核 HP CPU（各 400 MHz RISC-V）加一个 LP 核。NuttX 默认单核运行，
本项目通过 `CONFIG_ARCH_CUSTOM_SMP` 打通 CPU1 启动与跨核调度。

SMP 适配的难点不在"点亮 CPU1"，而在**共享资源的并发正确性**：

| 资源 | 并发风险 |
| --- | --- |
| PSRAM | CPU0 初始化后才能映射，CPU1 不能重复初始化 |
| SPI Flash / LittleFS | 两个核同时擦写会破坏 cache 一致性 |
| DMA2D / PPA | 硬件单任务，跨核提交会互相覆盖 |
| 中断控制 | 跨核 IPI、中断亲和性 |

因此适配拆成三个独立选项，逐级放开：

```text
CONFIG_ESP32P4_SMP=y                # 基础双核调度
CONFIG_ESP32P4_SMP_PSRAM=y          # CPU0 先初始化 PSRAM 再启动 CPU1
CONFIG_ESP32P4_SMP_SPIFLASH=y       # Flash MTD / LittleFS 双核安全
CONFIG_ESP32P4_SMP_GRAPHICS_CPU0=y  # DMA2D/PPA 限定 CPU0
```

## 二、平台就绪

```text
nsh> uname -a
NuttX 0.0.0 106bf012365-dirty Nov  4 2026 17:30:01 risc-v esp32p4-function-ev-board

nsh> ps
  PID GROUP CPU(AFFINITY) PRI POLICY TYPE    STATE     EVENT      STACK  USED  COMMAND
    0     0 000(0x00000001)  0 FIFO   Kthread Assigned          0004064 000748  CPU0 IDLE
    1     0 001(0x00000001)  0 FIFO   Kthread Running           0004064 000332  CPU1 IDLE
    3     0 ---(0x00000001) 255 RR     Kthread Waiting Semaphore 0001216 000276  ipc0 0
    4     0 ---(0x00000002) 255 RR     Kthread Waiting Semaphore 0001208 000276  ipc1 1
    5     5 000(0x00000001) 100 RR     Task    Running           0004024 0001700 nsh_main
```

关键观察：

- `CPU0 IDLE` 与 `CPU1 IDLE` 两个 idle 线程同时存在，说明两个核都在跑；
- `ipc0` 绑定 CPU0、`ipc1` 绑定 CPU1，跨核中断通道已建立；
- 亲和性列显示 `0x00000001` / `0x00000002`，调度掩码正确。

## 三、板端验证结果

### 3.1 单核基线

```text
nsh> smp
  Main[0]: Running on CPU0
  Main[0]: Initializing barrier
  Main[0]: Thread 1 created
  ... Thread 8 created
Thread[1]: Started
... Thread[8]: Started
Thread[1]: Running on CPU0
... Thread[8]: Running on CPU0
Thread[1]: Calling pthread_barrier_wait()
... Thread[8]: Calling pthread_barrier_wait()
Thread[8]: Back with ret=PTHREAD_BARRIER_SERIAL_THREAD (I AM SPECIAL)
Thread[1]: Back with ret=0 (I am not special)
... Thread[7]: Back with ret=0 (I am not special)
  Main[0]: Thread 1 completed with result=0
... Thread 8 completed with result=0
```

8 个 pthread 全部完成，`pthread_barrier_wait()` 正确：**恰好一个**线程返回
`PTHREAD_BARRIER_SERIAL_THREAD`，其余返回 0。

### 3.2 CPU1 独占

```text
nsh> taskset 2 smp
  Main[0]: Running on CPU1
Thread[1]: Running on CPU1
... Thread[8]: Running on CPU1
  Main[0]: Thread 1 completed with result=0
... Thread 8 completed with result=0
```

CPU1 可独立承担完整的多线程负载，不依赖 CPU0。

### 3.3 真跨核并发

前两项都是单核，只有把 affinity 设为两核才能验证并发：

```text
nsh> taskset 3 smp
  Main[0]: Running on CPU0
Thread[1]: Running on CPU1
Thread[2]: Running on CPU1
Thread[3]: Running on CPU0
Thread[4]: Running on CPU1
Thread[5]: Running on CPU0
Thread[6]: Running on CPU1
Thread[7]: Running on CPU1
Thread[8]: Running on CPU0
  Main[0]: Thread 1 completed with result=0
... Thread 8 completed with result=0
```

8 个线程**真实分布在两个核上**，跨核 `pthread_barrier_wait()` 正常收敛，
无死锁、无丢失唤醒。这是 SMP 适配最关键的一项证据。

### 3.4 跨核调用与中断上下文

`ostest` 中的 `smp_call_test`：

```text
smp_call_test: Test start
smp_call_test: Call cpu 0, nowait
smp_call_test: Call cpu 0, wait
smp_call_test: Call cpu 1, nowait
smp_call_test: Call cpu 1, wait
smp_call_test: Call multi cpu, nowait
smp_call_test: Call in interrupt, wait
smp_call_test: Call multi cpu, wait
smp_call_test: Test success
```

覆盖：单核调用、双核广播、nowait/wait 两种模式、**中断上下文内的跨核调用**。

### 3.5 ostest 全量

```text
user_main: Exiting
ostest_main: Exiting with status 0
```

各子项 `nerrors=0`，包括 cond、robust mutex、cancel、signal、timer、vfork、waitpid。

### 3.6 LittleFS 双核并发

Flash 是 SMP 里最容易出问题的资源（cache 与 IPC 锁竞争）。测试内容：
每 CPU 独立文件、带 CPU 槽位的共享文件、目录创建/删除抖动。

8 轮：

```text
nsh> smp_flash_test
smp_flash: rounds=8 chunk=16384 bytes shared=/data/smp_shared.bin
smp_flash: cpu1 round 8/8 errors=0
smp_flash: cpu0 round 8/8 errors=0
smp_flash: done rounds=8 chunk=16384 errors=0 PASS
```

加压 50 轮：

```text
nsh> smp_flash_test 50
smp_flash: cpu1 round 48/50 errors=0
smp_flash: cpu0 round 48/50 errors=0
smp_flash: done rounds=50 chunk=16384 errors=0 PASS
```

50 轮 0 错误。每轮每 CPU 写 16 KB，即总计约 1.6 MB 的并发 Flash 读写。

### 3.7 计算负载

```text
nsh> getprime 8
thread #0 started, looking for primes < 10000, doing 10 run(s)
... thread #7 started
thread #0 finished, found 1230 primes, last one was 9973
... thread #7 finished, found 1230 primes, last one was 9973
Done
getprime took 4700 msec
```

8 线程在双核上完成，每个线程结果一致（都是 1230 个素数，最后一个 9973），
说明没有出现数据竞争导致的错误结果。

## 四、内存基线

`smp_fs` 固件启动后：

```text
      total       used       free    maxused    maxfree  nused  nfree name
   33554428      27948   33526480      52072   33518040     20      3 Umem
     410484       7188     403296      11776     402080     50      4 Kmem
```

`smp_smart_home` 固件（带显示、音频、WiFi、PPA）：

```text
   33554428    2465220   31089208    2465600   31089208     16      1 Umem
     192076      11036     181040      15208     177248    100      2 Kmem
```

Umem 33 MB 是 PSRAM 用户堆，Kmem 是内部 SRAM 内核堆。跑完 PPA 150 轮后两者均无变化。

## 五、一个操作细节

`taskset` 的 cpuset 参数用 **`atoi` 十进制**解析：

| 写法 | 结果 |
| --- | --- |
| `taskset 2` | 只在 CPU1（二进制 10） |
| `taskset 3` | 两核均可（二进制 11） |
| `taskset 0x2` | 报 `invalid cpuset 0x2` |

第一次误用十六进制，得到的是"无效参数"而非"绑核失败"，容易误判为 SMP 不支持。
出处：`apps/system/taskset/taskset.c` 的 `get_cpuset()` 使用 `atoi`。

## 六、未覆盖项

以下内容本次没有验证，不据此判定通过：

- PSRAM 探针 `smp_psram`（需 `configs/smp_psram`，本次未构建）；
- CPU1 上运行 DMA2D/PPA（当前设计限定 CPU0）；
- CPU1 上运行 LVGL UI（当前 UI 固定 CPU0）；
- 长时间（小时级）双核稳定性；
- 两个核同时跑重计算 + Flash + 网络的三重压力；
- 跨核 IPC 在高负载下的延迟测量。

## 七、复现步骤

```sh
cd /home/arongw/openvela
./contest2026_031_niudanxianqianchong/scripts/build_p4x.sh smp_fs -j2
myenv/bin/esptool --chip esp32p4 --port /dev/ttyACM0 --baud 921600 \
  write-flash -fs 16MB -fm dio -ff 80m 0x2000 nuttx/nuttx.bin
```

板端：

```text
smp
taskset 2 smp
taskset 3 smp
ostest
smp_flash_test
smp_flash_test 50
getprime 8
```

判据：`taskset 3 smp` 中线程分布在 CPU0 与 CPU1；`smp_flash_test 50` 输出
`errors=0 PASS`；`ostest` 以 `status 0` 退出。

## 八、结论

SMP 双核适配在 ESP32-P4 revision v3.2 上完成验证：CPU1 可独立运行，两核可真实并发，
跨核 barrier 与跨核调用正确，LittleFS 双核并发 50 轮无错误，ostest 全量通过。

PPA 与 SMP 的共存也在 `smp_smart_home` 上验证（图形限定 CPU0），见 10 号文档。

PSRAM 独立探针、CPU1 图形/UI、长时间稳定性留待后续。
