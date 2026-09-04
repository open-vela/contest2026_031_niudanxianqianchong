# ESP32-P4X SmartHome Demo 测试开发计划

## 1. 目标与范围

本计划将现有 `demos/smart_home` 从 Goldfish / ESP32-S3 的运行环境迁移到
**ESP32-P4X-Function-EV-Board**，并按可隔离、可回归的阶段完成真机测试。

首个交付目标不是一次性启用云端模型、MCP、Node 和手机 App，而是让 P4X LCD 上稳定
显示 Smart Home 的本地设备面板；随后再逐层接入触摸、以太网和 cAGENT 能力。

本计划不改变已验证的 P4X DSI 链路参数：EK79007、1024×600、2 lane、RGB565、
52 MHz nominal pixel clock，以及 GPIO27 低有效复位、GPIO26 背光控制。

## 2. 当前基线与结论

| 项目 | 状态 | 证据 / 说明 |
| --- | --- | --- |
| 最小 P4X 固件 | 已通过 | USB Console 可进入 `nsh>`。 |
| EK79007 DBI 命令通路 | 已通过 | 已对齐 ESP-IDF 的 Command ACK 与 LP 传输配置。 |
| Host 内建色条 | 已通过 | `dsi_probe pattern 10` 真机可见。 |
| PSRAM + GDMA RGB565 扫描 | 已通过 | `dsi_probe video 10` 真机可见。 |
| NuttX framebuffer 设备 | 单缓冲真机 PASS；双缓冲待验收 | `/dev/fb0` 已完成 RGB565 绘制和刷新；已实现两页 PSRAM、`FBIOPAN_DISPLAY` 与 DMA 帧边界换页，待实板确认无撕裂。 |
| LVGL Smart Home 页面 | P2 首屏真机 PASS；P3.2 待测 | 静态首页已通过 `/dev/fb0` 显示；正式 Agent + LVGL 离线启动配置已就绪，待真机验证页面与输入设备创建。 |
| GT911 触摸 | P3.1 单指真机通过；P3.2 待测 | `/dev/input0` 与 `gt911_probe` 已验证 `DOWN/MOVE/UP`、坐标和 size；待交给正式 LVGL 的 `indev`。 |
| P4X 以太网、DNS、TLS、云端模型 | 未验证 | 必须与显示问题分阶段验证。 |
| MCP / Node / App Bridge | 未上 P4X | 在本地 UI、网络和模型链路稳定后再启用。 |

当前 DSI 已从“Host PASS、视觉待确认”提升为真机视觉 PASS。详细根因与复测证据见
[P4X DSI 黑屏 DBI 配置排障闭环](../开发日志/编译/2026-08-24-ESP32-P4X-DSI黑屏DBI配置排障闭环.md)。

## 3. 总体策略

```text
已点亮的 dsi_probe
       |
       v
P0：固定显示基线回归
       |
       v
P1：DSI DPI Panel -> /dev/fb0
       |
       v
P2：LVGL 静态 Smart Home 首页（无网络、无触摸）
       |
       v
P3.1：GT911 原始触摸验证
       |
       v
P3.2：正式 LVGL 离线 UI + GT911
       |
       v
P4：以太网、DNS、TLS 与控制台 cAGENT
       |
       v
P5：LVGL 异步 AI 对话
       |
       v
P6：MCP、Node、App Bridge（分别启用）
```

每一阶段只新增一个不确定性；上一阶段未通过时，不进入下一阶段。

## 4. 配置与内存原则

1. 新增独立配置目录，而不修改 `dsi_probe/defconfig` 或
   `fb_probe/defconfig`：

   ```text
   board/esp32p4/esp32p4-function-ev-board/configs/smart_home/
     defconfig
   ```

   它以 P1 `/dev/fb0` 配置为显示基线。P2 选择
   `SMART_HOME_DEMO_STATIC_LVGL_HOME`；当前 P3.2 改用常规 Agent + LVGL
   分支，并选择 `SMART_HOME_DEMO_OFFLINE_UI`。

