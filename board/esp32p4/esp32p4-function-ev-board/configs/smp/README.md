# ESP32-P4X 实验性 SMP 配置

本配置用于 P4 revision 3.x、两个 HP 核、FLAT/M-mode 的 NSH 启动与调度验证。
使用 USB Serial/JTAG 控制台，保留原有单核 `nsh` 配置。

## 构建

在已映射到 `feat/p4x-drivers` 的独立 openvela 工作区执行：

```sh
source myenv/bin/activate
git -C nuttx apply --check ../contest2026_031_niudanxianqianchong/patches/nuttx/0010-riscv-allow-custom-smp.patch
git -C nuttx apply ../contest2026_031_niudanxianqianchong/patches/nuttx/0010-riscv-allow-custom-smp.patch
git -C contest2026_031_niudanxianqianchong/chips/esp32p4/esp-hal-3rdparty apply --check ../../../patches/esp-hal/0001-nuttx-ipc-serialize-smp-initialization.patch
git -C contest2026_031_niudanxianqianchong/chips/esp32p4/esp-hal-3rdparty apply ../../../patches/esp-hal/0001-nuttx-ipc-serialize-smp-initialization.patch
./build.sh contest2026_031_niudanxianqianchong/board/esp32p4/esp32p4-function-ev-board/configs/smp -j4
```

补丁使通用 RISC-V 构建在 `ARCH_CUSTOM_SMP=y` 时使用芯片自己的启动与 IPI 实现。
HAL 补丁匹配 `b90b1837cb5ad24747deb4c895246037cc206ce5`，接入本树任务创建接口，
修复 IPC 首次初始化互斥和参数发布。已应用时不重复执行；从单核切换至 SMP 必须使用干净输出，不能复用单核对象。

## 板端验证

启动时应出现 `ESP32-P4 SMP: CPU1 ready`，随后进入 NSH。该提示只证明次核初始化握手。

```text
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

## 验证状态

2026-10-28：SMP 配置已通过 Make 编译、链接和镜像生成；ELF 已核对次核入口、
IPI 符号与两核中断栈。尚未烧录，单核回归与 CMake 构建尚未验收。
CMake 仍有公共板级 Kconfig 路径缺失、默认 HAL 版本与 Make 不一致的问题。

## 当前边界

普通任务默认亲和性为 CPU0，`taskset 3` 显式允许测试程序及其子线程运行于两核。
当前限制 Flash MTD、PSRAM、PM 和影子栈，尚不包含存储擦写、休眠、显示/摄像头/音频
的 SMP 并发验收。只有板端结果通过后，才进入后续外设组合验证。
