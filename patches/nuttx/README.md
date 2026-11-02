# 本地 NuttX 兼容补丁

这些补丁针对本项目使用的 NuttX `drivers/audio/es8311.c`，按顺序应用：

1. `0001-es8311-single-byte-register-read.patch`：修复单字节寄存器读取的缓冲区越界。
2. `0002-es8311-mutex-and-log-formats.patch`：补充 `nuttx/mutex.h`，修复 `nxmutex_*` 未声明和链接失败，并使用 `PRIu32` 修复 MCLK、采样率的日志格式。
3. `0003-es8311-propagate-configure-errors.patch`：修复有效的采样率、位宽配置仍错误返回 `-ERANGE` 的问题，并保留 I2S 时钟或 codec 配置的真实错误码。
4. `0004-es8311-audio-smoke-diagnostics.patch`：增加音频 smoke test、codec/I2S/I2C 边界日志，用于定位配置阶段阻塞。
5. `0005-es8311-start-boundary-diagnostics.patch`：增加 codec 启动、worker 创建和首个音频缓冲提交边界日志，用于定位复位位置。
6. `0006-es8311-worker-boundary-diagnostics.patch`：增加 worker 首次处理和消息队列等待边界日志，用于定位启动后的卡死。
7. `0007-es8311-reduce-i2c-success-log.patch`：将逐笔 I2C 写成功日志降为 `DEBUG`，降低串口压力。
8. `0008-es8311-recover-from-submit-and-mq-errors.patch`：修复 worker 将负的消息队列错误误判为完整消息，并在 I2S 提交失败时回收 in-flight 计数和应用 buffer，避免停止或 drain 永久等待。
9. `0009-es8311-demote-bringup-diagnostics.patch`：将 ES8311 成功路径的 bring-up 诊断降为 `DEBUG`，避免无 syslog 缓冲时串口输出阻塞应用首包提交。

在尚未应用补丁的 NuttX 仓库中执行（本机工作区已经应用，不要重复执行）：

```sh
git apply --check ../contest2026_031_niudanxianqianchong/patches/nuttx/0001-es8311-single-byte-register-read.patch
git apply ../contest2026_031_niudanxianqianchong/patches/nuttx/0001-es8311-single-byte-register-read.patch
git apply --check ../contest2026_031_niudanxianqianchong/patches/nuttx/0002-es8311-mutex-and-log-formats.patch
git apply ../contest2026_031_niudanxianqianchong/patches/nuttx/0002-es8311-mutex-and-log-formats.patch
git apply --unidiff-zero --check ../contest2026_031_niudanxianqianchong/patches/nuttx/0003-es8311-propagate-configure-errors.patch
git apply --unidiff-zero ../contest2026_031_niudanxianqianchong/patches/nuttx/0003-es8311-propagate-configure-errors.patch
git apply --unidiff-zero --check ../contest2026_031_niudanxianqianchong/patches/nuttx/0004-es8311-audio-smoke-diagnostics.patch
git apply --unidiff-zero ../contest2026_031_niudanxianqianchong/patches/nuttx/0004-es8311-audio-smoke-diagnostics.patch
git apply --unidiff-zero --check ../contest2026_031_niudanxianqianchong/patches/nuttx/0005-es8311-start-boundary-diagnostics.patch
git apply --unidiff-zero ../contest2026_031_niudanxianqianchong/patches/nuttx/0005-es8311-start-boundary-diagnostics.patch
git apply --unidiff-zero --check ../contest2026_031_niudanxianqianchong/patches/nuttx/0006-es8311-worker-boundary-diagnostics.patch
git apply --unidiff-zero ../contest2026_031_niudanxianqianchong/patches/nuttx/0006-es8311-worker-boundary-diagnostics.patch
git apply --unidiff-zero --check ../contest2026_031_niudanxianqianchong/patches/nuttx/0007-es8311-reduce-i2c-success-log.patch
git apply --unidiff-zero ../contest2026_031_niudanxianqianchong/patches/nuttx/0007-es8311-reduce-i2c-success-log.patch
git apply --check ../contest2026_031_niudanxianqianchong/patches/nuttx/0008-es8311-recover-from-submit-and-mq-errors.patch
git apply ../contest2026_031_niudanxianqianchong/patches/nuttx/0008-es8311-recover-from-submit-and-mq-errors.patch
git apply --unidiff-zero --check ../contest2026_031_niudanxianqianchong/patches/nuttx/0009-es8311-demote-bringup-diagnostics.patch
git apply --unidiff-zero ../contest2026_031_niudanxianqianchong/patches/nuttx/0009-es8311-demote-bringup-diagnostics.patch
```

当前配置未启用 `CONFIG_LIBC_SEM_MUTEX_NOINLINE`，`nxmutex_init/lock/unlock` 是头文件中的内联实现。缺少该头文件时，编译器按隐式外部函数处理，随后链接失败；不需要新增互斥锁库或更改同步实现。

本机已有的补丁不要重复应用。新增单声道补丁和本轮板端验证见本文末尾，以及
[录放修复记录](../../docs/开发日志/ESP32-P4X-SMP-ES8311录放修复记录_20261102.md)。

## RISC-V 自定义 SMP

