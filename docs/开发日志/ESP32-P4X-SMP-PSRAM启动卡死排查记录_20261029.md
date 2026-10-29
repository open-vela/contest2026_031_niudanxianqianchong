# ESP32-P4X SMP + PSRAM 启动卡死排查记录

日期：2026-10-29。状态：已修复并完成板端验证。

## 1. 背景与当前结论

用户此前提供的板测日志表明，`taskset 1 reboot`、`taskset 2 reboot` 均可触发
`SW_CPU_RESET`，重新启动至 `CPU1 ready` 和 NSH。该结果属于不启用 PSRAM 的 SMP
基线，尚不能证明加入外设后的稳定性。

本次按 PSRAM → 显示 → 触摸的顺序接入。启用 PSRAM 后，固件可以构建、烧录并进入
CPU0 的早期启动流程，但停在 PSRAM MMU 清表阶段，未到达 CPU1 启动和 NSH。

后续定位确认根因是 `bootloader_init_ext_mem()` 中 `mmu_ll_unmap_all(PSRAM0)` 在
PSRAM 控制器时钟初始化之前访问 S 总线 MMU 寄存器导致挂起。该问题与 SMP 无关，
单核 `smart_home`（同样启用 PSRAM）在同一位置卡死。修复后 SMP + PSRAM 和单核
SmartHome 均可正常启动，详见第 8 节。

## 2. 环境与改动

| 项目 | 本次记录 |
| --- | --- |
| 开发板 | ESP32-P4X-Function-EV-Board，P4 revision v3.2 |
| 目标标识 | MAC `e8:f6:0a:e3:a6:58`，USB Serial/JTAG `/dev/ttyACM0` |
| 项目分支 | `contest2026` |
| 项目 HEAD | `4b4bf44e5f41e9702eed2adeae556b33c184611d`，叠加未提交的 SMP 复位修复和本次改动 |
| HAL 基线 | `b90b1837cb5ad24747deb4c895246037cc206ce5`，应用项目补丁 |
| 烧录参数 | 偏移 `0x2000`，16MB / DIO / 80MHz，传输波特率 921600 |

本次新增内容：

- 增加默认关闭的 `CONFIG_ESP32P4_SMP_PSRAM` 实验开关，允许单独验证 SMP + PSRAM。
- 增加 [smp_psram](../../board/esp32p4/esp32p4-function-ev-board/configs/smp_psram/defconfig)、
  [smp_display](../../board/esp32p4/esp32p4-function-ev-board/configs/smp_display/defconfig)、
  [smp_touch](../../board/esp32p4/esp32p4-function-ev-board/configs/smp_touch/defconfig) 三级配置。
- 增加 [smp_psram 探针](../../app/smp_psram_probe/smp_psram_main.c)：分配 PSRAM，绑定两核
  并发写入各自区域，再交叉读取校验，不手工刷新缓存。

探针通过工作区软链接 `packages/demos/contest2026_031_smp_psram_probe` 接入应用构建。
显示与触摸目前仅准备配置，尚未构建或烧录。

## 3. 构建与复现

使用 `configs/smp_psram` 构建并烧录后，串口停在 `*** Booting NuttX ***`，
等待约 30 秒仍无 NSH，原计划执行的 `smp_psram 8 16` 未实际运行。

中途重新配置清理了 HAL 下载目录，随后 GitHub 克隆超时。改用固定提交的源码归档，
并补齐 mbedTLS 子模块 `582ff482038db6e4010dbf6f943d97b05ad06ea5` 后完成构建。
这解决了构建依赖获取问题，板端启动卡点仍然存在。

各次烧录均返回 `Hash of data verified`。ROM 的 `SHA-256 comparison failed`
提示与当前 Simple Boot 的 `--ram-only-header` 镜像模式一致，不能单凭此提示判断烧录损坏。

## 4. 定位过程

在 HAL 临时加入位于 IRAM 的 USB 字符标记，逐段缩小范围：

| 轮次 | 最后标记 | 定位结果 |
| --- | --- | --- |
| 初始固件 | `*** Booting NuttX ***` | 尚未进入正常系统启动输出 |
| debug1 | `4` | 停在 `bootloader_init_ext_mem()` 内 |
| debug2 | `4ciklmd` | Cache 初始化、模式设置、失效操作已返回，MMU 初始化未返回 |
| debug3 | `4ciklmdXBYZFG` | 64 KiB 页大小设置和 Flash MMU 清表已返回，PSRAM MMU 清表未返回 |
| debug4 | `4ciklmd1WXBYZFG` | PSRAM 系统时钟位原本为 1，显式使能模块时钟后仍复现 |

