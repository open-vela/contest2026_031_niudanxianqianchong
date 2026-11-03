# ESP32-P4X PPA 与 2D-DMA 构建验证指南

日期：2026-11-02。状态：独立探针及内存矩阵已收到板测通过反馈；Demo 预览加速待板测。

## 1. 范围

- `dma2d`：RGB565 二维复制、不同 stride 的 ROI、链式描述符和完成中断。
- `ppa`：复制、填充、缩放/旋转/镜像、混合及部分格式转换。
- 首轮仅验证 P4 revision 3.x、FLAT、内部 SRAM、单核、无 PM。
- 两个配置不启用 SmartHome、显示或摄像头；屏幕没有测试画面，结果由串口给出。
- `smart_home` 显式启用 CPU0 图形探针共存，其余 SMP 配置保持关闭；
  摄像头预览可选 PPA 缩放，LVGL 通用页面渲染保持原路径。

## 2. 构建

本变更合入主检出后，在 openvela 根目录执行。首次创建探针链接；已有链接时用
`readlink -f` 核对目标，须指向本次已合入的 `app/graphics_probe`，不要覆盖其他工作区链接。

```sh
cd /home/arongw/openvela
source myenv/bin/activate
ln -s ../../contest2026_031_niudanxianqianchong/app/graphics_probe packages/demos/contest2026_031_graphics_probe
./build.sh contest2026_031_niudanxianqianchong/board/esp32p4/esp32p4-function-ev-board/configs/dma2d -j4
```

先完成 `dma2d` 烧录和测试，再构建 `ppa`：

```sh
./build.sh contest2026_031_niudanxianqianchong/board/esp32p4/esp32p4-function-ev-board/configs/ppa -j4
```

成功后应生成 `nuttx/nuttx.bin`，构建输出包含 `Generated: nuttx.bin`。
两种配置共用该路径，每次烧录都应使用刚构建的镜像。

核对 `nuttx/.config`：

| 项目 | dma2d | ppa |
| --- | --- | --- |
| `CONFIG_ESP32P4_DMA2D` | y | y |
| `CONFIG_ESP32P4_PPA` | 未启用 | y |
| `CONFIG_LVX_USE_DEMO_CONTEST2026_031_GRAPHICS_PROBE` | y | y |
| `CONFIG_SMP`、`CONFIG_PM`、`CONFIG_ESPRESSIF_SPIRAM` | 未启用 | 未启用 |
| `CONFIG_ESPRESSIF_USBSERIAL`、`CONFIG_ESPRESSIF_SIMPLE_BOOT` | y | y |

## 3. 烧录

先确认目标串口与芯片，关闭占用串口的终端。下面的 `/dev/ttyACM0` 须替换为实际设备。
确认本次产物仍为 P4 Simple Boot、16MB / DIO / 80MHz 后执行：

```sh
esptool --chip esp32p4 --port /dev/ttyACM0 chip-id
esptool --chip esp32p4 --port /dev/ttyACM0 --baud 921600 write-flash -fs 16MB -fm dio -ff 80m 0x2000 nuttx/nuttx.bin
picocom -b 115200 /dev/ttyACM0
```

烧录应返回 `Hash of data verified`，重新枚举后应进入 `nsh>`。
只写入固件，不擦除整片 Flash，不烧写 LittleFS 资源镜像。

## 4. 板端测试

`dma2d` 固件执行：

```text
nsh> graphics_probe copy
graphics copy: PASS (0)
```

`ppa` 固件依次执行：

```text
nsh> graphics_probe copy
nsh> graphics_probe fill
nsh> graphics_probe srm
nsh> graphics_probe blend
nsh> graphics_probe convert
```

每项预期输出 `graphics <操作>: PASS (0)`；单项通过后运行 `graphics_probe all`，
连续重复 100 次，并比较测试前后的 `free`、`ps`。要求无校验错误、无超时、无异常复位，
堆余量不持续下降。遇到首个失败即保留日志，先定位再继续下一阶段。

