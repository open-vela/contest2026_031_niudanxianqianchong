# ESP32-P4X SMP + LittleFS 并发验证记录

日期：2026-10-29
状态：G2 完成，双核并发读写通过。
关联文档：
[ESP32-P4X-SMP-PSRAM启动卡死排查记录](ESP32-P4X-SMP-PSRAM启动卡死排查记录_20261029.md)

## 1. 目标与配置

在已验证的 `smp_psram` 基线上放开 SPI Flash MTD 与 LittleFS，验证两个 HP 核同时
访问同一文件系统时的正确性和稳定性。

新增配置 `configs/smp_fs`：

```text
CONFIG_ESP32P4_SMP_PSRAM=y
CONFIG_ESPRESSIF_SPIRAM=y
CONFIG_ESP32P4_SMP_SPIFLASH=y
CONFIG_ESPRESSIF_SPIFLASH=y
CONFIG_ESPRESSIF_SPIFLASH_LITTLEFS=y
CONFIG_ESPRESSIF_STORAGE_MTD_OFFSET=0x600000
CONFIG_ESPRESSIF_STORAGE_MTD_SIZE=0xa00000
CONFIG_BOARD_LATE_INITIALIZE=y
```

`CONFIG_BOARD_LATE_INITIALIZE` 必须打开，否则 `board_late_initialize()` 不会执行
`esp_bringup()`，`/dev/espflash` 不会注册，LittleFS 也无法挂载。

## 2. 代码改动

### 2.1 放开 SMP 下的 Flash 限制

- `chips/esp32p4/Kconfig`：`ESP32P4_SMP` 不再依赖 `!ESPRESSIF_SPIFLASH`，新增
  `ESP32P4_SMP_SPIFLASH` 实验开关（默认关闭）。
- `chips/esp32p4/common/espressif/Kconfig`：
  `ESPRESSIF_SPIFLASH` 增加 `depends on !SMP || ESP32P4_SMP_SPIFLASH`。
- `chips/esp32p4/common/espressif/esp_smp.c`：编译期检查改为只有启用 Flash 但未
  打开实验开关时报错；PM 仍保持禁止。

### 2.2 配置可复现性

`smp_psram` 与 `smp_fs` 原来的 defconfig 只有几行 `#include` 注释，实际依赖构建
目录里已有的 `.config`。一旦清空配置就无法复现。两者已改为自包含的完整 defconfig
（以 `configs/smp/defconfig` 为基线追加选项）。

### 2.3 并发测试应用

新增 `app/smp_flash_test`，通过工作区软链接接入构建：

```sh
ln -sfn ../../contest2026_031_niudanxianqianchong/app/smp_flash_test \
  packages/demos/contest2026_031_smp_flash_test
```

命令：`smp_flash_test [rounds] [chunk_kb]`，默认 8 轮、16 KiB。

两个线程分别绑定 CPU0 与 CPU1，每轮执行：

1. **私有文件**：`/data/smp_cpuN.bin` 写入 16 KiB 伪随机数据，`fsync`、重新打开、
   读回逐字节校验。
2. **共享文件**：两个线程向 `/data/smp_shared.bin` 追加 512 字节记录（CPU、轮次、
   FNV 校验和），barrier 后读取文件尾部两条记录，确认两个 CPU 的本轮记录都存在。
3. **目录抖动**：各自创建并删除一个临时文件，制造元数据并发更新。

I/O 缓冲区放在内部 SRAM（静态数组），避免 flash 擦写期间 cache 关闭影响 PSRAM
缓冲区。

## 3. 验证结果

### 3.1 启动与挂载

```text
ABCD[CPU0] ESP32-P4 SMP: CPU1 ready
[CPU0] P4X storage bring-up: offset=0x00600000 size=0x00a00000
[CPU0] P4X default Flash chip initialized: size=0x01000000
[CPU0] P4X MTD partition: flash_size=0x1000000 offset=0x600000 size=0xa00000 block=64 erase=4096
[CPU0] P4X LittleFS mounted: source=/dev/espflash mount=/data
NuttShell (NSH)
```

