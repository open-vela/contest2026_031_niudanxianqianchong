# ESP32-P4X CSI Probe

`csi_probe` 用于将 P4X 摄像头适配拆成可独立验收的阶段。第一批只实现
SC2336 SCCB 产品 ID 探测：

```text
nsh> csi_probe sensor
```

当前命令不会初始化 MIPI-CSI、ISP 或 DMA，也不会注册 `/dev/video0`。
预期 SC2336 I²C 地址为 `0x30`，产品 ID 寄存器为 `0x3107/0x3108`，正确
结果为 `0xcb3a`。这些定义对齐乐鑫 `esp-video-components`
中的 SC2336 驱动。失败会返回非零状态，不影响系统默认
bring-up。

团队 manifest 已声明应用链接，正常 `repo sync` 后会生成
`packages/demos/contest2026_031_csi_probe`。若是在已存在的本地工作区
直接添加源码，可手工建立一次链接：

```bash
ln -s ../../contest2026_031_niudanxianqianchong/app/csi_probe \
  apps/packages/demos/contest2026_031_csi_probe
```

然后使用独立配置构建：

```bash
./build.sh \
  contest2026_031_niudanxianqianchong/board/esp32p4/esp32p4-function-ev-board/configs/csi_probe \
  -j2
```
