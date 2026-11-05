# ESP32-P4X SmartHome 整体架构

> 代码基线：`contest2026` @ `ff79c50`（2026-11-01）。文中路径相对比赛仓根目录；
> 行号可能随提交漂移，以函数名为准。
> 范围：从 ESP32-P4X 硬件、芯片/板级驱动、NuttX 框架，到 cAGENT 与 SmartHome
> 应用的整体分层、内存布局、线程模型、数据流与控制流。
> 证据口径：标"实测"的数字来自板端日志或构建产物；其余为代码与配置推导。
> 各外设的逐函数调用链与排障史见文末"延伸阅读"，本文不重复。

## 1. 系统概览

SmartHome 是运行在 ESP32-P4X Function EV Board 上的 1024×600 横屏中控面板：

- 触摸 LVGL 界面控制本地虚拟设备和米家网关设备；
- 自然语言对话经云端大模型（OpenAI 兼容接口）驱动工具调用；
- 摄像头实时预览，可选语音输入（ASR）、播报（TTS）和本地唤醒（KWS）。

openvela 以 Route A 方式接入：custom chip（`chips/esp32p4/`）+ custom board
（`board/esp32p4/`），由 manifest 链接进 `vendor/espressif/`。

### 1.1 硬件资源

| 资源 | 规格 | 软件出口 |
| --- | --- | --- |
| SoC | ESP32-P4 rev3.x，RISC-V HP 双核 400 MHz，768 KB 内部 SRAM，L2 Cache 256 KB / 64 B 行 | `CONFIG_ESP32P4_REV_MIN_301`、`CONFIG_SMP_NCPUS=2`（SMP 配置） |
| PSRAM | 32 MB（实测 Umem 统计），200 MHz，映射在 `0x48000000` 起 | 整块作为 NuttX 用户堆 |
| Flash | 16 MB；`0x600000` 起 10 MiB 为 LittleFS 数据分区 | `/data` |
| 显示 | EK79007 1024×600 RGB565，MIPI-DSI 2 lane × 1000 Mbps，RST=GPIO27，背光=GPIO26 | `/dev/fb0` |
| 触摸 | GT911，I2C0 100 kHz，地址 0x5d/0x14；默认 20 ms 轮询，可选中断模式 | `/dev/input0` |
| 摄像头 | SC2336，SCCB=I2C0 0x30；MIPI-CSI 2 lane RAW8 1024×600@30 → ISP → RGB565 | `/dev/video0` |
| 音频 | ES8311 codec，I2C0 0x18；I2S0 全双工 + GDMA；PA 使能 GPIO53 | `/dev/audio/pcm0`、`/dev/audio/pcm_in0` |
| WiFi | 板载 ESP32-C6 协处理器，SDIO slot1（CLK18/CMD19/D0–D3=14–17），复位 GPIO54，ESP-Hosted 协议 | `wlan0` |
| 以太网 | P4 内置 EMAC | `eth0`（代码就绪，未真机验收） |

I2C0 由 GT911、SC2336、ES8311 三个设备共用，各驱动通过 `esp_i2cbus_initialize()`
共享同一总线实例。

## 2. 分层架构

```text
┌───────────────────────────────────────────────────────────────────────────┐
│ L6 应用      demos/smart_home                                              │
│   UI: LVGL V9 首页/聊天/设置 + 设备/房间/摄像头弹窗  (src/ui/lvgl)           │
│   业务: device service | miloco 米家 | camera | voice | network | config    │
│   AI 胶水: agent app/backend/session | tools | skills | addons(可选)        │
├───────────────────────────────────────────────────────────────────────────┤
│ L5 中间件    packages/cAGENT (V2: ReAct/Tool/Skill/Context/Session/Policy)  │
│              packages/cagent_addons (Node/MCP，可选)                        │
│              apps: LVGL 9.1 | cJSON | mbedTLS | TFLM(KWS，可选) | netlib     │
├───────────────────────────────────────────────────────────────────────────┤
│ L4 OS 服务   NuttX: 调度(SMP) | VFS/LittleFS/procfs | TCP/IP+DHCPC+DNS      │
│              fb | input | V4L2(v4l2_cap) | audio upper-half | HP/LP work    │
├───────────────────────────────────────────────────────────────────────────┤
│ L3 器件驱动  nuttx/drivers: es8311.c | gt911.c* | ek79007.c* (*本仓维护)     │
│              board: esp32p4_sc2336.c (板级私有传感器驱动)                   │
├───────────────────────────────────────────────────────────────────────────┤
│ L2 板级装配  board/esp32p4/esp32p4-function-ev-board/src                    │
│   esp_bringup(): 引脚/复位/上电时序 → 注册 fb0/input0/video0/pcm/wlan0/data │
├───────────────────────────────────────────────────────────────────────────┤
│ L1 芯片层    chips/esp32p4/common/espressif                                 │
│   esp_start/esp_smp | esp_mipi_dsi(+dpi_panel) | esp_mipi_csi(+video)/isp   │
│   esp_i2s | esp_i2c | esp_hosted_{sdio,transport,wlan} | esp_spiflash_mtd   │
│   esp_allocateheap | esp_irq                                                │
├───────────────────────────────────────────────────────────────────────────┤
│ L0 厂商 HAL  esp-hal-3rdparty @ b90b1837（LL/HAL/ROM ld）+ 本仓 patches 0001–0008 │
└───────────────────────────────────────────────────────────────────────────┘
```