2. P3.2 使用双 RGB565 framebuffer：

   ```text
   1024 × 600 × 2 B × 2 = 2,457,600 B
   ```

   两页连续放入 PSRAM。LVGL 的 NuttX framebuffer 后端通过
   `yres_virtual=1200` 自动识别双缓冲，并在每次末次 flush 后提交
   `FBIOPAN_DISPLAY`；板级仅在 DW-GDMA 完成当前整帧后切换下一轮扫描源地址。
   因此 CPU 只写后台页，GDMA 只读前台页，不引入全屏软件复制。

3. P3.2 使用 LittleFS 的外置视觉资源：`/data/res/fonts/MiSans-Normal.ttf` 与
   `/data/res/icons/*.png`。`make_p4x_littlefs_data_image.sh` 默认打包 MiSans Normal
   子集和 PNG 图标；LVGL 启用 TinyTTF、POSIX 文件系统和 LodePNG。TinyTTF 直接从
   LittleFS 流式读取 TTF，不依赖 OpenVela 根目录 `external/freetype`。内置 Montserrat
   与编译图标字形仍保留，作为资源缺失或加载失败时的兜底，首屏与触摸验收不依赖云端。
   其中 `src/ui/lvgl/icons/*.c` 的 `ac_20`、`fan_20`、`light_20` 等是嵌入式
   LVGL 字体图标，会直接链接进固件；它们不打包到 LittleFS，供导航和设备语义图标使用。

4. P2 静态模式不只是“关闭” `SMART_HOME_MCP_BRIDGE`、
   `SMART_HOME_NODE_GATEWAY`、`SMART_HOME_APP_BRIDGE`：它不初始化 cAGENT、
   网络、模型密钥或完整智能体运行源文件。P3.1 可在同一固件注册触摸设备，但 LVGL
   在原始事件验收前仍不打开 `/dev/input0`；这样首屏问题仍可与输入问题隔离。P3.2
   切回正式 UI 后，离线 profile 使用网络状态桩，不链接 Wi-Fi、DHCP、DNS 或 TLS；
   缺少 `/data/res/skills` 仅记录技能不可用，不阻止 UI 启动。

5. 云端链路启用后，模型密钥仅来自 `/data/smart_home/secrets.json`；示例文件或
   固件镜像不得携带真实密钥。

## 5. 分阶段开发与测试

### P0：冻结显示基线

**目的**：保证后续问题不是 DSI 回归。

**输入**：当前 `configs/dsi_probe/defconfig` 与已验证的 LCD adapter 接线。

**操作**：

```text
nsh> dsi_probe pattern 10
nsh> dsi_probe video 10
```

**通过条件**：两种模式都显示色条；日志包含
`cmd_mode_cfg=010f7f02`，没有 DMA error、Bridge underrun 或 PLL lock loss。

**失败处置**：停止 Smart Home 集成，先按 DSI 排障闭环文档恢复显示基线。

### P1：注册 P4X `/dev/fb0`（真机 PASS）

**目的**：将已验证的 DPI Panel 扫描缓冲区以标准 NuttX framebuffer 接口暴露，使
LVGL 无需了解 DSI、Bridge 或 GDMA。

**拟修改文件**：

| 文件 | 改动 |
| --- | --- |
| `board/.../src/esp32p4_fb.c`（新增） | 实现单平面 `fb_vtable_s`：`getvideoinfo`、`getplaneinfo`、`updatearea`；将 `FBIO_UPDATE` 映射为 PSRAM 全帧 cache clean。 |
| `board/.../src/esp32p4_lcd.c` | 不改动已验收的 Host、reset、DPI timing 和持续 scanout 实现；新 framebuffer 复用其公开板级接口，不创建第二个 GDMA 管线。 |
| `board/.../include/board.h` | 导出 `board_mipi_dsi_fb_initialize()` 板级装配接口。 |
| `board/.../src/Make.defs`、`CMakeLists.txt` | 条件编译 framebuffer 装配文件。 |
| `board/.../Kconfig` | 新增 `CONFIG_ESP32P4_FUNCTION_EV_BOARD_DSI_FRAMEBUFFER`，选择 DSI video/DMA/DPI 和 NuttX framebuffer；与 `dsi_probe` 互斥。 |
| `board/.../configs/fb_probe/defconfig`（新增） | 独立 P1 固件：启用 `BOARD_LATE_INITIALIZE` 与标准 `CONFIG_EXAMPLES_FB`，不编入 `dsi_probe`。 |