现有探针只覆盖首轮冒烟：缩放使用纯色、旋转使用正方形、YUV 转换使用中性黑。
非正方形与组合变换、彩色 YUV、像素 alpha、受控超时、PSRAM、SMP、性能均待后续验证。

## 5. 回传记录

请分别保留两个配置的完整构建日志、`.config`、镜像 SHA-256、烧录日志，以及从复位到
各项探针结束的串口日志。启动失败时保留 ROM 输出；测试失败时保留命令、错误码和首次
像素不匹配信息。Host 检查通过不作为板端通过证据。

接口、缓冲约束与历史构建记录见[驱动说明](../../app/graphics_probe/README.md)。

## 6. SmartHome 固件共存测试

独立单核测试已由用户反馈五项 PASS；用户另反馈 CPU0、三个 PSRAM 缓冲的
100 轮测试完成，末两轮五项 PASS 并返回 NSH。其余内存组合及业务共存仍需分别验收。
使用同一探针链接，改为构建现有 SmartHome 配置：

```sh
./build.sh contest2026_031_niudanxianqianchong/board/esp32p4/esp32p4-function-ev-board/configs/smart_home -j4
```

本配置的图形开关如下，构建后同时核对 SMP 和 PSRAM 用户堆仍启用：

```text
CONFIG_ESP32P4_SMP_GRAPHICS_CPU0=y
CONFIG_ESP32P4_DMA2D=y
CONFIG_ESP32P4_PPA=y
CONFIG_LVX_USE_DEMO_CONTEST2026_031_GRAPHICS_PROBE=y
```

按第 3 节核对镜像和烧录参数后烧录固件，保留原 LittleFS 资源。进入 NSH 后：

```text
nsh> taskset 1 smart_home &
nsh> graphics_probe all
nsh> free
nsh> graphics_probe all 100
nsh> free
nsh> ps
```

若 SmartHome 已启动，跳过启动命令。探针将自身固定在 CPU0，首行应打印 `cpu=0`、
缓冲地址和轮次；PSRAM 地址窗口为 `0x48000000` 至 `0x4c000000`。
单轮预热后再统计堆余量，排除驱动首次初始化常驻内存的影响。

验收要求：每轮五项 PASS，UI 持续响应且无花屏、无复位、堆余量不持续下降；
分别记录基础网络、摄像头预览和音频启用时的结果，不将未启动的外设计为共存通过。
探针只访问自己的缓冲。摄像头业务接入见第 8 节；LVGL 页面渲染保持原路径。
跨核图形缓冲、Flash 擦写并发和性能收益仍需后续专项验证。

## 7. SRAM / PSRAM 矩阵验证

重新构建、烧录第 6 节的 `smart_home` 固件，按以下顺序执行：

```text
nsh> graphics_probe matrix
nsh> free
nsh> graphics_probe matrix 100
nsh> free
nsh> ps
```

首轮用于验证全部组合并完成驱动预热。PPA 启用时共 22 组：复制、SRM、转换各 4 组，
填充 2 组，混合 8 组。100 表示每组执行 100 轮，每组结束即释放缓冲。
最终应输出：

```text
graphics matrix: PASS (22 cases, 100 rounds/case)
```

核对每组 `graphics case` 的请求内存、`graphics memory` 的实际内存及缓冲地址。
显式 SRAM / PSRAM 必须匹配，分配不足、地址不符或像素失败均立即停止，不降级跳过。
SRAM 用内核堆，PSRAM 用用户堆；缓冲保持 64 字节对齐，描述符仍在内部 SRAM。

定位单组问题时执行：

```text
nsh> graphics_probe copy 1 sram psram
nsh> graphics_probe srm 1 psram sram
nsh> graphics_probe fill 1 auto sram
nsh> graphics_probe blend 1 sram psram psram
```