依赖方向自上而下，上层不跨层访问寄存器。有两个有意的例外：

- 应用层 `smart_home_network.c` 直接调用板级 `board_esp_hosted_wifi_connect()`
  下发 WiFi 凭据，因为 NuttX 没有通用的 ESP-Hosted 配置 ioctl；
- 摄像头服务通过 V4L2 用户态接口取帧，不直接依赖芯片层。

### 2.1 代码规模（`demos/smart_home/src`，`.c/.h/.cc`）

| 模块 | 行数 | 说明 |
| --- | ---: | --- |
| `ui/` | ≈37 000 | 含约 160 个生成的图标 C 数组；手写逻辑主要在 `smart_home_lvgl_*.c` |
| `voice/` | ≈8 200 | 其中 `kws/model.cc` 3125 行为模型常量 |
| `miloco/` | ≈3 400 | 米家网关 HTTP 客户端、轮询 worker、spec 解析、Agent 工具 |
| `addons/` | ≈3 200 | App Bridge / Node 网关 / MCP 客户端（默认未启用） |
| `device/`、`agent/`、`config/`、`net/`、`tools/`、`camera/`、`skills/` | ≈11 000 | 业务与 AI 胶水 |

## 3. 启动与初始化控制流

### 3.1 系统启动

```text
ROM bootloader (CONFIG_ESPRESSIF_SIMPLE_BOOT，无二级 bootloader)
 └─ __start → esp_start()                       chips/.../esp_start.c
     ├─ 映射 Flash IROM/DROM 到 Cache MMU，使能 L1/L2 总线
     ├─ esp_psram_init() + extram 内存自检
     └─ nx_start()
         ├─ up_allocate_kheap(): 内部 SRAM → 内核堆 (Kmem)
         ├─ up_allocate_heap():  整块 PSRAM → 用户堆 (Umem)
         ├─ [SMP] up_cpu_start(1): ets_set_appcpu_boot_addr(esp_cpu1_start)
         │        CPU1 启动，跨核中断(crosscore_int)作为 IPI
         ├─ board_late_initialize() → esp_bringup()     （见 3.2）
         └─ 启动 nsh_main；用户执行 `smart_home`
```

### 3.2 板级装配顺序（`esp_bringup()`）

| 顺序 | 步骤 | 结果 | 说明 |
| --- | --- | --- | --- |
| 1 | `board_camera_initialize()` | 注册 `/dev/video0` | 只注册，不碰硬件；首次 `open` 时才上电、探测 SC2336、初始化 CSI/ISP |
| 2 | 挂载 procfs、tmpfs | `/proc`、`/tmp` | |
| 3 | `board_i2c_init()` | I2C 总线 | |
| 4 | `board_es8311_initialize()` | `/dev/audio/pcm0`、`pcm_in0` | 两个节点都注册成功后才拉高 PA |
| 5 | `board_emac_init()` | `eth0` | 仅在启用 EMAC 时 |
| 6 | `board_esp_hosted_initialize()` | `wlan0` | C6 复位 → 等 1 s → SDIO 枚举 → RPC 初始化 → 注册 netdev；最多重试 2 次 |
| 7 | `board_mipi_dsi_fb_initialize(0)` | `/dev/fb0` | 面板复位、DSI/DPI 初始化、分配双页帧缓冲、开背光 |
| 8 | `board_gt911_initialize()` | `/dev/input0` | 双地址探测，进入轮询或中断模式 |
| 9 | `board_spiflash_init()` | `/data`（LittleFS） | 数据分区最后挂载 |

每一步失败只打日志、继续后续步骤，没有汇总的健康状态（见 §9 R7）。

### 3.3 应用启动（`smart_home_main.c: main()`）

