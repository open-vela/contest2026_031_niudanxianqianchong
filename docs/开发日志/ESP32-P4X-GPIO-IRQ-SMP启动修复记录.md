# ESP32-P4X GPIO IRQ 与 SMP 启动修复记录

日期：2026-11-01。状态：主机测试、补丁重放和对象编译通过，待用户完整构建及板端验收。

## 故障与证据

- 用户实测：双核 SMP、音频关闭时，GPIO IRQ 关闭可以启动，单独开启 GPIO IRQ 后启动无响应。
- 此前故障镜像的 JTAG 取证捕获了 `gpio_uninstall_isr_service()` 将内核堆指针交给用户堆释放；随后 CPU0 SMP 中断分配失败并进入 panic，CPU1 尚未启动。
- 源码显示 GPIO ISR 服务在 `up_irqinitialize()` 安装，此时阻塞式 IPC 所依赖的系统就绪条件尚未满足。IPC 原始返回码尚未在旧镜像中直接捕获。
- 新的 GPIO-only 镜像尚未重新取证，不能仅凭无响应认定停留地址与旧镜像相同。

## 修复顺序

1. 新增 HAL 补丁 `0009-nuttx-gpio-isr-early-init.patch`，将 ISR 表的两处普通 `free()` 改为 `heap_caps_free()`，覆盖安装失败卸载和并发安装失败清理。
2. 仅在 NuttX 系统尚未就绪、注册目标为当前 CPU 时直接注册 GPIO ISR；运行期和跨核调用保留原 IPC 路径，不放宽 IPC 的系统就绪检查。
3. HAL 保留 IPC/中断分配的原始错误。`esp_gpioirqinitialize()` 返回负 errno 并用 `syslog(LOG_ERR, ...)` 记录原始错误；调用方检查失败并进入 panic，避免静默继续启动。

HAL 副本由构建系统管理，持久修改保存在上述补丁中。Make/CMake 现有补丁发现机制会加载该补丁；Make 的补丁时间戳使 HAL 对象重新编译。本轮未修改音频或 SMP 配置。

## 已完成验证

- 固定 HAL `b90b1837cb5ad24747deb4c895246037cc206ce5` 上，0001–0009 顺序应用及已应用检查通过。
- `python3 scripts/test_p4x_gpio_irq_init.py`：三种模式通过；覆盖早期本核、运行期本核/跨核、IPC 未就绪、内存及中断分配失败、失败后重试、并发安装清理和初始化失败停止执行。测试编译实际函数，硬件、分配器和 IPC 使用主机替身。
- `esp_gpio.c`、`esp_irq.c`、HAL `gpio.c` 使用当前 P4 配置和 `riscv-none-elf-gcc` 编译为独立对象，通过；未链接、未替换 `nuttx.bin`。
- 验证产物与原有未提交文件备份：`out/gpio-irq-smp-fix-20261101/`，位于外层 openvela 工作区。

## 用户验收

在外层 `/home/arongw/openvela` 执行：

```bash
./build.sh \
  contest2026_031_niudanxianqianchong/board/esp32p4/esp32p4-function-ev-board/configs/smart_home \
  -j2
```

先确认最终配置保持双核 SMP、GPIO IRQ 开启，音频与 I2S0 关闭；烧录本次构建镜像后，确认 `ESP32-P4 SMP: CPU1 ready` 和 `nsh>`，串口可以执行命令。随后对已核实接线的 GPIO 进行中断回调验收。启动成功不等于 GPIO 中断功能验收通过。

GPIO 阶段通过后再恢复音频。此前 `audio_smoke` 运行后卡死的问题仍需独立检查，不能以本修复代替录放验收。