**实现约束**：

- `/dev/fb0` 的像素格式、stride、显存地址由 DPI Panel 实际对象提供，不能由
  Smart Home 硬编码；
- `FBIO_UPDATE` / `updatearea()` 必须完成最小必要的 cache clean，以保证 LVGL 写入
  的脏区域对 GDMA 可见；
- 只注册一个 display、一个 RGB565 plane、一个持续扫描缓冲区；
- `dsi_probe` 与 `/dev/fb0` 不能同时占用同一 Host，测试固件中只选择其一。

**最小验证程序**：直接复用 NuttX 标准 `apps/examples/fb`；它会查询
`FBIOGET_VIDEOINFO` / `FBIOGET_PLANEINFO`、绘制彩色矩形，并在每一步调用
`FBIO_UPDATE`。

**通过条件**：

```text
nsh> ls /dev/fb0
nsh> fb
```

**本次真机结果**：`/dev/fb0` 可枚举；标准 `fb` 成功返回 `fmt=11`（RGB565）、
`1024x600`、`stride=2048`、`fblen=1228800`，映射地址为 `0x48000200`，并完成六次
矩形绘制后输出 `FB test finished`。这证明 framebuffer 注册、应用映射和
`FBIO_UPDATE` cache clean 已打通；详细串口证据见
[framebuffer 真机验收记录](../开发日志/编译/2026-08-24-ESP32-P4X-framebuffer真机验收.md)。

**双缓冲增强（代码完成，真机待验收）**：P4X framebuffer 现分配两页连续 RGB565
PSRAM，报告 `fblen=2457600`、`yres_virtual=1200` 并实现 `pandisplay()`。页面更新先
完成后台页 cache clean，再登记到 DSI DMA；GDMA 在整帧结束中断中更新下一页 LLI
源地址，同时释放一条 NuttX pan 队列并发送 VSync 通知。验收时应执行：

```text
nsh> fb
nsh> lvgldemo widgets &
```

预期 `fb` 显示 `fblen=2457600`、`yres_virtual=1200`；拖动 Widgets、连续切换页面或
触发动画时不得出现横向撕裂、花屏或输入卡死。该项通过后，才将 P3.2 的 LVGL 交互
稳定性标记为 PASS。

### P2：LVGL 静态 Smart Home 首页

**目的**：在不依赖网络、触摸和外部资源的前提下验证 UI 初始化、布局与 framebuffer
flush。

**拟修改文件**：

| 文件 | 改动 |
| --- | --- |
| `board/.../configs/smart_home/defconfig`（新增） | 以 P1 配置为基线，选择 LVGL、NuttX framebuffer、`SMART_HOME_DEMO`、`SMART_HOME_DEMO_UI_LVGL` 与静态首页分支。 |
| `demos/smart_home/src/app/smart_home_static_main.c`（新增） | P2 专用入口；只启动静态 LVGL 首页，禁止调用网络与 cAGENT 初始化。 |
| `demos/smart_home/src/ui/lvgl/smart_home_lvgl_static.c`（新增） | 以固定设备/环境夹具构造首页，验证 LVGL 直写 `/dev/fb0` 与 `FBIO_UPDATE` flush。 |
| `demos/smart_home/src/ui/lvgl/smart_home_lvgl.c` | 将完整 UI 的显示路径改为配置项；P4X 使用 `/dev/fb0`，现有 LCD 目标保留 `/dev/lcd0` 兼容路径。 |
| `demos/smart_home/Kconfig`、`Makefile`、`CMakeLists.txt` | 增加静态 LVGL 分支、可覆盖的 framebuffer 路径；静态分支不选择 cAGENT，完整 Smart Home 分支行为保持不变。 |