```text
main()
 ├─ 自检分支: --selftest-miloco-write/read、--selftest-camera
 ├─ 网络就绪
 │   secrets.json 有 WiFi 凭据 → smart_home_network_connect_credentials()
 │      → board_esp_hosted_wifi_connect() (RPC: SetStorage/SetConfig/Connect)
 │      → 等 carrier → netlib_ifup → DHCP → getaddrinfo(DNS 校验)
 │   没有凭据 → smart_home_network_prepare_setup()（UI 内配网）
 │   失败只记录状态，继续启动
 ├─ smart_home_agent_app_init(&app)                 src/agent/smart_home_agent.c
 │   ├─ 设备状态: device_init → 读 state.json 恢复(原子提交) → device_service_init
 │   ├─ 模型: 默认值 → settings.json 选择后端
 │   ├─ backend_create: runtime/TLS/transport → agent_init → session(JSONL) → model
 │   ├─ 注册: Context(smart_home_state) → Skills(/data/res/skills) → 场景目录
 │   │         → 本地工具 ×9 → 米家工具 ×2 → 本地工具访问策略
 │   ├─ agent_set_event_callback → agent_start
 │   ├─ apply_model_config: 解析 API Key 并替换 provider；缺 Key 只禁用模型
 │   └─ [可选] App run service / Node 网关 / MCP 桥
 ├─ smart_home_miloco_start(): 米家 worker（SMP 下绑定 CPU1）
 ├─ [可选] App Bridge、voice_play_init（TTS）
 └─ smart_home_lvgl_run(&app)                       src/ui/lvgl/smart_home_lvgl.c
     ├─ lv_init → style_init(MiSans 整文件预读入 PSRAM，TinyTTF 共享)
     ├─ lv_nuttx_init(fb=/dev/fb0, input=/dev/input0)
     ├─ smart_home_lvgl_init: 构建屏保/首页/面板/网络/米家/聊天/设置等屏
     │   并恢复聊天历史；[KWS] 启动语音会话
     ├─ set_observer(smart_home_lvgl_event_cb)
     └─ for(;;){ process_pending; lv_timer_handler; process_pending; usleep }
```

不同错误的处理方式不同：

- 设备服务、Agent 内核、Skill 解析等失败：`app_init` 直接返回错误，`smart_home` 退出。
- 网络、模型密钥、Node/MCP 失败：降级运行，界面可以正常启动，错误信息写入 `system_status.last_error` 并在设置页显示。

## 4. 内存架构

### 4.1 堆划分（`CONFIG_MM_KERNEL_HEAP=y` + `CONFIG_ESPRESSIF_SPIRAM_USER_HEAP=y`）

```text
内部 SRAM (768 KB)                         PSRAM (32 MB, 0x48000000~)
┌──────────────────────────────┐          ┌──────────────────────────────────┐
│ .iram0.text   45 KB (ISR/Flash 期间代码)│ │ 用户堆 Umem = 整块 PSRAM          │
│ .dram0.data   16 KB          │          │  malloc / memalign / bulk_alloc  │
│ .dram0.bss   169 KB          │          │  应用对象、LVGL、帧缓冲、字体、   │
│ 中断栈 / IDLE 栈             │          │  模型缓冲、线程栈(多数)          │
│ 内核堆 Kmem (实测约 420 KB)  │          └──────────────────────────────────┘
│  kmm_*: 驱动对象、netdev、   │
│  IOB、TLS 以外的内核结构      │          Flash (16 MB, XIP via Cache)
└──────────────────────────────┘          ┌──────────────────────────────────┐
                                          │ .flash.text 1.0 MB  rodata 545 KB│
                                          │ 0x600000: LittleFS 10 MiB (/data)│
                                          └──────────────────────────────────┘
```

段大小来自当前镜像 `nuttx/nuttx` 的 `size -A`；内核堆的实测值见
`competition/开发日志/05-WiFi-C6托管适配.md` §5.4。

在这个配置下，FLAT 构建中应用调用 `malloc()` 拿到的都是 PSRAM。
`smart_home_bulk_alloc()` 在 P4 上就是 `malloc()`，S3 上才走专用的 PSRAM 池。
需要内部 SRAM 的场合必须显式使用内核堆或静态段。现在需要内部 SRAM 的只有
ISR、Flash 擦写期间运行的代码，以及 DMA 描述符等少量结构。

### 4.2 主要 PSRAM 占用（推导值）

