# ESP32-P4X 实验性 SMP 配置

本配置用于 P4 revision 3.x、两个 HP 核、FLAT/M-mode 的 NSH 启动与调度验证。
使用 USB Serial/JTAG 控制台，保留原有单核 `nsh` 配置。

## 构建

在已映射到 `feat/p4x-drivers` 的独立 openvela 工作区执行：

```sh
source myenv/bin/activate
git -C nuttx apply --check ../contest2026_031_niudanxianqianchong/patches/nuttx/0010-riscv-allow-custom-smp.patch
git -C nuttx apply ../contest2026_031_niudanxianqianchong/patches/nuttx/0010-riscv-allow-custom-smp.patch
./build.sh contest2026_031_niudanxianqianchong/board/esp32p4/esp32p4-function-ev-board/configs/smp -j4
```

NuttX 补丁使通用 RISC-V 构建在 `ARCH_CUSTOM_SMP=y` 时使用芯片自己的启动与 IPI 实现，
需手工应用一次。HAL 补丁位于 `chips/esp32p4/common/espressif/patches/`，由 Make 在
每次 checkout 后按文件名顺序自动重放，匹配 `b90b1837cb5ad24747deb4c895246037cc206ce5`：
修复早期临界区、RTC 校准定时器初始化、HAL 单核启动路径、SMP IPC 初始化和双核复位。
从单核切换至 SMP 必须使用干净输出，不能复用单核对象。

## 板端验证

启动时应出现 `ESP32-P4 SMP: CPU1 ready`，随后进入 NSH。该提示只证明次核初始化握手。

```text
nsh> mount -t procfs /proc
nsh> ps
nsh> taskset 1 getprime
nsh> taskset 2 getprime
nsh> taskset 3 atomic
nsh> taskset 3 smp
nsh> taskset 3 ostest
```

- `ps` 应列出两个 IDLE；`getprime` 分别在单核亲和性下结束。
- `atomic` 应完成本树提供的 8/16/32 位原子运算测试；64 位并发一致性单独验收。
- `smp` 输出中必须观察到 CPU0 和 CPU1，全部 barrier 线程应正常退出。
- `ostest` 的跨核调用、同步及线程测试应通过，无断言、死锁或异常复位。
- 冷启动与复位各验证 10 次，再持续运行并行负载 30 分钟，检查堆和栈余量。

软件复位分别在 CPU0、CPU1 上执行，每条命令都会重启开发板：

```text
nsh> taskset 1 reboot
nsh> taskset 2 reboot
```

每次复位后应重新出现 CPU1 ready 并进入 NSH，不能卡死、连续异常复位或依赖看门狗恢复。

## 验证状态

2026-10-28：SMP 配置已在 ESP32-P4 Function EV Board（revision v3.2）上启动到 NSH，
串口输出 `[CPU0] ESP32-P4 SMP: CPU1 ready`；挂载 procfs 后 `ps` 列出 CPU0/CPU1 两个
IDLE。`taskset 3 smp` 观察到两核且全部线程返回 0，`taskset 3 atomic` 通过，
`taskset 3 ostest` 以 status 0 结束并包含 `smp_call_test: Test success`。
单核回归、CMake 构建、反复冷启动和长时间负载尚未验收；CMake 仍有公共板级 Kconfig
路径缺失、默认 HAL 版本与 Make 不一致的问题。

双核复位补丁依据 `CONFIG_SMP` 保留停另一核、清除 CPU1 入口和两核复位分支，
复位及外设清理函数放入 IRAM；HAL 启动阶段仍使用原有单核初始化路径。
2026-10-29：5 组主机调用顺序测试、SMP Make 构建及最终 ELF 的 IRAM 检查通过。
此项板端复位验证待完成；主机单核分支测试不代替单核固件回归。

主机测试在 HAL 补丁应用后，从 openvela 根目录执行：

```sh
python3 contest2026_031_niudanxianqianchong/scripts/test_p4x_smp_reset.py
```

## 当前边界

普通任务默认亲和性为 CPU0，`taskset 3` 显式允许测试程序及其子线程运行于两核。
当前限制 Flash MTD、PSRAM、PM 和影子栈，尚不包含存储擦写、休眠、显示/摄像头/音频
的 SMP 并发验收。只有板端结果通过后，才进入后续外设组合验证。