**配置边界**：

- 启用 `GRAPHICS_LVGL`、`LV_USE_NUTTX` 与 framebuffer 后端；不选择
  `LV_USE_NUTTX_LCD`；
- 先使用非 libuv 的 `lv_timer_handler() + usleep()` 循环，降低任务模型变量；
- 只展示首页、底部导航和模拟设备卡片；不初始化自动 MCP discovery、Node gateway、
  App Bridge、云端模型请求、网络与触摸；
- P4X 分辨率为 1024×600，布局需以实际 `lv_display` 分辨率计算，不能以 UI 默认的
  320×240 常量作为渲染尺寸。

**P2 固件配置**：

```text
CONFIG_ESP32P4_FUNCTION_EV_BOARD_DSI_FRAMEBUFFER=y
CONFIG_GRAPHICS_LVGL=y
CONFIG_LV_COLOR_DEPTH_16=y
CONFIG_LV_USE_NUTTX=y
# CONFIG_LV_USE_NUTTX_LCD is not set
CONFIG_SMART_HOME_DEMO=y
CONFIG_SMART_HOME_DEMO_UI_LVGL=y
CONFIG_SMART_HOME_DEMO_STATIC_LVGL_HOME=y
CONFIG_SMART_HOME_DEMO_LVGL_FB_PATH="/dev/fb0"
```

**构建、烧录与运行**：

```bash
cd ~/openvela
export PATH="$PWD/prebuilts/gcc/linux-x86_64/riscv-none-elf/bin:$PATH"

./build.sh \
  contest2026_031_niudanxianqianchong/board/esp32p4/esp32p4-function-ev-board/configs/smart_home \
  -j2

esptool --chip esp32p4 --port /dev/ttyACM0 --baud 921600 \
  write-flash -fs 16MB -fm dio -ff 80m 0x2000 nuttx/nuttx.bin

picocom -b 115200 /dev/ttyACM0
```

```text
nsh> ls /dev/fb0
nsh> smart_home
```

**通过条件**：

- 画面在 3 秒内显示 1024×600 静态首页：标题为 `Smart Home`，含三个环境指标、四张
  模拟设备卡片和底部 `Home / Chat / Settings` 导航；
- 串口依次出现 `[lvgl-static] lv_init`、
  `[lvgl-static] framebuffer=/dev/fb0 resolution=1024x600`、
  `[lvgl-static] dashboard shown; entering timer loop`；
- 不出现 DNS、TLS、cAGENT、MCP、Node 或密钥读取日志；
- 连续运行 10 分钟无 assert、看门狗复位、framebuffer 花屏或背光熄灭；
- 在启动前后分别记录 `ps`、`free`（本配置已启用 procfs；若提示未挂载，先执行
  `mount -t procfs /proc`），并保存完整串口日志与屏幕照片。

**P2 首屏真机结果（2026-08-24）**：PASS。实板确认 `/dev/fb0` 存在，执行
`smart_home` 后静态首页正常显示，串口依次输出：

```text
=== Smart Home Static LVGL P2 ===
starting local dashboard without network, cAGENT, or touch

[lvgl-static] lv_init
[lvgl-static] framebuffer=/dev/fb0 resolution=1024x600
[lvgl-static] dashboard shown; entering timer loop
```

该结果确认 P2 的 LVGL 初始化、1024×600 framebuffer 绑定和首帧刷新链路可用；连续
10 分钟运行、`ps/free` 基线及重启回归仍作为保留回归项，不将本次首屏验收扩大解释为
长期稳定性结论。详细记录见
[P4X LVGL 静态首页真机验收](../开发日志/编译/2026-08-24-ESP32-P4X-LVGL静态首页真机验收.md)。

**失败隔离**：