| 对象 | 大小 | 所有者 | 生命周期 |
| --- | ---: | --- | --- |
| DSI 帧缓冲 2 页 × 1024×600×2 | 2.4 MB | `esp32p4_fb.c`（`memalign` 64 B） | 常驻 |
| MiSans 字体文件镜像 | 7.6 MB（实测 7 943 504 B） | `smart_home_lvgl_style.c` | 常驻 |
| 摄像头 V4L2 环形缓冲 3 × 1.2 MB | 3.5 MB | camera service | 预览期间 |
| 芯片层 CSI 暂存帧 3 × 1.2 MB | 3.5 MB | `esp_mipi_csi.c` | 预览期间 |
| 预览双缓冲 2 × 512×300×2 + 弹窗副本 | 0.9 MB | camera service / 弹窗 | 预览期间 |
| 模型请求 / 响应 / 解码缓冲 | 64 + 16 + 8 KB | agent backend | 常驻 |
| cAGENT workspace + scratch | 96 + 64 KB | agent backend | 常驻 |
| Session 编解码缓冲 | 64 KB + 2 × 128 KB 行缓冲 | `smart_home_session.c` | 常驻 |
| 米家响应缓冲 | 32 KB | miloco worker | 常驻 |
| 线程栈（agent 32 KB / miloco 48 KB / voice 32 KB …） | ≈200 KB | 各服务 | 随线程 |
| LVGL 对象与图片缓存 | 视页面而定 | LVGL（`LV_USE_CLIB_MALLOC`） | 常驻 |

常驻部分约 11 MB；打开摄像头预览后峰值约 19 MB，32 MB 内余量充足。

### 4.3 Cache 一致性规则

PSRAM 经 L2 Cache 访问，DMA 直接读写 PSRAM，因此每条 DMA 通路都在固定位置做 cache 维护：

| 通路 | CPU→设备（写回） | 设备→CPU（失效） |
| --- | --- | --- |
| DSI 扫描 | `FBIO_UPDATE` → 按页 clean 整页（1.2 MB） | — |
| CSI 采集 | — | HPWORK 帧 worker 中对 1.2 MB 暂存帧做 invalidate |
| I2S/ES8311 | TX 缓冲提交前 | RX 完成后 |
| SDIO（ESP-Hosted） | 由 SDMMC 控制器路径处理 | 同左 |

SMP 下，摄像头 worker 固定在 CPU0，与 DSI 扫描、LVGL 同核，保证帧缓冲的
cache 维护和使用不跨核（`smart_home_camera_service.c` 的注释说明了原因）。

### 4.4 ROM 函数绑定

ROM 中的 libc/libgcc 函数经 `esp32p4.rom.eco5.*.ld` 以绝对地址绑定。P4 ROM 是用
硬件浮点编译的，而本固件是 soft-float（`-mabi=ilp32`，没有 F 扩展）。所以所有浮点
辅助函数都必须用 `#ifndef __NuttX__` 屏蔽，退回工具链的软件实现，否则会在 `frrm`
指令上触发非法指令异常。HAL 补丁 `0008-esp32p4-rom-libgcc-guard-float-symbols.patch`
补齐了上游漏掉的 7 个符号。整数辅助函数（`__divdi3` 等）仍绑定 ROM。

## 5. 线程与并发模型

### 5.1 执行实体一览

| 实体 | 类型 | 优先级 | 栈 | CPU（SMP） | 职责 |
| --- | --- | ---: | ---: | --- | --- |
| `smart_home` 主任务 | task | 100 | 64 KB | 0 | 启动编排，之后就是 **LVGL 线程**（唯一允许调用 LVGL API 的线程） |
| agent worker | pthread，每条消息创建 | 继承 100 | 32 KB（PSRAM） | 继承 0 | `smart_home_agent_run()`，阻塞直到本轮对话完成 |
| miloco worker | pthread，常驻 | 继承 | 48 KB（PSRAM） | **1** | 每 5 s 轮询网关，执行控制队列，每 10 min 拉取天气并同步时间 |
| camera worker | pthread，按需 | 继承 | 8 KB | **0** | V4L2 DQBUF/QBUF，缩放到预览双缓冲 |
| voice capture | pthread | 110 | 8 KB | 继承 | 独占 `pcm_in0`，把帧分发给当前的单一消费者 |
| KWS worker | pthread | 90 | 32 KB（`CONFIG_SMART_HOME_KWS_WORKER_STACKSIZE`，range 8~128 KB） | 继承 | 特征前端 + TFLM 推理 |
| voice play worker | pthread | 90 | 32 KB | 继承 | TTS 请求与 PCM 播放 |
| ASR worker | pthread，按需 | 继承 | Kconfig | 继承 | 录音 + 云端识别 |
| V9 设置 model/net worker | pthread，按需 | 继承 | 16 KB | 继承 | 模型切换、配网，避免阻塞 UI |
| 房间保存 worker | pthread，按需 | 继承 | 32 KB | 继承 | 后台写 `state.json` |
| HPWORK | 内核 work | 224 | 2 KB | — | I2S TX/RX worker、CSI 帧 worker |
| LPWORK | 内核 work | 100 | 4 KB | — | GT911 轮询，ESP-Hosted 收包 |

