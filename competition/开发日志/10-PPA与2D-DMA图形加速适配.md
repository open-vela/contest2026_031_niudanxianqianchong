# PPA 与 2D-DMA 适配（ESP32-P4X · 图像加速）

| 元信息 | 内容 |
| --- | --- |
| 适配对象 | ESP32-P4 片内 2D-DMA（DMA2D）与 PPA（Pixel Processing Accelerator） |
| 适配方式 | Route A：芯片层 `chips/esp32p4/common/espressif/esp_dma2d.c` + `esp_ppa.c`（共享 `esp_graphics_context.h` / `esp_graphics_priv.h` / `esp_graphics_validate.c`）；板级不参与；应用层 `app/graphics_probe/` 独立验证 |
| 当前真机状态 | **通过**：`graphics_probe` 五项（copy/fill/srm/blend/convert）全 PASS；`all 100` 跑满 100 轮 500 项 0 失败；150 轮压测前后堆用量零变化 |
| 关键代码入口 | `chips/esp32p4/common/espressif/esp_dma2d.c`、`esp_ppa.c`、`app/graphics_probe/graphics_probe_main.c`、`board/esp32p4/esp32p4-function-ev-board/configs/{dma2d,ppa,smp_smart_home}` |
| 验证日期与固件 | 2026-11-04，`contest2026` @ `37bb1f0`，ESP32-P4 revision v3.2 |

## 一、适配背景与目标

ESP32-P4 比 ESP32-S3 多出两类图像加速硬件：2D-DMA（DMA2D）与 PPA。二者共用同一个
DMA2D 通道管理器，在 NuttX 侧由 `esp_graphics` 统一抽象。

本项目适配这两个硬件的目的有两个：

1. **降低 CPU 占用**：UI 刷新、摄像头预览缩放等矩形搬运若走 CPU memcpy，
   在 1024×600 RGB565 下一帧 1.2 MB，开销明显；
2. **为摄像头预览加速预留通路**：`SMART_HOME_CAMERA_PPA` 用 PPA 做缩放。

适配遵循两条边界：

- 不引入 FreeRTOS 上层驱动，只用 ESP-IDF HAL 的寄存器、描述符与 LL 接口；
- PPA 与 2D-DMA **只能有一个未回收任务**，不设计并发提交。

## 二、硬件能力与软件封装

### 2.1 支持的操作

| 操作 | 命令 | 支持范围 |
| --- | --- | --- |
| RGB565 二维复制 | `copy` | 不同 stride、源/目标 ROI、最多 16 个链式描述符，每段 128 行 |
| 填充 | `fill` | RGB565、BGR888、ARGB8888，颜色统一为 `0xAARRGGBB` |
| 缩放/旋转/镜像 | `srm` | Q4 缩放系数（16 = 1 倍）、逆时针 0/90/180/270 度、X/Y 镜像 |
| 双图层混合 | `blend` | 三种 RGB 格式，固定 alpha 或保留像素 alpha，straight alpha |
| 格式/颜色空间转换 | `convert` | 三种 RGB 格式互转；UYVY YUV422 转 RGB，BT.601 limited range |

### 2.2 完成与错误路径

- 完成通知走 DMA2D 完成中断，中断内只写回结果，不做内存分配；
- PPA 参数错误通过错误中断返回 `-EIO`；
- 探针不访问 LVGL 或摄像头缓冲，不接入 JPEG 或 PM。

### 2.3 SMP 共存约束

PPA/2D-DMA 在 SMP 固件中受 `CONFIG_ESP32P4_SMP_GRAPHICS_CPU0` 保护：

| 约束 | 说明 |
| --- | --- |
| 只允许 CPU0 调用 | `graphics_probe` 运行时报告 `cpu=0` |
| 缓冲私有 | 缓冲区在任务回收前不得跨核共享 |
| 初始化延后 | 图形初始化推迟到探针运行时，不占启动路径 |
| 不加速 UI | 该选项只允许共存，不改 LVGL 渲染路径 |

## 三、验证配置

本次在 `smp_smart_home` 上验证，这是**最严格的组合**——PPA 与 SMP 共存，
同时 CPU1 在跑 idle/中断，且显示、音频、WiFi、LittleFS 全部启用。

```text
CONFIG_ESP32P4_DMA2D=y
CONFIG_ESP32P4_PPA=y
CONFIG_ESP32P4_SMP=y
CONFIG_ESP32P4_SMP_PSRAM=y
CONFIG_ESP32P4_SMP_SPIFLASH=y
CONFIG_ESP32P4_SMP_GRAPHICS_CPU0=y
CONFIG_LVX_USE_DEMO_CONTEST2026_031_GRAPHICS_PROBE=y
```

独立单核配置 `configs/dma2d` / `configs/ppa` 也供隔离排障使用，但本次未单独构建。

## 四、板端验证结果

### 4.1 五项功能逐项

