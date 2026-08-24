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
| NuttX framebuffer 设备 | 单缓冲真机 PASS；双缓冲待验收 | `fb_probe` 在 board late-init 注册 RGB565 `/dev/fb0`；双页 PSRAM、`FBIOPAN_DISPLAY` 与 DMA 帧边界换页已实现，待实板确认无撕裂。 |
| LVGL Smart Home 页面 | P2 已实现，真机待测 | 新增静态首页分支，使用 `/dev/fb0`；完整 UI 路径已改为 Kconfig 指定显示设备。 |
| GT911 触摸 | P3.1 单指真机通过 | `/dev/input0` 与 `gt911_probe` 已验证 `DOWN/MOVE/UP`、坐标和触摸面积；多点和 LVGL 接入待验证。 |
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
P3：GT911 触摸与本地控制回归
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

1. 新增独立配置目录，而不修改 `dsi_probe/defconfig`：

   ```text
   board/esp32p4/esp32p4-function-ev-board/configs/smart_home/
     defconfig
   ```

   它以 `dsi_probe/defconfig` 为显示基线，并选择 `SMART_HOME_DEMO`。

2. framebuffer 支持双 RGB565 扫描页：

   ```text
   1024 × 600 × 2 B × 2 = 2,457,600 B
   ```

   两页连续放入 PSRAM。NuttX framebuffer 通过 `yres_virtual=1200` 暴露双页，
   调用方使用 `FBIOPAN_DISPLAY` 提交后台页；板级仅在 DW-GDMA 完成当前整帧后
   切换下一轮扫描源地址，不引入全屏软件复制。

3. 首版 LVGL 使用内置 Montserrat 字体和已编译的图标字形；不把 FreeType、外置
   MiSans 字体、PNG 文件部署或 `/data` 挂载作为首屏验收条件。文件资源在后续视觉
   优化阶段单独恢复。

4. 首版关闭 `SMART_HOME_MCP_BRIDGE`、`SMART_HOME_NODE_GATEWAY`、
   `SMART_HOME_APP_BRIDGE`，并不配置真实 API Key。这样首屏失败只能归因于
   framebuffer、LVGL 或显示驱动。

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

尚未完成的增强验证是连续执行 10 次、长时间扫描以及与 LVGL 并发刷新；这些不阻塞
进入 P2，但应在 P2 回归项中保留。

### P2：LVGL 静态 Smart Home 首页

**目的**：在不依赖网络、触摸和外部资源的前提下验证 UI 初始化、布局与 framebuffer
flush。

**拟修改文件**：

| 文件 | 改动 |
| --- | --- |
| `board/.../configs/smart_home/defconfig`（新增） | 以 P0/P1 配置为基线，选择 LVGL、NuttX framebuffer、`SMART_HOME_DEMO`、`SMART_HOME_DEMO_UI_LVGL` 和必要的 builtin app。 |
| `demos/smart_home/src/ui/lvgl/smart_home_lvgl.c` | 将显示路径改为配置项：P4X 使用 `/dev/fb0`，保留现有 `/dev/lcd0` 兼容路径；初版不启用 libuv。 |
| `demos/smart_home/Kconfig` | 增加可覆盖的 LVGL framebuffer 路径或明确的 P4 framebuffer 选择，避免用芯片型号猜设备路径。 |
| `demos/smart_home/src/ui/lvgl/smart_home_lvgl_style.c` | 确保外置字体不可用时稳定回退到编译进固件的 Montserrat。 |

**配置边界**：

- 启用 `GRAPHICS_LVGL`、`LV_USE_NUTTX` 与 framebuffer 后端；不选择
  `LV_USE_NUTTX_LCD`；
- 先使用非 libuv 的 `lv_timer_handler() + usleep()` 循环，降低任务模型变量；
- 只展示首页、底部导航和模拟设备卡片；禁用自动 MCP discovery、Node gateway、
  App Bridge、云端模型请求；
- P4X 分辨率为 1024×600，布局需以实际 `lv_display` 分辨率计算，不能以 UI 默认的
  320×240 常量作为渲染尺寸。

**操作与通过条件**：

```text
nsh> smart_home
```

- 画面在 3 秒内显示，不依赖网络成功；
- 串口出现 `[lvgl] show done, entering run loop`；
- 连续运行 10 分钟无 assert、看门狗复位或 framebuffer 花屏；
- 使用 `ps`、`free`（需 procfs）记录任务数和内存基线。

### P3：GT911 触摸和本地工具

**目的**：验证真实输入可驱动面板、设置和本地设备状态，而不是先接入云端。

**前置条件**：P2 静态 UI 已稳定；P4X 的 GT911 I2C、INT、RST 引脚和板级电源连接已
核实。

**拟修改文件**：

| 文件 | 改动 |
| --- | --- |
| `board/.../src/esp32p4_touch.c`（新增） | 初始化 I2C + GT911，并注册标准触摸输入设备。 |
| `board/.../include/board.h`、`src/Make.defs` | 声明和编译触摸板级装配。 |
| `board/.../configs/smart_home/defconfig` | 启用 I2C、GT911、`INPUT_TOUCHSCREEN` 与实际输入设备路径。 |
| `smart_home_lvgl.c` | 通过配置指定 P4X 输入路径，不复用 ESP32-S3 专用宏。 |

**通过条件**：

- 点击首页、对话、设置三个导航项，页面正确切换；
- 本地 `set_light`、`set_fan` 工具可由页面控件修改设备状态；
- 触摸坐标、旋转和边缘区域无明显偏移；
- 触摸高频输入下 DSI 连续扫描不花屏。

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
| LVGL 资源过重 | 启动失败、PSRAM 紧张、字体加载失败 | 关闭 FreeType/运行时 PNG，保留内置字体与图标。 |
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