`CONFIG_SMP_DEFAULT_CPUSET=0x1`：没有显式绑核的任务和线程都只在 CPU0 上运行。
目前只有 miloco worker 被放到了 CPU1（见 §9 R3）。

### 5.2 跨线程通信规则

LVGL 运行在 `LV_OS_NONE` 模式，非线程安全。项目统一遵守"**后台线程不碰
LVGL，结果投递回 UI 线程**"的规则：

```text
后台线程 ──(calloc 节点)──▶ pending 队列 ──(每次主循环两次 drain)──▶ LVGL 线程
           agent 事件拷贝            pending_mutex 保护单链表             渲染/更新控件
           UI_PENDING_DONE / ASR_TEXT / KWS_WAKE
```

- cAGENT 的事件回调在 agent worker 上执行。`smart_home_lvgl_event_cb()` 只把事件深拷贝后入队，不在回调里调用 LVGL，也不重入 Agent。
- 设备状态和米家快照走**拉模式**：UI 用 `lv_timer` 按 1 s 周期，通过 `copy_state` / `miloco_list` 复制快照，再比较 revision 决定是否刷新。
- 摄像头预览由 66 ms 的 `lv_timer` 调用 `smart_home_camera_copy_latest()`，把最新一帧复制到弹窗自己的缓冲区，再 `lv_image_set_src`。

### 5.3 共享状态与锁

| 锁 | 保护对象 | 持有者 | 最长持有时间 |
| --- | --- | --- | --- |
| `app->agent_mutex` | `agent_t`、模型后端、Session、工具访问策略 | agent worker、设置 worker、LVGL（部分 API） | **一整轮对话**（`agent_run` 全程，最长到模型超时） |
| `device_service.mutex` | `smart_home_state_t`、revision、事件环 | 工具 handler、UI | 微秒级 |
| miloco `lock_state` | 设备快照、控制队列（深度 8）、结果环、配置 | miloco worker、工具、UI | 微秒级；网络 I/O 在锁外 |
| camera `g_camera.lock` | 预览双缓冲索引与状态 | camera worker、UI | 复制一帧（300 KB）的时间 |
| `ui->pending_mutex` | pending 队列 | 所有后台线程、LVGL | 微秒级 |
| Agent 内核状态机 | CONFIGURING → READY ⇄ ACTIVE | cAGENT | `agent_run` 期间为 ACTIVE，重入时返回 BUSY |

## 6. 数据流

### 6.1 触摸 → 界面 → 显示

```text
GT911 ─I2C0─▶ gt911.c (LPWORK 20ms 轮询 | GPIO 中断) ─▶ /dev/input0
  ─▶ lv_nuttx touchscreen indev(read) ─▶ LVGL 事件分发 ─▶ 控件回调
  ─▶ LVGL SW 渲染 (1 个 draw unit，RGB565，DIRECT 模式直接画进 fb 后备页)
  ─▶ FBIO_UPDATE: 按页 cache clean ─▶ FBIOPAN_DISPLAY 翻页
  ─▶ esp_mipi_dsi_dpi_panel: DW-GDMA 读帧 ─▶ DSI Bridge ─▶ D-PHY 2×1 Gbps ─▶ EK79007
```

- 显示刷新周期为 33 ms（`LV_DEF_REFR_PERIOD`）。
- 面板以 60 Hz 从 PSRAM 读出整帧，持续占用约 74 MB/s 带宽。

### 6.2 摄像头预览

```text
SC2336 RAW8 1024×600@30 ─MIPI 2 lane─▶ CSI-2 Host ─▶ ISP(在线 demosaic→RGB565)
 ─▶ CSI Bridge ─▶ GDMA ─▶ PSRAM 暂存帧×3
 ─[ISR 只记账]─▶ HPWORK frame_worker: invalidate 1.2MB → memcpy → V4L2 缓冲
 ─▶ v4l2_cap complete_capture ─▶ /dev/video0 poll 可读
 ─▶ camera worker(CPU0): DQBUF → 每 2 帧缩放 1024×600→512×300 → 预览双缓冲 → QBUF
 ─▶ 摄像头弹窗 lv_timer(66ms): copy_latest → lv_image
```