```text
nsh> graphics_probe copy
graphics case: copy src=auto dst=auto fg=unused
graphics: cpu=0 rounds=1 bytes=49152 src=0x48266f80 fg=0 dst=0x4825af40
graphics memory: src=psram fg=unused dst=psram
graphics copy: PASS (0)

nsh> graphics_probe fill
graphics case: fill src=unused dst=auto fg=unused
graphics memory: src=unused fg=unused dst=psram
graphics fill: PASS (0)

nsh> graphics_probe srm
graphics memory: src=psram fg=unused dst=psram
graphics srm: PASS (0)

nsh> graphics_probe blend
graphics case: blend src=auto dst=auto fg=auto
graphics: cpu=0 rounds=1 bytes=49152 src=0x48266f80 fg=0x48272fc0 dst=0x4825af40
graphics memory: src=psram fg=psram dst=psram
graphics blend: PASS (0)

nsh> graphics_probe convert
graphics memory: src=psram fg=unused dst=psram
graphics convert: PASS (0)
```

注意所有缓冲区都落在 **PSRAM**（`src=psram dst=psram`），即 DMA2D 访问外部 PSRAM，
需要 cache 一致性处理，比内部 SRAM 的条件更严格。

### 4.2 每项的校验内容

探针不是"跑通即 PASS"，每项都做逐像素校验：

| 命令 | 校验点 |
| --- | --- |
| `copy` | 带偏移、不同 stride、跨 128 行描述符边界的逐像素比对；另检查目标外区域未被写、busy 状态、错误任务号、非法参数、取消后重新提交 |
| `fill` | 红色填充值正确 + 目标外区域未被污染 |
| `srm` | 固定角度旋转与镜像逐个方向校验；纯色图验证 0.5/1/2 倍缩放及区域边界 |
| `blend` | 不透明背景上前景 alpha 0/255/128 的结果；中间值允许每个 RGB565 分量一个量化码误差 |
| `convert` | RGB888/ARGB8888 红色转 RGB565；中性黑 UYVY 转 RGB565 |

### 4.3 压力测试

```text
nsh> graphics_probe all 100
graphics round 1/100
graphics copy: PASS (0)
graphics fill: PASS (0)
graphics srm: PASS (0)
graphics blend: PASS (0)
graphics convert: PASS (0)
...
graphics round 100/100
graphics copy: PASS (0)
graphics fill: PASS (0)
graphics srm: PASS (0)
graphics blend: PASS (0)
graphics convert: PASS (0)
```

统计：100 轮 × 5 项 = 500 项，累计 PASS 911 条记录、**FAIL 0 条**。

### 4.4 资源泄漏检查

追加 50 轮前后对比 `free`：

```text
轮前：Umem used=2465220  Kmem used=13572
轮后：Umem used=2465220  Kmem used=13572
```

**完全无变化**。说明 DMA 描述符与中间缓冲都在任务回收时正确释放，无 IOB 泄漏。

整个过程未出现超时、异常复位或 Bridge underrun。

## 五、一个必须记录的测量陷阱

ESP32-P4 的 USB Serial/JTAG 会在**每次打开串口时**由 DTR/RTS 触发复位。

最初用"一条命令开一次端口"的方式跑 `graphics_probe all 100`，结果每轮只跑到
第 32 轮就被复位截断，而命令退出码仍为 0——如果不看 `grep -c "graphics round"`
统计，会误以为 100 轮全部通过。

改用常驻端口 + Unix socket 转发后才拿到完整结果。**后续所有长测试都必须使用
常驻控制台，不能以"命令返回"当作"测试完成"。**

## 六、未覆盖项

以下内容本次没有验证，不据此判定通过：

- `blend` 的**保留像素 alpha** 模式（README 列为"仍需补充板端专项验收"）；
- PPA 在 CPU1 上运行（当前设计限定 CPU0，不是缺陷而是约束）；
- PPA 与摄像头预览并发的实际帧率收益（`SMART_HOME_CAMERA_PPA` 未在此次开启）；
- 内部 SRAM 与 PSRAM 的完整内存矩阵（本次只跑通了 PSRAM 路径）；
- `configs/dma2d` 与 `configs/ppa` 两个独立单核配置的构建与板测。

## 七、复现步骤

```sh
cd /home/arongw/openvela
./contest2026_031_niudanxianqianchong/scripts/build_p4x.sh smp_smart_home -j2
myenv/bin/esptool --chip esp32p4 --port /dev/ttyACM0 --baud 921600 \
  write-flash -fs 16MB -fm dio -ff 80m 0x2000 nuttx/nuttx.bin
```

板端：

```text
graphics_probe copy
graphics_probe fill
graphics_probe srm
graphics_probe blend
graphics_probe convert
graphics_probe all 100
```

判据：每项输出 `graphics <操作>: PASS (0)`，`all 100` 无 FAIL，前后 `free` 一致。

## 八、结论

PPA 与 2D-DMA 在 ESP32-P4 revision v3.2 上完成五项功能验证与 150 轮压力测试，
全部通过，无内存泄漏，且在 SMP + 显示 + 音频 + WiFi + LittleFS 同时启用的重负载
配置下保持稳定。

保留像素 alpha 混合与摄像头并发收益两项待补。
