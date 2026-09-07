# ESP32-P4X CSI Probe

`csi_probe` 用于将 P4X 摄像头适配拆成可独立验收的阶段。目前提供五种
有界诊断：

```text
nsh> csi_probe sensor
nsh> csi_probe sccb
nsh> csi_probe pointer
nsh> csi_probe split
nsh> csi_probe write
nsh> csi_probe raw
nsh> csi_probe raw 10
```

`sensor` 先获取与 ESP-IDF 基线相同的 MIPI D-PHY LDO（channel 3、2500mV），
然后只读取 SC2336 SCCB 产品 ID；预期 I²C 地址为 `0x30`，产品 ID 寄存器为
`0x3107/0x3108`，正确结果为 `0xcb3a`。它不初始化 CSI Host、ISP 或 DMA。

`sccb` 运行读 ID → 写 `0x0100=0x00`（停流）→ 再读 ID 的读后写诊断。
`pointer` 仅向总线发送一次 `0x30 + W, 0x31, 0x07, STOP`，用于验证产品 ID
寄存器地址前导；它不读取数据，也不写入任何传感器控制寄存器。
`split` 则将该地址前导和一个字节读取拆成两次独立 I²C 事务，期望读回
`0xCB`；用于区分多消息读路径和读方向本身的问题。
`write` 则执行 ESP-IDF SC2336 初始化序列的安全前缀：写 `0x0103=0x01`
（软件复位）、等待 5 ms、写 `0x0100=0x00`（停流）、再读产品 ID。二者均不
初始化 CSI Host、ISP 或 DMA；`write` 用于将写启动问题与此前 ID 读事务分离。

`raw [1-10]` 使用乐鑫 `esp_cam_sensor` v1.7.0 的 Apache-2.0 profile：
24 MHz 输入、2 lane、RAW8/CSI-2 datatype `0x2a`、1024×600@30fps、
288 Mbps/lane、BGGR。它按 ESP-IDF 相同的摄像头相关时序执行：先申请 Function
EV Board 的 MIPI D-PHY LDO（channel 3、2500mV），再探测和配置传感器、开启
sensor stream，随后初始化并 arm CSI/DMA 接收端。完成指定帧数后，它按相反顺序
释放 CSI Host、sensor stream、SCCB 和 LDO，并输出帧
长度、CRC32、非全零/非固定值判定和 DMA、Bridge、CSI ECC/CRC/PHY/packet
计数。

任何失败都会返回非零状态；命令不会注册 `/dev/video0` 或启用 ISP，也不接入
默认 board bring-up。RAW 文件保存属于下一小步，避免在 LittleFS 挂载路径尚未
验收前混入本次 CSI 链路验证。

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

P2 首轮应先运行 `csi_probe sensor`，确认日志出现 `MIPI-CSI D-PHY LDO ready`
且能够读取 SC2336 ID；这可直接覆盖 ESP-IDF 基线的 LDO→SCCB 前半段。再运行
`csi_probe raw`，单帧通过后运行 `csi_probe raw 10`。
`raw 10` 的 `frames` 可以大于请求值（DMA 已完成的帧可能积累在信号量中）；
通过条件是至少收到请求帧数、RAW 校验为 `yes` 且所有错误计数为零。