参数依次为操作、轮次、源内存、目标内存、可选前景内存；前景默认与源相同。
`blend` 的源表示背景，示例是 SRAM 背景、PSRAM 目标、PSRAM 前景。
`fill` 不使用源，其他非混合操作不使用前景；`auto` 保留原分配行为。

先关闭摄像头和音频等额外负载完成矩阵，再逐项加入业务。每次记录启用的外设、完整日志、
预热后运行前后的 `free` 和 `ps`。要求无校验错误、超时、复位，空闲堆不持续下降，
业务启用时 UI 和触摸保持响应。受控超时、跨核共享和性能测试另行开展。

## 8. SmartHome 摄像头预览使用 PPA

用户已反馈 `graphics matrix: PASS (22 cases, 100 rounds/case)`。
本阶段将 PPA 接入真实摄像头预览：SC2336 RGB565 1024×600 → PPA SRM →
512×300 双缓冲 → 原有 LVGL 取图接口。采集仍为 30 FPS，每两帧生成一次预览。

`smart_home` 新增启用：

```text
CONFIG_SMART_HOME_CAMERA_PPA=y
```

该选项全局默认关闭，依赖摄像头预览和 PPA 驱动。沿用第 6 节构建命令及第 3 节固件
烧录步骤，保留 LittleFS 资源。构建后核对 `nuttx/.config` 中新选项为 y。

启动 SmartHome，在设备页打开本地摄像头预览；已运行时不重复启动：

```text
nsh> taskset 1 smart_home &
```

串口应出现 `backend=ppa` 和缓冲地址；首个预览、每 150 个预览及采集退出时打印统计。
硬件路径正常时 `ppa` 帧数持续增加，`cpu=0 errors=0`。以下仅为字段示例：

```text
[smart_home_camera] scale ppa=150 cpu=0 errors=0 last=0 avg_ppa_us=... avg_cpu_us=0 max_us=...
```

- `ppa` / `cpu`：本次摄像头启动以来，由硬件 / CPU 完成的预览帧数。
- `errors` / `last`：PPA 初始化或任务失败次数、最近错误码；`-EBUSY` 也计入。
- `avg_ppa_us`：成功硬件帧从提交至等待完成的平均墙钟耗时；`avg_cpu_us` 为 CPU 缩放耗时。
- `max_us`：预览生成最大耗时，包含失败等待和回退；不代表整条显示链路延迟。

首次验收时不同时运行图形探针，避免争用单任务图形通道。观察预览至少 5 分钟，要求
画面比例、颜色正确，无撕裂、花屏、复位，触摸保持响应，PPA 成功计数持续增长。
再开关预览 10 次，比较预热后关闭状态下的 `free`，确认缓冲释放、能重复启动。
最后逐项加入网络和音频负载，记录统计、实际 FPS、`ps` 与堆余量。
预览 FPS 按完成帧测量，启动首秒可能为 0；15 FPS 是目标，不是硬件加速收益结论。

PPA 初始化失败、提交忙或任务失败时，服务使用 CPU 缩放。已提交的任务必须确认释放
DMA 所有权后才能回退、发布或 QBUF；异常释放错误会保留帧并重试取消，不提前释放内存。
停止时先等待采集线程退出，再释放预览和采集缓冲。图形驱动自身无法停止 AXI 时仍会 PANIC。

本阶段未接入 LVGL 通用硬件绘图或额外的 DMA2D 预览复制。CPU 取图和屏幕刷新仍保持原路径。
需比较收益时，关闭 `SMART_HOME_CAMERA_PPA` 对照构建，在相同场景比较缩放耗时和 `ps` CPU
负载；不能仅依据 PPA 计数宣称帧率提升。

Host 检查：`python3 tests/esp32p4/test_camera_ppa.py`。测试使用假的 V4L2/PPA，验证真实服务
控制流程、回退及分配清理，不验证摄像头、DMA、Cache 或实机性能。
