# ESP32-P4X 2D-DMA / PPA

初始实现基线：`ddc4ff7c8799c531c1c640b889fed56fa89322bf`。
2026-11-02 整理至主线基线 `6f13fcd`，作为默认关闭、待用户构建和板测的实验性接入。
驱动使用固定 HAL
`b90b1837cb5ad24747deb4c895246037cc206ce5` 的寄存器、描述符和 LL 接口，
不引入 FreeRTOS 上层驱动。

## 能力

| 配置 | 操作 | 支持范围 |
| --- | --- | --- |
| `dma2d` | RGB565 二维复制 | 不同 stride、源/目标 ROI、最多 16 个链式描述符，每段 128 行 |
| `ppa` | 填充 | RGB565、BGR888、ARGB8888；颜色参数统一为 `0xAARRGGBB` |
| `ppa` | 缩放、旋转、镜像 | Q4 缩放系数，16 表示 1 倍；逆时针 0/90/180/270 度，X/Y 镜像 |
| `ppa` | 双图层混合 | 三种 RGB 格式；固定 alpha 或保留像素 alpha，使用 straight alpha |
| `ppa` | 格式/颜色空间转换 | 三种 RGB 格式互转；UYVY YUV422 输入转 RGB，BT.601 limited range |

PPA 与二维复制共用一个 DMA2D 管理层，当前一次只允许一个未回收任务。
DMA2D 完成中断通知写回结果，PPA 参数错误中断返回 `-EIO`。
暂不接入 LVGL、摄像头业务、JPEG、SMP 或 PM。

## 构建

以下命令在本变更合入后的主检出执行，工作目录为 openvela 根目录。
首次注册探针目录；已有同名链接时先核对目标，不重复创建：

```sh
ln -s ../../contest2026_031_niudanxianqianchong/app/graphics_probe packages/demos/contest2026_031_graphics_probe
source myenv/bin/activate
./build.sh contest2026_031_niudanxianqianchong/board/esp32p4/esp32p4-function-ev-board/configs/dma2d -j4
```

先构建并板测 `dma2d`，再将命令中的 `configs/dma2d` 改为 `configs/ppa`，重新构建和板测。
两者共用 `nuttx/nuttx.bin`，烧录前核对本次 `.config` 与镜像，避免混用。
两者使用 USB Serial/JTAG 控制台，不启用屏幕、摄像头、音频、PSRAM 或 SMP。
Make/CMake 均接入源码列表；完整 CMake 构建尚未验证。
烧录与日志要求见[构建验证指南](../../docs/开发指南/ESP32-P4X-PPA与2D-DMA构建验证指南.md)。

## 板端验收

```text
nsh> graphics_probe copy
nsh> graphics_probe fill
nsh> graphics_probe srm
nsh> graphics_probe blend
nsh> graphics_probe convert
```

`dma2d` 配置只提供 `copy`；`ppa` 配置提供全部命令，也可运行 `graphics_probe all`。

- `copy`：带偏移、不同 stride、跨 128 行描述符边界的逐像素校验；检查目标外区域、
  busy、错误任务号、非法参数、取消和再次提交。
- `fill`：红色填充及目标外区域校验。
- `srm`：分别校验固定角度旋转和镜像；纯色图验证 0.5/1/2 倍缩放及区域边界。
- `blend`：不透明背景上前景 alpha 为 0、255、128 的结果；中间值允许每个 RGB565
  颜色分量一个量化码的误差。保留像素 alpha 的模式仍需补充板端专项验收。
- `convert`：RGB888/ARGB8888 红色转 RGB565，以及中性黑 UYVY 转 RGB565。

每项应输出 `graphics <操作>: PASS (0)`。先单项通过，再重复全套 100 次，记录复位、
超时、堆余量和数据校验结果；未完成板端验证前不宣称吞吐提升。

## 接口与约束

接口位于 `arch/chip/esp_graphics.h`：初始化后 `submit()` 返回任务号，随后必须
`wait(ticket, timeout_ms)` 或 `cancel(ticket)`。超时上限 60000 ms；超时、取消和错误
均先停止硬件，再恢复 CPU 对目标缓冲的访问。AXI 无法在 100 ms 内安全停止时触发
PANIC，防止向调用者归还仍可能被 DMA 写入的内存。

- 当前仅支持 P4 revision 3.x、FLAT、单核、无 PM。
- 缓冲首地址和完整分配长度须按 64 字节对齐，stride 单位为字节，必须能整除像素大小。
- stride、ROI 起始字节偏移和行内数据长度须按 4 字节对齐；图像维度受 14 位字段限制。
  SRM 输出至少 2×2；不满足条件返回错误，由上层选择软件回退。
- 输入、输出均须在 DMA 可访问的 RAM。目标不得与任一输入分配区重叠，不支持原地操作。
- Cache 同步覆盖整个分配区。提交后，直到等待或取消返回，调用者不得退出、释放缓冲，
  CPU 和其他 DMA 也不得访问这些分配区。
- 本驱动独占 DMA2D 时钟、复位和通道；不能与另一个 DMA2D 分配器同时使用。
  PSRAM、显示/摄像头并发、所有格式组合与像素 alpha 均需另行实板验收。

## Host 检查

```sh
python3 tests/esp32p4/test_graphics_bounds.py
```

该检查直接编译生产用几何校验代码，覆盖 stride、像素对齐、边界、地址溢出及描述符
字段上限；不模拟 DMA，也不能替代板端像素和中断验收。

## 历史验证状态（2026-10-28，初始实现）

| 检查 | 结果 |
| --- | --- |
| Host 几何边界检查，5 类测试 | 通过 |
| 6 个新增 C/头文件 nxstyle、文本空白与本地链接 | 通过 |
| `dma2d`、`ppa` Make 编译、链接、镜像生成 | 通过 |
| 原 `nsh` 配置干净输出重建 | 通过 |
| CMake 完整构建、板端像素/中断/稳定性、PSRAM 并发 | 未验收 |

2026-11-02 主线集成仅重跑 Host 几何检查和静态接入检查，固件构建及板端测试由用户执行。
上述历史构建结果不能替代本次集成后的完整构建或板端驱动验收。