| 现象 | 首先检查 | 不应同时做的事 |
| --- | --- | --- |
| `/dev/fb0` 缺失或 `smart_home` 报 framebuffer 初始化失败 | 回退 `fb_probe` 执行 `fb`，检查 board late-init 和 P1 DSI 基线。 | 不接入触摸、网络或完整 cAGENT。 |
| 有日志但黑屏 / 花屏 | 复跑 `dsi_probe pattern 10`、`dsi_probe video 10`，再核对 LVGL 为 RGB565、`/dev/fb0`。 | 不调整模型栈或 TLS 配置。 |
| 启动后 assert / 重启 | 保存 `dmesg`、`dumpstack`、`ps` 与 `free`，先检查 LVGL 栈和 framebuffer flush。 | 不把 MCP、Node、App Bridge 一并打开。 |
| 出现网络、模型或密钥日志 | 检查静态配置是否同时设置 `SMART_HOME_DEMO_STATIC_LVGL_HOME=y`。 | 不通过补充 secrets.json 绕过问题。 |

**P2 退出与下一阶段切换**：P2 首屏已通过，应保留本配置作为显示回归固件；不要直接在这
个二进制中追加网络或触摸。P3/P4 另起增量配置时应取消：

```text
# CONFIG_SMART_HOME_DEMO_STATIC_LVGL_HOME is not set
```

随后才恢复完整 `smart_home_main.c`、cAGENT 和设备状态机，并且每次只增加一个能力。

### P3：GT911 触摸和本地工具

**目的**：验证真实输入可驱动面板、设置和本地设备状态，而不是先接入云端。

**前置条件**：P2 静态 UI 已稳定；已确认 GT911 共享 I2C0（SCL=GPIO8、SDA=GPIO7）。
官方 P4X adapter 未将 GT911 `RST/INT` 接到 SoC，因此 P3.1 固定采用 20 ms 轮询，
不伪造 GPIO 复位或中断配置。

**拟修改文件**：

| 文件 | 改动 |
| --- | --- |
| `drivers/nuttx/drivers/input/gt911.c/.h` | NuttX touchscreen lower-half：GT911 ID、触点解析与轮询 worker。 |
| `nuttx/drivers/input/{Kconfig,Make.defs,CMakeLists.txt}` | 新增 `CONFIG_INPUT_GT911`、临时 `CONFIG_INPUT_GT911_DIAGNOSTICS` 与 Make/CMake 构建入口。 |
| `board/.../src/esp32p4_touch.c`（新增） | 获取 I2C0，以 100 kHz 按 `0x5d -> 0x14` 自动探测，并以 20 ms 轮询注册 `/dev/input0`。 |
| `board/.../{Kconfig,include/board.h,src/Make.defs,src/CMakeLists.txt,src/esp32p4_bringup.c}` | 声明、构建并在 board late bring-up 中装配触摸设备。 |
| `app/gt911_probe/`（新增） | 在 LVGL 之前读取并打印原始 Down/Move/Up 事件。 |
| `board/.../configs/smart_home/defconfig` | 启用 I2C0 GPIO8/7、GT911 和探针应用；暂不向 LVGL 指定输入路径。 |

**P3.1 验收结果**：已验证 `/dev/input0` 和 `lpwork` 存在。临时启用
`CONFIG_INPUT_GT911_DIAGNOSTICS` 后，串口统计确认 `scans` 与 `queued` 同步增长、
`i2c_err=0`。排查过程修正了触点起始地址 `0x8150 -> 0x814f`，并将处理顺序
收敛为“读状态 -> 读触点 -> 清 `0x814e` -> `touch_event()`”。`gt911_probe`
已实测得到连续坐标和完整 `DOWN/MOVE/UP`。诊断开关现已关闭，
避免干扰 NSH；保留 Kconfig 入口供后续板级排障使用。

**P3.1 当前通过项**：`/dev/input0` 注册成功，`gt911_probe 15` 可重复输出
单指 `DOWN/MOVE/UP`，坐标落在 1024×600 范围且 track ID 稳定。多点识别、
坐标旋转和边界精度仍属 P3.1 剩余验收项。