最后一次串口输出：

```text
entry 0x4ff47990
0123*** Booting NuttX ***
4ciklmd1WXBYZFG
```

当前卡点所在调用链：

```text
__esp_start()
  → bootloader_init()
    → bootloader_init_ext_mem()
      → cache_hal_init()                  已返回
      → mmu_hal_init()
        → mmu_ll_set_page_size(0, 64KiB)  已返回
        → mmu_hal_unmap_all()
          → mmu_ll_unmap_all(FLASH0)      已返回，输出 G
          → mmu_ll_unmap_all(PSRAM0)      未观察到返回，无 H
```

PSRAM 分支循环处理 1024 个 MMU 表项，涉及索引寄存器 `0x5008e380` 和内容寄存器
`0x5008e37c`。尚未定位到具体写操作或表项，也未取得 `mcause/mepc/mtval`，
因此还不能区分寄存器访问阻塞与早期异常。

## 5. 已确认与未确认

- 卡点发生在 CPU0 早期启动，尚未执行到 PSRAM 芯片初始化和 CPU1 启动，不能归因于
  双核缓存一致性或 PSRAM 数据读写时序。
- “PSRAM 模块时钟未开启”的简单假设未得到支持：读取时已开启，显式使能也未解决问题。
- 已备份的可启动 SMP 基线同样包含 PSRAM MMU 清表循环。仅发现该循环不足以解释差异，
  仍需对比配置、复位状态和初始化顺序。
- 探针在 Host 模拟环境通过 2 MiB、8 轮读写，并检查了分配失败、第二线程创建失败和
  非法参数路径；这些结果不构成真实 PSRAM 或硬件缓存验证。

临时标记和显式使能时钟只存在于构建目录中的 HAL，尚未形成项目补丁，不作为正式修复。

## 6. 下一步

1. 在 PSRAM MMU 索引写入、内容写入前后加最小标记，确定具体访问与循环位置。
2. 捕获早期异常寄存器，并与可启动基线对比 PSRAM 控制器时钟、复位和访问权限状态；
   补做同一 HAL 的单核 PSRAM 对照与冷启动验证。
3. 修复后移除临时标记，形成可重放补丁；验证 PSRAM 容量、双核交叉读写及两核复位回归。
4. PSRAM 通过后再执行屏幕测试图和触摸坐标测试，分别记录实际显示与触摸结果。

## 7. 现场与证据

当前板上仍是 debug4 调试固件，启动未进入 NSH；已暂停后续烧录，尚未恢复基线。
启动前保存的基线镜像与 Flash 读回内容前缀一致，可用于后续恢复。

记录目录：工作区 `out/p4x-smp-bringup-20261029-224110/`，本地证据未纳入 Git。

| 证据 | 文件 |
| --- | --- |
| 基线固件与 Flash 读回备份 | [baseline](../../../out/p4x-smp-bringup-20261029-224110/baseline/) |
| 首次 PSRAM 镜像信息 | [psram/image.json](../../../out/p4x-smp-bringup-20261029-224110/psram/image.json) |
| debug4 ELF、BIN、配置及临时 HAL 源码快照 | [psram-debug4](../../../out/p4x-smp-bringup-20261029-224110/psram-debug4/) |
| debug4 构建、烧录与启动记录 | [build](../../../out/p4x-smp-bringup-20261029-224110/build-psram-debug4.log)、[flash](../../../out/p4x-smp-bringup-20261029-224110/psram/flash-debug4.log)、[boot](../../../out/p4x-smp-bringup-20261029-224110/psram/boot-debug4.log) |
| Host 模拟结果 | [host-tests.log](../../../out/p4x-smp-bringup-20261029-224110/host-tests.log) |

当前 debug4 镜像大小为 340096 字节，SHA-256：

```text
fc9d6f597346661d8ffaee6fa99272ffb5a45cc1fc3d5704298a4096f2c6546e
```

它与首次 PSRAM 镜像不同；各自镜像信息分别保存在对应目录的 `image.json`。

## 8. 修复与验证（2026-10-29 完成）