`0010-riscv-allow-custom-smp.patch` 为 Make/CMake 增加 `ARCH_CUSTOM_SMP` 构建判断。
P4 SMP 由本项目实现启核与 IPI；其他配置继续使用原有通用实现。
适用基线为 NuttX `106bf012365ed035c6d6c717e73448e76089d721`。
构建和板端验收见[独立 SMP 配置](../../board/esp32p4/esp32p4-function-ev-board/configs/smp/README.md)。

## ESP-NN / 工具链相关

10. `0010-riscv-allow-custom-smp.patch`：允许自定义 SMP 配置。
11. `0011-cmake-multi-ld-script.patch`：CMake 路径支持多个链接脚本。
12. `0012-espressif-riscv-toolchain.patch`：`CONFIG_ESP32P4_ESPRESSIF_TOOLCHAIN=y`
    时，Make（`arch/risc-v/src/common/Toolchain.defs`）与 CMake
    （`arch/risc-v/src/cmake/Toolchain.cmake`）都把工具链前缀切到
    `riscv32-esp-elf`。开关默认 `n`，关闭时行为与打补丁前完全一致。
    需要该开关的配置必须先准备乐鑫工具链，见
    `docs/开发日志/编译/ESP32-P4X-乐鑫工具链S0-S3接入记录.md`。

```sh
git apply --check ../contest2026_031_niudanxianqianchong/patches/nuttx/0012-espressif-riscv-toolchain.patch
git apply ../contest2026_031_niudanxianqianchong/patches/nuttx/0012-espressif-riscv-toolchain.patch
```

13. `0013-usrsock-guard-net-only-inline.patch`：把 `usrsock_sem_timedwait()`
    包进 `#ifdef CONFIG_NET`。该 inline 函数调用 `net_sem_timedwait2()`，
    而后者在 `include/nuttx/net/net.h` 里位于 `#ifdef CONFIG_NET` 之内。
    `drivers/drivers_initialize.c` 通过 `usrsock_rpmsg.h` 无条件引入本头文件，
    在 `CONFIG_NET=n` 的配置（如 `nsh`）下 GCC 14 报
    `implicit declaration of function net_sem_timedwait2` 错误（GCC 13 只是警告）。
    其余三个 inline（`usrsock_lock` / `usrsock_unlock` / `usrsock_mutex_timedlock`）
    只依赖 `nxrmutex_*` 与 `nxmutex_*`，保持无条件。

```sh
git apply --check ../contest2026_031_niudanxianqianchong/patches/nuttx/0013-usrsock-guard-net-only-inline.patch
git apply ../contest2026_031_niudanxianqianchong/patches/nuttx/0013-usrsock-guard-net-only-inline.patch
```


## ES8311 单声道配置

14. `0014-es8311-configure-i2s-channels.patch`：将应用申请的声道数传给
    `I2S_TXCHANNELS` / `I2S_RXCHANNELS`，检查实际返回的声道数。
    结合本项目 P4 RX 单声道槽选择，修复单声道录音夹入空槽、录放耗时减半的问题。

```sh
git apply --check ../contest2026_031_niudanxianqianchong/patches/nuttx/0014-es8311-configure-i2s-channels.patch
git apply ../contest2026_031_niudanxianqianchong/patches/nuttx/0014-es8311-configure-i2s-channels.patch
```

`0004`、`0005`、`0006` 已改用上下文补丁；`0004`、`0005` 不再混入比赛应用
的修改，应用诊断直接保存在本仓源码中。14 个补丁在 NuttX
`106bf012365ed035c6d6c717e73448e76089d721` 的干净文件上按序检查、应用通过，
重放后的 ES8311 文件与板端验证所编译的源码逐字节一致。

当前 `smart_home` 使用 4032 字节、2 个 codec 缓冲。P4 RX 超过单帧上限的请求
返回 `-EFBIG`，暂不支持一个接收请求跨多个 EOF。使用 `GETBUFFERINFO` 返回的
大小分配缓冲，避免硬编码 4096 字节。

15. `0015-libcxx-gcc14-builtin-trait.patch`：`libs/libxx/libcxx/Make.defs`
    在 `GCCVER >= 14` 时追加
    `_LIBCPP_WORKAROUND_OBJCXX_COMPILER_INTRINSICS`。

    GCC 14 拒绝在函数签名里使用内建 trait，而 libc++ 17 的
    `__filesystem/path.h:623` 用 `__remove_pointer_t<...>` 做返回类型，
    `libcxx/src/ios.instantiations.cpp` 因此报
    `error: use of built-in trait '__remove_pointer(...)' in function signature`。
    openvela 默认的 `riscv-none-elf` 是 GCC 13，不受影响；启用
    `CONFIG_ESP32P4_ESPRESSIF_TOOLCHAIN=y` 后用的是 Espressif
    `riscv32-esp-elf` 14.2，才会命中该问题。

    该宏由 libc++ 自己提供，作用是切到非内建 trait 的等价实现，
    不改变语义、不影响 GCC <= 13 的构建。

```sh
git apply --check ../contest2026_031_niudanxianqianchong/patches/nuttx/0015-libcxx-gcc14-builtin-trait.patch
git apply ../contest2026_031_niudanxianqianchong/patches/nuttx/0015-libcxx-gcc14-builtin-trait.patch
```