- 背压：暂存环用尽时，ISR 暂停 Bridge 和 GDMA，丢弃整帧而不是覆盖写；缓冲归还后自动重新启动。
- 带宽：采集 37 MB/s，HPWORK 拷贝 37 MB/s，应用缩放每秒约 5 MB，加上 §6.1 的 DSI 扫描，PSRAM 总负载超过 150 MB/s。这是 PPA/2D-DMA 卸载的首要目标（见 PPA 规格）。

### 6.3 对话（AI 主链路）

```text
用户输入(键盘/ASR 文本)
 └─ LVGL: chat_send_text → 若 request_inflight 则拒绝 → 创建 agent worker
     └─ smart_home_agent_run()  [持 agent_mutex]
         └─ cAGENT agent_run(): ReAct 循环 (max_steps=8, max_tool_calls=4)
             ├─ Session 打开本轮 (JSONL，投影最近 3 个完整回合)
             ├─ Context 投影:
             │    system prompt (中文、简短、工具规则)
             │  + Skill 摘要 ×6 ("Use read_skill(name=...)")
             │  + smart_home_state 设备快照 (≤2KB, REFERENCE, 优先级 80)
             │  + 历史消息 + 工具 schema
             ├─ model_step: OpenAI provider 组装 JSON (请求缓冲 64KB)
             │   └─ transport ─▶ mbedTLS (16KB in/out) ─▶ socket
             │       ─▶ TCP/IP ─▶ wlan0 ─▶ esp_hosted_wlan ─▶ transport 封帧
             │       ─▶ SDIO CMD53 (512B 块+尾字节) ─▶ C6 ─▶ WiFi ─▶ 云端模型
             │   ◀─ 响应: C6 ─SDIO 中断─▶ LPWORK rx_work ─▶ netdev 输入 ─▶ TLS ─▶ 解析
             ├─ 若含 tool_calls: Policy 检查 → handler (每个 ≤3s) → 结果写回 Session
             │    本地工具 → device service (mutex + revision + 事件)
             │    miot_device_control → miloco 控制队列 (立即返回 "queued")
             │    read_skill → 返回 Skill 全文
             └─ 无 tool_calls → final 文本 → 写入输出缓冲(4KB, UTF-8 安全截断)
     ├─ 每个事件(TURN/MODEL/TOOL BEGIN/END) → observer → pending 队列 → UI 思考/工具标签
     └─ 完成 → UI_PENDING_DONE → 回复气泡；[TTS] voice_play 播报
```

- 模型的调用方式：使用 OpenAI 兼容的 Chat Completions 接口，`backends.json` 中有多个预设，`settings.json` 记录当前选中的后端。API Key 按以下顺序解析：当次手工输入 > `secrets.json` > 无（无 Key 时模型不可用）。
- Session 持久化：位置 `/data/smart_home/sessions/`，最多保留 20 个回合、1 MiB；重启后可以恢复聊天记录。

### 6.4 米家网关

```text
miloco worker (CPU1) 主循环:
  ├─ 每 10 min（未成功时每 1 min）: wttr.in 天气 + HTTP Date 时间同步
  ├─ 每 5 s: GET /api/miot/status → 已绑定则 GET /api/miot/device_list
  │          → 新设备 GET .../spec (≤32KB) 解析可控项 → 可控设备回读状态
  ├─ 控制队列: POST /api/miot/devices/<did>/control → 结果写入结果环
  └─ sem_timedwait(5s) 等待下一轮，控制请求到达时提前唤醒
网关: 局域网 Miloco 服务 (默认端口 1810，明文 HTTP + token)
消费者: UI 1s 定时拉取快照；设备弹窗用 submit_control_tracked + control_result 确认
        Agent 工具 miot_device_list 读快照，miot_device_control 入队
```

### 6.5 语音（`smart_home_voice` 配置）

```text
采集: 麦克风 → ES8311 ADC → I2S0 RX GDMA → HPWORK i2s_rx_worker → audio upper-half
      → /dev/audio/pcm_in0 (16kHz/16bit/单声道)
      → voice capture worker(110) → 单一活跃消费者回调（只做 memcpy/入队）
         ├─ KWS: 帧 → 特征前端 → TFLM wake_large int8 → 唤醒事件 → pending → UI
         └─ ASR: 录音 ≤N 秒 → MiMo 云端 ASR → 文本 → pending → 进入 §6.3
播报: 回复文本 → voice play worker(90) → TTS 云端(多家预设) → PCM
      → /dev/audio/pcm0 → I2S0 TX GDMA → ES8311 DAC → NS4150B 功放
      播报期间暂停 KWS 推理，避免自激
```