**P3.1 与存储联合复测**：P4 I2C 已能输出原始错误掩码，将一次注册失败
收敛为 `raw=0x400` 的地址 NACK，而非 timeout 或 arbitration lost。板级现按
`0x5d -> 0x14` 自动探测，且仅在 `-EIO` 时回退地址；最新真机启动在
`0x5d/100kHz` 读取到 Product ID `911`、注册 `/dev/input0`，随后成功将
0x800000 起的 LittleFS 挂载到 `/data`。该结果证明当前顺序可以共存，仍需
通过多次断电冷启动覆盖地址回退和上电稳定性。

**P3.2 已实现、待真机验收**：`configs/smart_home/defconfig` 已取消静态首页分支，
启用 GT911、`LV_USE_NUTTX_TOUCHSCREEN`、`NETUTILS_CJSON`、
`SMART_HOME_DEMO_OFFLINE_UI` 与 64 KiB 应用栈。cJSON 是设备状态、后端配置和
技能元数据共用的 JSON 依赖，即使离线 UI 不启用网络/TLS 也必须保留。常规
`smart_home_main.c`、`smart_home_agent_app_init()`、
`smart_home_lvgl.c` 会参与构建；`/dev/input0` 由 LVGL NuttX port 创建为输入设备。
该配置同时启用 SPI Flash LittleFS：`0x800000` 起的 1 MiB 分区自动挂载到
`/data`，由 `scripts/make_p4x_littlefs_data_image.sh` 默认预置 skills、非敏感 JSON、
MiSans Normal 子集（设备路径为 `/data/res/fonts/MiSans-Normal.ttf`）及
`/data/res/icons/*.png`。完整 MiSans 字体不进入镜像，避免消耗约 7.6 MiB 的 Flash。
没有 `/data/res/skills/*.md` 时应用记录 warning 并跳过场景目录，模型 Key 缺失时聊天页
显示不可用状态，不应阻止首页、设置或本地设备面板出现。

**P3.2 通过条件**：启动日志出现非空 `indev`，例如
`[smart_home_lvgl] disp=... indev=... input=/dev/input0`；点击首页、对话、设置导航项
正确切换，坐标方向和边缘区域无明显偏移。页面本地控制随后再映射为 `set_light`、
`set_fan` 等本地工具，仍不引入云端。

### P4：以太网、DNS、TLS 与控制台 cAGENT

**目的**：在控制台先验证 P4X 网络和模型请求，避免把 TLS 阻塞或资源问题误判为 LVGL
问题。

**基础配置**：以现有 `configs/ethernet/defconfig` 的 EMAC、DHCP、DNS、TCP 配置为
来源，合并至 `configs/smart_home/defconfig`；保留 P4X USB Console。

**验证顺序**：

```text
nsh> ifconfig eth0
nsh> ifup eth0
nsh> renew eth0
nsh> ping <已知主机>
nsh> tls_probe api.deepseek.com 443 --repeat 3
nsh> smart_home "打开客厅灯，亮度35%"
```

**通过条件**：

- DHCP、DNS、TCP/TLS 各自有独立成功证据；
- 控制台模型请求完成，工具调用改变本地设备状态；
- 使用只读 `secrets.json` loader 获得 Key，不输出 Key；
- 未启用 MCP / Node / App Bridge 时，LLM 成功或失败均不影响 P2/P3 的 UI 存活。

### P5：LVGL 异步 AI 对话

**目的**：验证 cAGENT worker、TLS 和 LVGL 主循环的并发边界。

**实施要点**：

- 保持现有“agent worker 执行网络请求、LVGL 线程处理 UI 更新”的模型；
- 从 P4 栈水位和 PSRAM 可用量确定 `SMART_HOME_DEMO_STACKSIZE`、agent worker 栈、
  TLS buffer 的实际值，不照搬 Goldfish / S3 数值；
- 先关闭 MCP discovery；发送“你好”和单次本地工具指令，观察加载态、工具卡片和
  最终回复。

**通过条件**：连续 20 次对话 / 工具调用后，UI 始终响应，无互斥锁 assert、任务泄漏、
堆持续增长或 DSI underrun。

