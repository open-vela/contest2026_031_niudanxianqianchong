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

本机 NuttX 工作区已经按上述顺序应用九个音频补丁，不要重复执行。固件编译及板端录放音仍需验证。

## RISC-V 自定义 SMP

`0010-riscv-allow-custom-smp.patch` 为 Make/CMake 增加 `ARCH_CUSTOM_SMP` 构建判断。
P4 SMP 由本项目实现启核与 IPI；其他配置继续使用原有通用实现。
适用基线为 NuttX `106bf012365ed035c6d6c717e73448e76089d721`。
构建和板端验收见[独立 SMP 配置](../../board/esp32p4/esp32p4-function-ev-board/configs/smp/README.md)。