### 3.2 并发读写

```text
nsh> smp_flash_test 8 16
smp_flash: rounds=8 chunk=16384 bytes shared=/data/smp_shared.bin
smp_flash: cpu0 round 4/8 errors=0
smp_flash: cpu1 round 4/8 errors=0
smp_flash: cpu1 round 8/8 errors=0
smp_flash: cpu0 round 8/8 errors=0
smp_flash: done rounds=8 chunk=16384 errors=0 PASS
```

压力轮次：

```text
nsh> smp_flash_test 32 16
...
smp_flash: done rounds=32 chunk=16384 errors=0 PASS

nsh> ls -l /data
 -rw-rw-rw-       16384 smp_cpu0.bin
 -rw-rw-rw-       16384 smp_cpu1.bin
 -rw-rw-rw-       32768 smp_shared.bin
```

共享文件大小 32768 = 32 轮 × 2 条 × 512 字节，说明没有丢写或重复写。

### 3.3 复位回归

执行 `taskset 1 reboot` 与 `taskset 2 reboot`，两次都重新出现 `CPU1 ready`、
LittleFS 挂载日志并进入 NSH，flash 写操作没有破坏启动镜像或文件系统。

## 4. 过程中确认的限制

### 4.1 LittleFS 的 O_APPEND 只在 open 时定位

NuttX 的 littlefs 实现在 `fs/littlefs/lfs_vfs.c` 中仅在 `open()` 时对
`LFS_O_APPEND` 做一次 `lfs_file_seek(SEEK_END)`，`write()` 前不会重新定位。

因此两个长期打开的 fd 同时追加时，它们都以各自的 `f_pos` 写入同一偏移：

```text
期望每轮 +1024 字节，实际每轮只 +512 字节（后写覆盖先写）
```

这不是 flash 驱动或 SMP 的问题，单核上同样成立。当前测试采用以下两种安全模式：

- 每次追加前重新 `open(O_APPEND)` 并关闭。
- 同文件更新使用应用层互斥量串行化。

如果后续业务需要在两个核上并发写同一个文件，必须显式加锁或每次重新打开，不能依赖
`O_APPEND` 的原子性。

### 4.2 VFS 调用已有文件系统级互斥

`littlefs_read`/`write`/`seek` 都在 `fs->lock` 保护下执行，因此 VFS 层不会出现
并发导致的元数据损坏；问题只在于 `O_APPEND` 的偏移计算是每个文件对象独立的。

### 4.3 仍不允许 PM

`ESP32P4_SMP` 继续禁止 `CONFIG_PM`。flash 写入期间 cache 与中断状态特殊，休眠路径
需要单独验证。

## 5. 边界与后续

- 本轮验证的是 32 轮、每轮 16 KiB 的双核读写；长时间（30 分钟以上）磨损与掉电
  一致性未验证。
- 共享文件的“每轮 reopen + 应用锁”是可复现的安全用法，但不代表任意并发写同一
  文件都安全。
- 下一步 G3：在 `smp_fs` 基础上接入 DSI 显示与 GT911 触摸，UI 任务固定 CPU0。
- 摄像头/音频仍留在 G4/G5，PSRAM 帧缓冲的 DMA cache 一致性尚未验证。

## 6. 复现步骤

```sh
source myenv/bin/activate
ln -sfn ../../contest2026_031_niudanxianqianchong/app/smp_flash_test \
  packages/demos/contest2026_031_smp_flash_test
./build.sh contest2026_031_niudanxianqianchong/board/esp32p4/esp32p4-function-ev-board/configs/smp_fs -j4
esptool --chip esp32p4 --port /dev/ttyACM0 --baud 921600 --after hard-reset \
  write-flash -fs 16MB -fm dio -ff 80m 0x2000 nuttx/nuttx.bin
```

串口进入 NSH 后执行：

```text
nsh> smp_flash_test 32 16
nsh> ls -l /data
nsh> taskset 1 reboot
nsh> taskset 2 reboot
```
