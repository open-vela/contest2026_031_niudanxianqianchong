# ESP32-P4X PPA 与 2D-DMA 构建验证指南

日期：2026-11-02。状态：实验性接入，待用户构建、烧录及板测。

## 1. 范围

- `dma2d`：RGB565 二维复制、不同 stride 的 ROI、链式描述符和完成中断。
- `ppa`：复制、填充、缩放/旋转/镜像、混合及部分格式转换。
- 首轮仅验证 P4 revision 3.x、FLAT、内部 SRAM、单核、无 PM。
- 两个配置不启用 SmartHome、显示或摄像头；屏幕没有测试画面，结果由串口给出。
- 原 SmartHome/SMP 配置不启用图形加速。本次接入不代表 SMP 或 LVGL 已使用 PPA。

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