### 8.1 根因

启用 `ESPRESSIF_SPIRAM` 后，`bootloader_init_ext_mem()` 依次执行：

```text
cache_hal_init()
mmu_hal_init()
  mmu_ll_set_page_size(0, 64KiB)
  mmu_hal_unmap_all()
    mmu_ll_unmap_all(FLASH0)   返回
    mmu_ll_unmap_all(PSRAM0)   挂起
```

`mmu_ll_unmap_all(PSRAM0)` 对 1024 个表项写 S 总线 MMU 索引/内容寄存器
（`0x5008e380` / `0x5008e37c`），此时 PSRAM 控制器时钟尚未初始化，写操作永久阻塞，
不是循环次数问题（等待 120 秒无进展）。

同一镜像在关闭 PSRAM 时可以启动，说明问题由 `ESPRESSIF_SPIRAM` 使能后的早期初始化
路径触发，而非 SMP 或双核缓存一致性。

### 8.2 修复

新增项目补丁
[0007-esp32p4-psram-mmu-unmap.patch](../../chips/esp32p4/common/espressif/patches/0007-esp32p4-psram-mmu-unmap.patch)：
在 `CONFIG_ESPRESSIF_SPIRAM` 使能时跳过 `mmu_ll_unmap_all(PSRAM0)`。

Simple Boot 由 ROM 直接加载应用，PSRAM 在 `esp_psram_chip_init()` 之前没有任何映射，
后续 `esp_mmu_map()` 会按需写入表项，因此这一步清表不是必需的。未启用 PSRAM 的配置
保持原有清表行为，SMP 基线不受影响。

### 8.3 板端验证

`smp_psram`（SMP + 32MB PSRAM）：

```text
I (304) esp_psram: Found 32MB PSRAM device
I (304) esp_psram: Speed: 200MHz
I (804) esp_psram: SPI SRAM memory test OK
ABCD[CPU0] ESP32-P4 SMP: CPU1 ready
NuttShell (NSH)
```

```text
nsh> free
      total       used       free    maxused    maxfree  nused  nfree name
   33554428      16900   33537528    8408176   33522584     15      4 Umem
     462068       3196     458872       6048     456912     32      2 Kmem

nsh> smp_psram 8 16
smp_psram: buffer=0x48002b00 bytes=8388608 rounds=16
smp_psram: worker CPU1 running on CPU1
smp_psram: worker CPU0 running on CPU0
smp_psram: CPU1 completed 16 rounds errors=0
smp_psram: CPU0 completed 16 rounds errors=0
smp_psram: PASS concurrent writes and cross-core reads
```

`taskset 3 ostest` 以 status 0 结束，`smp_call_test: Test success`；barrier、
sched_lock、vfork 全部通过。PSRAM 堆统计为 32MB `Umem`，说明 PSRAM 已并入用户堆。

单核 `smart_home`（同样启用 PSRAM）在应用同一补丁后也恢复启动，DSI  framebuffer
分配到 PSRAM（`base=0x48000200 bytes=2457600`），GT911 注册成功，LittleFS 挂载到
`/data`，进入 NSH。该结果同时修复了本次排查中发现的单核基线回归。

### 8.4 复现步骤

```sh
source myenv/bin/activate
# 探针应用通过工作区软链接接入构建
ln -sfn ../../contest2026_031_niudanxianqianchong/app/smp_psram_probe   packages/demos/contest2026_031_smp_psram_probe
./build.sh contest2026_031_niudanxianqianchong/board/esp32p4/esp32p4-function-ev-board/configs/smp_psram -j4
esptool --chip esp32p4 --port /dev/ttyACM0 --baud 921600 --after hard-reset   write-flash -fs 16MB -fm dio -ff 80m 0x2000 nuttx/nuttx.bin
```

### 8.5 遗留

- 跳过 PSRAM 清表是基于当前 HAL 与 rev3 启动时序的兼容处理；后续升级 HAL 时应重新
  评估，优先寻找在清表前正确初始化 PSRAM 控制器时钟的方案。
- PSRAM 双核压力测试目前为 8MB × 16 轮交叉读写；长时间负载、cache 一致性边界
  （DMA 缓冲、显示/摄像头帧缓冲）仍需在 G3/G5 阶段验证。
- 单核 `smart_home` 已恢复启动，但 UI、音频、摄像头功能未在本次逐一回归。