### 6.6 持久化数据（`/data`，LittleFS）

| 路径 | 内容 | 写入方 |
| --- | --- | --- |
| `/data/res/fonts`、`/data/res/icons`、`/data/res/skills` | 字体、PNG 图标、6 个 Skill（Markdown） | 由数据镜像烧录，运行时只读 |
| `/data/smart_home/backends.json` | 模型后端预设 | 镜像 / 设置页 |
| `/data/smart_home/settings.json` | 当前后端等偏好 | 设置页、`apply_model_config` |
| `/data/smart_home/state.json` | 房间、本地设备、米家设备的房间覆盖 | 房间弹窗后台 worker |
| `/data/smart_home/secrets.json` | WiFi 凭据、模型 Key、米家 token（**明文**） | 配网、设置；不进入数据镜像 |
| `/data/smart_home/sessions/` | 对话 JSONL | cAGENT Session |

写入统一用"写临时文件 → `fclose` → `rename`"的原子替换；读取失败时回退到默认值（`RECOVER_DEFAULT`）。

## 7. 关键控制流

### 7.1 设备控制：UI 与 Agent 两条入口

```text
            ┌─ UI 设备弹窗 ─────────────┐        ┌─ Agent 工具 ──────────────┐
本地设备    │ set_device_control()      │        │ set_light / set_ac /       │
           │ 同步, mutex, revision+1    │        │ run_scene → device service │
           └──────────┬────────────────┘        └──────────┬────────────────┘
                      ▼                                     ▼
            smart_home_state_t ──(下次 1s 轮询/上下文投影)──▶ UI / 模型
米家设备    UI: submit_control_tracked(id) → 轮询 control_result(id) → 弹窗确认/超时
           Agent: submit_control → 立即返回 queued → 下一轮轮询后才反映真实状态
                      ▼
            miloco 控制队列(8) → worker POST → 结果环 → 回读状态 → revision+1
```

### 7.2 Agent 单轮状态机

```text
READY ──agent_run──▶ ACTIVE: TURN_BEGIN
   ▲                  │  loop ≤8 次 { MODEL_BEGIN → 模型 → MODEL_END
   │                  │             [tool_calls] → TOOL_BEGIN → handler → TOOL_END }
   │                  │  超时(整轮) / 取消 / LIMIT / CAPACITY → 中止
   └──── TURN_END ◀───┘  Session 落盘，交付输出(可能截断)
```

### 7.3 摄像头弹窗生命周期

```text
OFF ─open─▶ STARTING (camera_request(on) → 打开 video0 → REQBUFS/QBUF×3 → STREAMON)
    ─首帧─▶ PREVIEWING (66ms 刷新) ─关闭/切页/退出─▶ STOPPING (STREAMOFF, 释放缓冲) ─▶ OFF
    任一步失败 ─▶ ERROR (显示原因，可重试)
```

### 7.4 网络状态

```text
init(ifup) ─▶ carrier(C6 STA_CONNECTED 事件) ─▶ ip(DHCP) ─▶ dns(getaddrinfo) ─▶ online
每一级有独立的状态码 (init/ip/dns)；任一级失败都继续启动，由设置页显示并提供重试
```

## 8. 构建与配置矩阵

| 配置 | 核数 | 触摸 | 音频 | 语音 | 工具链 | 用途 |
| --- | --- | --- | --- | --- | --- | --- |
| `smart_home` | 1 | **defconfig 未启用 GT911** | ES8311 | — | xPack | 单核主线 |
| `smp_smart_home` | 2 | GT911 | — | — | 乐鑫 `riscv32-esp-elf` | 当前开发主线（构建用 `scripts/build_p4x.sh`） |
| `smart_home_voice` | 1 | — | ES8311 | TTS + ASR | xPack | 语音链路验证 |
| `smart_home_local` / `smp_smart_home_local` | 同上 | | | | | 叠加本地 WiFi 凭据，不入库 |

公共工作区（`nuttx/`）的改动以 `patches/nuttx/0001–0013` 维护，HAL 的改动以
`chips/esp32p4/common/espressif/patches/0001–0008` 维护，构建脚本会自动打上补丁。

## 9. 架构评估：风险与建议

按影响从高到低排列。