### P6：远程扩展能力分项测试

远程能力必须逐项开启、逐项验收，不能在 P5 首次成功后同时打开。

| 顺序 | 能力 | 前置条件 | 核心验收 |
| --- | --- | --- | --- |
| P6.1 | MCP | P4 TLS 与 P5 已通过 | Settings 手动 Discover 成功，远程工具导入数可见；失败不退出 UI。 |
| P6.2 | Node gateway | P4X EMAC、WebSocket listener 已通过 | Node 上线、catalog mutation、工具可见和离线回收均正确。 |
| P6.3 | App Bridge | P6.2 非前置，可独立启用 | `GET /v1/home/snapshot`、本地控制 HTTP 与鉴权通过；聊天 API 最后启用。 |

MCP、Node token、App Bridge token 均从 `/data/smart_home/secrets.json` 读取；每一项
可用性须在设置页和串口中同时可观察。

## 6. 构建、烧录与记录规范

P4X Smart Home 配置创建后，统一从工作区根目录构建：

```bash
./build.sh \
  contest2026_031_niudanxianqianchong/board/esp32p4/esp32p4-function-ev-board/configs/smart_home \
  -j2
```

烧录前应先确认目标板已经进入可识别的 USB Serial/JTAG 模式；烧录地址、flash mode 和
flash size 必须复用当前 P4X 已验证固件的产物规则，不在本计划中猜测新的 offset。

每个阶段至少记录：固件 commit、defconfig、烧录命令、面板/触摸接线、串口完整日志、
视觉照片或视频、`free` / `ps` / DSI 状态快照，以及结果（PASS / FAIL / BLOCKED）。

## 7. 风险与止损条件

| 风险 | 识别方法 | 止损动作 |
| --- | --- | --- |
| framebuffer cache 不一致 | `/dev/fb0` 写色后不刷新或局部花屏 | 回到 P1，以 `FBIO_UPDATE` 和 cache clean 单独验证。 |
| LVGL 资源过重 | 启动失败、PSRAM 紧张、字体加载失败 | 关闭 TinyTTF/运行时 PNG，保留内置字体与图标。 |
| UI 与网络相互影响 | P4 控制台模型成功而 LVGL 对话失败 | 先运行 P5 的 worker/栈诊断，禁止同时调试 MCP。 |
| TLS/熵源不可用 | `tls_probe` 失败 | 停留在 P4，先修复网络或 entropy，不改 UI。 |
| 触摸影响显示 | 触摸后花屏或 DSI 停止 | 回到 P3，隔离 I2C/IRQ 与 display 任务。 |
| 外部配置缺失 | `/data` 文件不存在导致桥接禁用 | 本地 UI 继续运行；只在相应 P6 阶段部署最小无密钥配置。 |

## 8. 最终验收矩阵

| 层级 | 必测命令 / 动作 | PASS 定义 |
| --- | --- | --- |
| DSI 基线 | `dsi_probe pattern 10`、`dsi_probe video 10` | 两条色条可见。 |
| framebuffer | `ls /dev/fb0`、`fb` | **已通过**：1024×600 RGB565 查询、PSRAM 映射、六步矩形绘制和 `FBIO_UPDATE` 均成功。 |
| 静态 UI | `smart_home` | 首页稳定显示 10 分钟。 |
| 触摸与本地控制 | 点击导航 / 控制卡片 | UI 和本地状态一致。 |
| 控制台 AI | `smart_home "..."` | 模型回复与本地工具调用完成。 |
| LVGL AI | 屏幕发送两类指令 | UI 保持响应，结果正确回填。 |
| MCP / Node / App | 分项操作 | 远程能力独立成功、独立失败可恢复。 |

## 9. 完成定义

P4X Smart Home Demo 的“首版完成”定义为：P2、P3、P4、P5 全部通过，即真机 LCD 上可
触摸操作本地设备面板，并可通过 LVGL 异步对话触发本地工具。MCP、Node 和 App Bridge
属于后续扩展验收，不阻塞首版完成，但必须保持接口与配置兼容。
