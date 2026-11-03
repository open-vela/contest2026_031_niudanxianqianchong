# ESP32-P4X SMP SmartHome UI 启动验证记录（G3.5）

日期：2026-10-29
状态：真实 SmartHome UI 已在双核系统上运行，UI 固定在 CPU0。
关联文档：
[ESP32-P4X-SMP-DSI与GT911双核验证](ESP32-P4X-SMP-DSI与GT911双核验证_20261029.md)

## 1. 目标

在 `smp_ui`（SMP + PSRAM + LittleFS + DSI + GT911）基础上接入真实
`SMART_HOME_DEMO` 应用，先不启用音频、摄像头和 ESP-Hosted 网络，验证：

- SmartHome UI 能否在 SMP 内核上完整构建并启动；
- UI 任务是否固定在 CPU0；
- UI 运行时双核文件系统并发是否仍然稳定。

## 2. 配置

新增 `configs/smp_smart_home`，在 `smp_ui` 基础上追加：

```text
CONFIG_SMART_HOME_DEMO=y
CONFIG_SMART_HOME_DEMO_UI_LVGL=y
CONFIG_SMART_HOME_DEMO_STACKSIZE=65536
CONFIG_SMART_HOME_MODEL_REQUEST_BUFFER_SIZE=65536
CONFIG_SMART_HOME_SESSION_PERSIST=y
CONFIG_SMART_HOME_MILOCO_BRIDGE=y
CONFIG_SMART_HOME_MILOCO_VOLATILE_CONFIG=y
CONFIG_AGENT_CORE_WORKSPACE_BYTES=98304
CONFIG_AGENT_SCRATCH_BYTES=65536
CONFIG_NET=y
CONFIG_NET_TCP=y
CONFIG_NET_UDP=y
CONFIG_NETDEV_LATEINIT=y
CONFIG_NETUTILS_CJSON=y
CONFIG_NETUTILS_NETLIB=y
CONFIG_NETUTILS_DHCPC=y
CONFIG_LV_USE_FS_POSIX=y
CONFIG_LV_FS_POSIX_LETTER=65
CONFIG_LV_FS_POSIX_CACHE_SIZE=4096
CONFIG_LV_USE_LODEPNG=y
CONFIG_LV_USE_TINY_TTF=y
```

构建依赖说明：

- `NETDEV_LATEINIT=y` 让 `riscv_netinitialize()` 变成空宏，避免没有网卡时链接失败。
- `NETUTILS_DHCPC=y` 才会编译 `netlib_obtain_ipv4addr()`，SmartHome 的网络初始化
  依赖该符号。
- `MILOCO_BRIDGE` 必须开启，UI 代码引用了 miloco 结构与页面；未启用时编译失败。

## 3. 启动结果

```text
[smart_home_lvgl] font instances source=PSRAM-data size=7943504
[smart_home_lvgl] disp=0x482f9aa0 indev=0x482fad10 input=/dev/input0
[smart_home_lvgl] build screensaver done
[smart_home_lvgl] build home done
[smart_home_lvgl] build panel done
[smart_home_lvgl] build security done
[smart_home_lvgl] build more done
[smart_home_lvgl] build network done
[smart_home_lvgl] build miloco done
[smart_home_lvgl] build miloco-bind done
[smart_home_lvgl] build chat done
[smart_home_lvgl] build settings done
[lvgl] ui init done
[lvgl] show done, entering run loop
[smart_home_cpu] role=lvgl-loop-start cpu=0 mask=0x1
```

- 字体从 PSRAM 加载（约 7.9 MB）。
- 所有页面构建完成，LVGL 进入主循环。
- 应用自身打印 `cpu=0 mask=0x1`，与 `taskset 1 smart_home` 一致。

`ps` 中应用与工作线程的亲和性都是 `0x00000001`：

```text
 9  9 ---(0x00000001) 100 RR Task    - Waiting 0065464 0037124 smart_home
10  9 ---(0x00000001) 100 RR pthread - Waiting 0024544 0002164 smart_home
```

## 4. 双核并发

UI 运行期间执行 G2 文件系统压测：

```text
nsh> taskset 3 smp_flash_test 8 16
smp_flash: cpu1 round 8/8 errors=0
smp_flash: cpu0 round 8/8 errors=0
smp_flash: done rounds=8 chunk=16384 errors=0 PASS
```

UI 进程保持存活，PSRAM 字体、显示、LittleFS 均正常。

## 5. 复位回归

`taskset 2 reboot` 后重新出现 `CPU1 ready`、DSI framebuffer、GT911 注册、
LittleFS 挂载并进入 NSH（`ok=True`）。

## 6. 当前边界

- **没有网络**：`[milo] weather fetch failed ret=-113` 属预期，ESP-Hosted 尚未接入，
  cAGENT 对话请求无法完成，但 UI 与本地页面正常。
- **没有音频与摄像头**：ES8311、SC2336 未启用，语音链路和摄像头预览不可用。
- **物理触摸与显示效果未验证**：GT911 已注册，LVGL 已绑定 `/dev/input0`，仍需
  实际按压与目视确认。
- PM 仍被 SMP Kconfig 禁止。

## 7. 复现

```sh
source myenv/bin/activate
./build.sh contest2026_031_niudanxianqianchong/board/esp32p4/esp32p4-function-ev-board/configs/smp_smart_home -j4
esptool --chip esp32p4 --port /dev/ttyACM0 --baud 921600 --after hard-reset \
  write-flash -fs 16MB -fm dio -ff 80m 0x2000 nuttx/nuttx.bin
```

```text
nsh> taskset 1 smart_home &
nsh> ps
nsh> taskset 3 smp_flash_test 8 16
nsh> taskset 2 reboot
```

## 8. 下一步

`smart_demo` 的界面部分已经跑通。要得到可演示的完整体验，还需要：

1. G4：ES8311 + I2S 音频，验证录音/播放与 UI 并发。
2. G6：ESP-Hosted WiFi 与 cAGENT 对话链路。
3. G5：SC2336 摄像头预览（可放到最后）。