| ID | 问题 | 影响 | 建议 |
| --- | --- | --- | --- |
| R1 | `agent_mutex` 覆盖整轮 `agent_run`（含网络，最长到模型超时）。LVGL 线程会直接调用以下同样加锁的接口：V9 设置页工具开关回调里的 `set_local_tool_access`；旧设置页 `apply_settings_event`、`local_tool_switch_event` 和 `smart_home_lvgl_refresh_tool_directory`（后者在页面切换时也会被调用）；启动时 `chat_restore` 调用的 `visit_history` | 对话进行中操作这些控件，UI 线程会阻塞到本轮结束，表现为整屏卡死几秒到几十秒。`chat_clear` 有 `request_inflight` 保护，不受影响 | 工具访问策略改用独立的小锁，或改成原子数组，不要和 Agent 内核共用一把锁；UI 线程上所有需要拿 `agent_mutex` 的调用一律改为 worker 执行或用 trylock |
| R2 | 栈上大数组：`poll_device_list` 帧 38 KB，`smart_home_v9_collect_views`、`append_miloco_cards`、`miloco_sheet_build` 等各约 20 KB，都在栈上放 `smart_home_miloco_device_t[16]`（每个 1188 B） | 主线程静态最坏深度约 41 KB（栈 64 KB）；miloco worker 约 40 KB（刚从 24 KB 加到 48 KB）。余量依赖人工估算，加功能后容易溢出，而且溢出踩的是 PSRAM，现象会延后出现 | 快照数组改为服务持有的静态或堆缓冲，复用一份；CI 中加入基于反汇编的静态栈深检查 |
| R3 | `SMP_DEFAULT_CPUSET=0x1`，只有 miloco 绑到了 CPU1；agent worker（JSON 组包、TLS 加解密）、语音 worker 都和 LVGL、摄像头挤在 CPU0 | 对话或语音进行中界面掉帧；第二个核基本空闲 | agent worker、voice play、ASR worker 显式绑定 CPU1；LVGL、摄像头、DSI 相关保留在 CPU0 |
| R4 | PSRAM 带宽：DSI 扫描约 74 MB/s、摄像头约 75 MB/s、LVGL 全屏绘制、字体光栅化都在同一块 PSRAM 上 | 预览打开时 UI 帧时间变长 | 摄像头缩放交给 PPA；DSI 改为局部刷新（`updatearea` 目前按整页 clean，见缺陷清单 B-12）；LVGL 绘制缓冲和热点对象考虑放内部 SRAM |
| R5 | Agent 控制米家是异步的：工具返回 "queued"，真实结果要等下一次轮询（最多 5 s） | 模型可能在控制失败时仍回复"已完成" | 工具内等待一个短暂的结果窗口（例如 2 s，取 `control_result`），把真实结果返回给模型 |
| R6 | 凭据明文存放在 `secrets.json`；米家网关走明文 HTTP | 拿到 Flash 镜像或在局域网内嗅探即可获得凭据 | 比赛阶段可接受；产品化时需要 Flash 加密或 eFuse 密钥派生，网关改用 HTTPS 或签名 |
| R7 | 板级装配没有健康汇总：各步骤失败只打日志 | 外设缺失时到应用层才暴露，难以定位 | `esp_bringup` 输出一个 `ready_mask`，应用启动时打印，并在设置页显示 |
| R8 | 配置矩阵不对称：`smart_home` 没有 GT911，`smp_smart_home` 没有音频 | 干净构建出来的固件功能不完整 | 以 `smp_smart_home` 为基础补齐 ES8311 和语音，形成唯一的产品配置 |
| R9 | UI 状态更新是 1 s 拉模式 | 本地控制后 UI 最多延迟 1 s；空闲时的周期复制也有开销 | 设备服务已有事件监听接口（`set_event_listener`），可以改为"事件入 pending 队列"推送，保留低频轮询作兜底 |

## 10. 延伸阅读

| 主题 | 文档 |
| --- | --- |
| 摄像头逐函数调用链与带宽 | `docs/硬件适配/ESP32-P4X-SC2336-视频通路架构与调用链.md` |
| DSI 测试调用链 | `docs/开发计划/显示与触摸/ESP32-P4X-LCD测试调用全链路.md` |
| ESP-Hosted 分层与排障 | `competition/开发日志/05-WiFi-C6托管适配.md` |
| 音频适配 | `competition/开发日志/07-音频ES8311适配.md` |
| Session 持久化 | `docs/开发指南/SmartHome-Session持久化.md` |
| 编译、烧录与资源部署 | `docs/开发指南/ESP32-P4X-SmartHome编译烧录与资源部署指南.md` |
| cAGENT 内核 | `packages/cAGENT/README.md` |
| 驱动缺口与已知缺陷 | `docs/开发计划/驱动缺口与BUG清单.md` |
