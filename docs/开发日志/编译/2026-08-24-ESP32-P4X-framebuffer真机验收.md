# ESP32-P4X `/dev/fb0` 真机验收记录

**日期**：2026-08-24
**固件配置**：`board/esp32p4/esp32p4-function-ev-board/configs/fb_probe`
**结论**：PASS

## 1. 验收目的

验证 P4X 板级 MIPI-DSI framebuffer 装配是否将已点亮的 EK79007 DPI/GDMA 扫描通路
正确暴露为标准 NuttX `/dev/fb0`，并确认应用写入经 `FBIO_UPDATE` 后可由显示链路刷新。

该配置与 `dsi_probe` 互斥：两者都独占 P4 DSI Host、EK79007 面板和 DMA scanout，不能
编入同一固件。

## 2. 实测命令

```text
nsh> ls /dev/fb0
 /dev/fb0

nsh> fb
```

`fb` 是 NuttX 标准 `apps/examples/fb` 示例，不是项目私有测试程序。它依次执行：

1. `FBIOGET_VIDEOINFO`；
2. `FBIOGET_PLANEINFO`；
3. 映射 framebuffer；
4. 绘制六个逐层缩小的彩色矩形；
5. 每次绘制调用 `FBIO_UPDATE`，由板级 `updatearea()` 进行 DMA 所需的 cache clean。

## 3. 串口证据

```text
VideoInfo:
      fmt: 11
     xres: 1024
     yres: 600
  nplanes: 1
PlaneInfo (plane 0):
    fbmem: 0x48000200
    fblen: 1228800
   stride: 2048
  display: 0
      bpp: 16
Mapped FB: 0x48000200
 0: (  0,  0) (1024,600)
 1: ( 93, 54) (838,492)
 2: (186,108) (652,384)
 3: (279,162) (466,276)
 4: (372,216) (280,168)
 5: (465,270) ( 94, 60)
FB test finished
```

## 4. 结论与下一步

已确认：

- `/dev/fb0` 注册成功；
- 格式为 RGB565（`fmt=11`）、分辨率 1024×600、单平面、stride 2048；
- 1,228,800 B PSRAM buffer 可被用户态应用映射；
- 标准 framebuffer 绘制流程正常结束，说明 `FBIO_UPDATE` 回调可用。

因此 P1 framebuffer 阶段通过，允许进入 P2：以 `/dev/fb0` 接入无网络、无触摸的
静态 LVGL Smart Home 首页。P2 前不应同时启用 MCP、Node gateway、App Bridge 或云端
模型请求。

后续回归项：连续运行 `fb` 10 次、长时间静态扫描、LVGL 高频局部刷新，以及 GT911
触摸并发输入。
