# ESP32-P4X SmartHome 卡顿、线程与内存优化方案

> 代码基线：`contest2026` @ `491ffa9`（2026-11-01）；当前镜像 `smp_smart_home`。
> 状态：方案，未实施。基线数据只有 Host 测得的 `[v9-perf] avg_ms=12 max_ms=48`，
> 没有板端帧时间数据，所以每项收益都**以板端实测为准**。
> 前置文档：[整体架构](ESP32-P4X-SmartHome整体架构.md)、[问题分析](ESP32-P4X-SmartHome问题分析.md)、
> [LVGL 性能优化方案（10-25）](../开发计划/应用与AI/ESP32-P4X-SmartHome-LVGL性能优化方案.md)。
> 本文取代 10-25 方案中已经过时的部分（见 §1.2）。

## 1. 背景

### 1.1 当前帧管线（每帧固定开销）

```text
lv_timer_handler (33ms 周期, 单 draw unit, CPU0)
  └─ 渲染脏区 → DIRECT 模式直接画在后台页 (PSRAM)
  └─ flush (只在最后一块):
       FBIO_UPDATE(脏区并集) → esp32p4_fb_updatearea: clean 整页 1.2MB   ← ①
       FBIOPAN_DISPLAY        → esp32p4_fb_pandisplay:  再 clean 整页 1.2MB ← ②
       → DSI GDMA 下一帧切到该页
  └─ LVGL DIRECT 双缓冲 sync: 把本帧脏区拷贝到另一页 (PSRAM→PSRAM)
同时: DSI 以 60Hz 从 PSRAM 读整页 ≈ 74 MB/s；摄像头开启时再加 ≈ 75 MB/s
```

### 1.2 10-25 方案的核对结果

| 10-25 条目 | 现状（V9 代码） |
| --- | --- |
| 退出安防页时 `camera_stop` 的 join 阻塞 UI | **已解决**：V9 改为摄像头弹窗，`smart_home_camera_request(false)` 交给 detached worker 处理 |
| 设备页状态一变就整网格重建 | **V9 首页已解决**：key 集合不变时原地更新；旧 `panel.c` 仍会重建，但它已经不在 V9 导航里 |
| 全屏 MOVE 转场 | **部分解决**：V9 三个 Tab 用 220 ms FADE_IN；旧页面仍用 MOVE_LEFT |
| TinyTTF 字形缓存只有 128 | **仍存在**：`lv_tiny_ttf.c:149` 写死为 128，`cache_size` 参数被忽略 |
| 启动时把全部页面预构建 | **仍存在**：`smart_home_lvgl_init` 会构建 panel/network/miloco/bind/settings/model/tools/mcp/node 等旧页面 |

## 2. 问题清单

按"对体感的影响"排序。每条都标注了证据等级：已证实，或推断。

### 2.1 卡顿

| ID | 现象 | 根因 | 证据 |
| --- | --- | --- | --- |
| J1 | 对话进行中操作设置页或工具开关，整屏冻住数秒 | UI 线程等待 `agent_mutex`，而这把锁在整轮 `agent_run` 期间（包括网络）都被占用（问题分析 P1-1） | 已证实（代码路径） |
| J2 | 每帧都有固定的 cache 开销，哪怕只改了一个 label | 每帧对 1.2 MB 整页做**两次** cache clean（上图 ①②），按脏区算本来只需要 KB 级 | 已证实（`esp32p4_fb.c` 的 `updatearea` / `pandisplay`） |
| J3 | 首页刷新时卡片图会闪一下或顿一下；切房间、开关设备时尤其明显 | 卡片图是 `A:/data/res/devices/*.png`，但 `LV_CACHE_DEF_SIZE=0`，图片缓存关闭，**每次重绘都要从 LittleFS 读文件并用 lodepng 重新解码** | 已证实（配置 + `lv_image_decoder.c`） |
| J4 | 第一次进入聊天、设置页，或滚动中文长文本时卡顿 | TinyTTF 全局字形缓存只有 128 项，5 种字号共用 | 已证实 |
| J5 | 打开摄像头弹窗后整体掉帧 | PSRAM 带宽被 DSI 扫描、CSI 采集、HPWORK 拷贝、缩放、弹窗副本同时占用；缩放由 CPU 每两帧做一次 | 推断（带宽估算 > 150 MB/s） |
| J6 | 对话或语音进行时 UI 掉帧 | agent worker（JSON、TLS）、TTS、ASR 都和 LVGL 一起跑在 CPU0；`SMP_DEFAULT_CPUSET=0x1` | 已证实（配置） |
| J7 | 点首页场景按钮时短暂卡顿 | `v9_scene_run` 在 UI 回调里同步执行，栈上放了约 19 KB 快照 | 已证实 |
| J8 | 每秒一次的微卡 | `v9_poll_timer` 每秒复制设备快照和米家快照（约 19 KB），并重建视图数组 | 推断 |
| J9 | 启动慢 | 预构建约 10 个旧页面，加上 7.9 MB 字体整文件读入 | 已证实 |

### 2.2 线程

| ID | 问题 | 证据 |
| --- | --- | --- |
| T1 | 第二个核基本闲置：只有 miloco worker 绑在 CPU1 | 已证实 |
| T2 | 一把大锁 `agent_mutex` 同时保护 Agent 内核、模型配置和工具访问策略 | 已证实 |
| T3 | 每发一条消息就 `pthread_create` 一个 agent worker，结束后 join；栈复用了，但每轮都要创建线程 | 已证实 |
| T4 | 有 6 种按需创建的 detached 或 joinable 线程（摄像头控制、房间保存、V9 model/net、ASR、agent），各自管理生命周期，退出时序难以保证 | 已证实 |
| T5 | 优先级没有分层：UI 主线程 100、LPWORK 100（GT911 轮询和 ESP-Hosted 收包都在 LPWORK）、RR 时间片 200 ms。网络收包大量到达时，可能和 UI 在同一优先级上轮转 | 推断 |
| T6 | 音频 STOP 路径用了不可中断等待加 `pthread_join`，丢一次 EOF 就会永久卡住（问题分析 P1-6） | 已证实（代码路径） |

### 2.3 内存

| ID | 问题 | 证据 |
| --- | --- | --- |
| M1 | 栈上大数组：12 处 `smart_home_miloco_device_t[16]`（19 KB），主线程最坏约 41 KB / 64 KB，miloco worker 约 40 KB / 48 KB | 已证实（静态分析） |
| M2 | 摄像头三重拷贝：芯片层暂存帧 ×3（3.5 MB）→ V4L2 缓冲 ×3（3.5 MB）→ 预览双缓冲 → 弹窗副本；预览时约 7.9 MB，内存带宽也翻倍 | 已证实 |
| M3 | 字体常驻 7.6 MB：完整 MiSans 被整个读入 PSRAM | 已证实 |
| M4 | SMP 主配置的网络 IOB 是默认值 36×196 B，比单核配置的 256×256 B 少很多（问题分析 P1-4） | 已证实 |
| M5 | 所有对象都在 PSRAM，包括 LVGL 的小对象和绘制层缓冲；没有利用内部 SRAM 低延迟的优势 | 已证实（`malloc` 就是 PSRAM） |
| M6 | 高频小块分配：每个 agent 事件 `calloc` 一个 pending 节点，每个 UI 事件拷贝一次 | 已证实 |
| M7 | 没有运行期内存观测：缺少堆高水位和栈高水位的周期输出 | 已证实 |

## 3. 优化方案

### 3.1 卡顿

#### O-J1 拆锁：UI 线程不再等待 Agent（对应 J1、T2）

1. 工具访问策略从 `agent_mutex` 拆出来，改用独立的 `tool_access_mutex`。策略回调只读这把锁，持有时间是微秒级。
2. 其余在 UI 线程调用、又需要 `agent_mutex` 的接口统一改为两种方式之一：
   - 在 LVGL 线程使用 `pthread_mutex_trylock`，失败就提示"对话进行中，稍后再试"，同时把控件状态还原；
   - 或者投递到已有的 V9 model worker 执行，结果通过 pending 队列回到 UI。
   涉及的接口：`apply_model_config`、`enumerate_tools`、`visit_history`。
3. 启动时的 `chat_restore` 不受影响，因为那时还没有对话。

**验收**：在模型请求进行中连续切换工具开关、切换设置页，UI 帧间隔不超过 50 ms。

#### O-J2 帧缓冲 cache 只清一次、只清脏区（对应 J2）

1. `esp32p4_fb_updatearea()` 按 `area` 计算字节范围，起止各按 64 B 对齐，只 clean 脏区行。脏区通常是几十行，整屏刷新时才是 1.2 MB。
2. `esp32p4_fb_pandisplay()` 不再 clean。前提是 `FB_UPDATE` 已启用，并且 LVGL 在 pan 之前一定会先调用 `FBIO_UPDATE`（`lv_nuttx_fbdev.c` 正是这个顺序）。为了安全，可以加一个"本页自上次 pan 以来是否已经 update 过"的标志，没有 update 过才兜底 clean 整页。
3. LVGL DIRECT 双缓冲的 `refr_sync_areas` 会把脏区拷贝到另一页，同样要覆盖到 cache 维护里。这一点要在实现时核对，避免另一页出现残影。

**验收**：只改一个 label 的帧，`flush` 耗时降低一个数量级。用 `perf_count` 或 GPIO 翻转测量。

#### O-J3 打开 LVGL 图片缓存（对应 J3）

1. 设置 `CONFIG_LV_CACHE_DEF_SIZE` 约为 512 KB（PSRAM），`CONFIG_LV_IMAGE_HEADER_CACHE_DEF_CNT=32`。
   - 卡片图共 13 张，每张约 98×138 RGBA，解码后约 54 KB，全部常驻约 0.7 MB；
   - 也可以只缓存当前可见的 8 张，约 432 KB。
2. 更彻底的做法：把 `res/devices/*.png` 也像图标一样，在构建时转换成 `lv_image_dsc_t`（RGB565A8 C 数组），彻底去掉运行时的文件读取和 PNG 解码。
3. 状态切换的时候不要 `lv_image_set_src` 一个新路径：on/off 两张图预先缓存好，或者合并成一张图做两态切换。

**验收**：开关设备时卡片图不闪烁；`lodepng` 解码次数从"每帧"降到"首次加载一次"，用 `LV_PROFILER` 或计数器确认。

#### O-J4 扩大字形缓存，并预热常用字（对应 J4、J9）

1. 用补丁 `patches/apps/` 修改 `lv_tiny_ttf.c`：把缓存从写死的 128 改为读取 `cache_size` 参数，或者新增一个 Kconfig 常量，取值 1024。每个字形位图约 0.5–2 KB，合计约 1–2 MB PSRAM，可以接受。
2. 启动后在空闲时预热：用 `res/fonts/chars.txt` 中的常用字，对 3 种主字号各渲染一遍，只填充缓存，不显示。
3. 中期：界面固定文案用子集字体，对话文本用完整字体的文件模式（见 O-M3，同时解决 M3）。

**验收**：首次进入聊天、设置页不超过 150 ms；滚动 50 条中文消息时帧间隔的 p95 不超过 33 ms。

#### O-J5 摄像头链路减拷贝，交给 PPA（对应 J5、M2）

分三步：

1. **零成本**：弹窗不再自己保留一份副本，直接以预览缓冲作为 `lv_image` 的数据源。camera service 提供"借用最新一帧"的接口，用 sequence 号加读锁保证 LVGL 绘制期间不被覆盖，并且只在 sequence 变化时 invalidate。
2. **减一层**：芯片层暂存帧已经经过 V4L2 的 memcpy，可以评估改成 `V4L2_MEMORY_USERPTR`，让 DMA 直接写入应用的缓冲，省掉 HPWORK 中的 1.2 MB memcpy（每秒 37 MB）。这需要芯片层支持把外部缓冲用作 GDMA 目标，工作量较大，作为 P2。
3. **卸载缩放**：PPA SRM 把 1024×600 直接缩放到 512×300（见 PPA 规格，ADR-0003），CPU 不再逐像素缩放；还可以直接缩放进 LVGL 的图片缓冲。

**验收**：打开预览后，首页其他区域帧间隔 p95 不超过 40 ms，预览不低于 15 fps。

#### O-J6 场景执行和定时刷新移出 UI 热路径（对应 J7、J8）

1. `v9_scene_run` 改为投递给 miloco worker 执行。worker 已经拥有快照，不需要再复制。UI 只显示"执行中"，结果通过 pending 队列返回，并按条显示成功或失败数。
2. 1 s 轮询改为 revision 门控：先读取 `device_service_revision()` 和 `miloco_list(NULL, 0, &rev)`，这两个调用只返回 revision，不复制数据；revision 没变就直接返回，变了才复制快照。米家快照复制改用服务持有的缓冲（见 O-M1）。
3. 隐藏页面的定时器（旧 network 页 250 ms、旧 panel 的 node 页 500 ms）在 `load_tab` 切换时暂停或恢复。

#### O-J7 渲染参数（在 O-J2、O-J3 完成后做 A/B 实验）

| 参数 | 现值 | 候选 | 说明 |
| --- | --- | --- | --- |
| `LV_DEF_REFR_PERIOD` | 33 | 16（只在转场和动画期间） | 交互期间动态调整，空闲时恢复 33 |
| `LV_DRAW_SW_DRAW_UNIT_CNT` | 1 | 2 | 需要 `LV_USE_OS=NUTTX`，第 2 个 draw unit 绑到 CPU1；属于大改动，先 A/B 测试 |
| `LV_DRAW_SW_SHADOW_CACHE_SIZE` | 0 | 16 | 下拉菜单有 18 px 阴影 |
| 旧页面转场 | MOVE_LEFT 180 ms | 瞬时或 FADE | 旧页面仍有入口时才需要调整 |
| GT911 | 20 ms 轮询 | 中断模式（`1bb9eb1` 已支持） | 输入延迟降低约 10 ms，同时减少 LPWORK 占用 |

### 3.2 线程

#### O-T1 双核分工（对应 T1、J6）

| CPU0（实时 UI 和显示链路） | CPU1（后台计算和网络） |
| --- | --- |
| `smart_home` 主线程（LVGL） | agent worker（JSON、TLS、模型 I/O） |
| camera worker（与 DSI 同核做 cache 维护） | miloco worker（已绑定） |
| HPWORK（I2S、CSI 帧 worker） | voice play worker（TTS 请求和解码） |
| LPWORK（GT911、ESP-Hosted 收包） | ASR worker、KWS worker |
| | V9 model/net worker、房间保存 worker |

实现方式：在 `smart_home_memory.h` 旁边新增一个 `smart_home_thread.h`，提供下面的接口，统一处理栈分配、对齐、优先级、亲和性、名称和 detached 属性，替换现在 6 处各自写的 `pthread_attr` 代码：

```c
smart_home_thread_start(name, entry, arg, stack_bytes, priority, cpu_mask, detached)
```

**风险**：`ESP32P4_SMP_PSRAM` 和 `ESP32P4_SMP_SPIFLASH` 都是实验性选项。把 Flash 写入（房间保存、session 落盘）放到 CPU1 之前，先在 `smp_fs` 上做并发压测（问题分析 P2-5）。

#### O-T2 优先级分层（对应 T5）

| 层 | 优先级 | 成员 |
| --- | ---: | --- |
| 硬实时 | 224 | HPWORK（I2S/CSI 完成） |
| 采集 | 110 | voice capture |
| 交互 | 105 | LVGL 主线程（从 100 调到 105） |
| 内核延迟工作 | 100 | LPWORK（触摸、收包） |
| 业务后台 | 90 | agent、voice play、KWS、ASR |
| 低频 | 80 | miloco、保存、设置 worker |

`RR_INTERVAL` 从 200 ms 改为 20 ms，避免同优先级线程长时间占住 CPU。验证时要注意，TLS 握手期间 UI 不能被饿死。

#### O-T3 长驻 worker 替代每轮创建（对应 T3、T4）

- agent worker 改为常驻：用一个深度为 1 的输入队列加条件变量，处理完一轮回到等待状态。省掉每轮的 `pthread_create/join`，退出时序也统一到 `smart_home_lvgl_deinit`。
- 按需线程统一登记：维护一张表，记录名称、状态和句柄，退出时按"停止 → 唤醒 → join（带超时）"的固定顺序回收；detached 线程改为可 join，退出时等它结束。

#### O-T4 音频阻塞路径加超时（对应 T6）

- `es8311.c` 中的等待改为 `nxsem_tickwait`，超时后强制停止 DMA，再 join；STOP 路径设总超时 500 ms。
- 同时修复问题分析 P1-2（I2S 在锁内分配内存）和 P1-3（START 没有下发），三项一起上板验证。

### 3.3 内存

#### O-M1 消除栈上大数组（对应 M1）

- miloco service 内部持有一份 `snapshot[16]`，由 `lock_state` 保护。对外提供 `smart_home_miloco_visit(fn, ctx)`，在锁内回调，回调中只能做拷贝少量字段这类轻量操作；需要整份数据的地方，传入调用方持有的缓冲。
- UI 侧的 `collect_views`、`append_miloco_cards`、`miloco_sheet_build` 等，共用一份 `g_v9` 持有的静态快照缓冲（LVGL 线程独占，不需要加锁）。
- `poll_device_list` 的 `fresh` 和 `snapshot` 改为 worker 启动时一次分配的成员。
- 完成后：主线程最坏栈约 20 KB，miloco worker 约 4 KB。miloco 栈可以从 48 KB 降回 16 KB，主线程栈可以从 64 KB 降到 32 KB。

**防回归**：新增 `scripts/stack_depth.py`，用 objdump 加调用图，输出每个线程入口的最坏栈深，超过栈大小的 70% 就失败。放进提交前检查。

#### O-M2 内存分区：热数据放内部 SRAM，大块放 PSRAM（对应 M5）

| 放内部 SRAM（Kmem，通过 `kmm_malloc` 或静态段） | 留在 PSRAM（Umem） |
| --- | --- |
| LVGL 绘制层缓冲 `LV_DRAW_LAYER_SIMPLE_BUF_SIZE`（24 KB） | 帧缓冲、摄像头缓冲、字体 |
| LVGL 对象内存池（可选：`LV_USE_STDLIB_MALLOC=BUILTIN`，`LV_MEM_SIZE` 设 256 KB 放内部 SRAM） | 模型请求和响应缓冲、Session |
| pending 队列节点池（O-M4） | 米家响应缓冲、图片缓存 |
| 高频小对象（agent 事件拷贝） | 线程栈（除 LVGL 主线程以外） |

内部 SRAM 的内核堆实测约 420 KB 可用，扣除 netdev、IOB 和驱动对象后，预计还有 200 KB 以上可以分配。具体数值以 `free` 实测为准，按"先测再放"的原则进行。如果 LVGL 对象池放进内部 SRAM，对象遍历和样式查找会明显变快。这项改动影响面最大，先 A/B 测试。

#### O-M3 字体按需加载（对应 M3、J9）

现有子集字体 `MiSans-Normal-subset.ttf` 只有 72 KB，覆盖 `chars.txt` 中约 400 个字，只够界面固定文案使用。

| 方案 | 常驻 | 代价 |
| --- | --- | --- |
| 现状：整个文件读入 PSRAM | 7.6 MB | 启动时读 7.9 MB |
| A：只用子集字体 | 72 KB | 模型回复和用户输入中超出 400 字的汉字显示为方框，**对话页不可接受** |
| B：子集字体常驻负责界面文案，完整字体用 TinyTTF 文件模式（`lv_tiny_ttf_create_file`）负责对话文本 | 72 KB 加文件读缓存 | 对话中首次出现的字要从 Flash 读，靠 O-J4 的字形缓存摊薄 |
| C：保持现状，靠 O-J4 扩大缓存 | 7.6 MB | 没有内存收益 |

推荐 **B**：字体常驻内存从 7.6 MB 降到 1 MB 以下，启动时少读约 7.8 MB。需要实测文件模式下对话页的首屏渲染耗时，如果超过 O-J4 的验收值，就扩大子集（例如覆盖 GB2312 一级字库，约 1 MB），以此在内存和性能之间折中。

#### O-M4 pending 队列改为固定节点池（对应 M6）

- 预分配 32 个 `ui_pending_item_t`，放在内部 SRAM；队列满时丢弃最旧的 `TOOL_*` 中间事件，保留 `TURN_END` 和 `DONE`。
- 不再在回调路径上 `calloc` 和 `free`，也不会产生碎片。

#### O-M5 网络缓冲按配置统一（对应 M4）

把 `IOB_NBUFFERS=256`、`IOB_BUFSIZE=256`、`IOB_NCHAINS=32` 同步到 `smp_smart_home`。这部分来自内核堆，约 64 KB，要和 O-M2 一起做内部 SRAM 预算。

#### O-M6 运行期观测（对应 M7）

- 打开 `CONFIG_SMART_HOME_V9_PERF_LOG`，扩展现有的 `[v9-perf]` 行：在 Umem 之外加上 Kmem 的使用量和最大块，再加上 5 个主要线程的栈高水位（`STACK_COLORATION` 已经开启，可以通过 `nxsched_get_stackinfo` 或 `/proc/<pid>/stack` 读取）。
- 打开 `LV_USE_SYSMON`（只在调试配置中），屏幕角落显示 FPS 和 CPU 占用。

## 4. 实施计划

| 阶段 | 内容 | 预期收益 | 风险 |
| --- | --- | --- | --- |
| **S0 测量**（先做） | O-M6；烧录带 `[v9-perf]` 的固件，按 V9 实施方案的 P01–P12 场景采集基线 | 有了基线才能判断后续收益 | 无 |
| **S1 低风险、高收益** | O-J1 拆锁；O-J3 图片缓存；O-J4 字形缓存；O-M5 IOB；O-J6 revision 门控和场景执行移出 UI | 消除硬卡，首页和聊天流畅度明显改善 | 低：都是局部改动 |
| **S2 显示链路** | O-J2 脏区 cache；O-T2 优先级；GT911 中断模式 | 降低每帧固定开销和输入延迟 | 中：O-J2 可能出现残影，需要逐场景肉眼核对 |
| **S3 线程与栈** | O-T1 绑核；O-T3 常驻 worker；O-M1 去掉栈上大数组和栈深检查；O-T4 音频超时 | 用上第二个核，栈风险可控 | 中：SMP 下 Flash 写入并发需要压测 |
| **S4 结构性优化** | O-M3 字体 B 方案；O-M4 节点池；O-J5 摄像头减拷贝和 PPA；O-M2 内部 SRAM 分区；O-J7 双 draw unit | 字体常驻内存减少约 7 MB，摄像头开启时不再明显掉帧 | 高：需要单独立项，逐项 A/B 测试 |

## 5. 验收指标

| 指标 | 场景 | 目标 |
| --- | --- | --- |
| 帧间隔 p95 | 首页空闲 / 切房间 / 开关设备 | ≤ 33 ms |
| 最坏帧 | 同上 | ≤ 66 ms |
| 对话中的 UI 帧间隔 | 模型请求进行中操作设置页 | ≤ 50 ms（不能冻结） |
| 首次进入聊天、设置 | 冷启动后第一次进入 | ≤ 150 ms |
| 摄像头预览 | 弹窗打开 | 预览 ≥ 15 fps，其余 UI p95 ≤ 40 ms |
| 触摸到反馈 | 点开关 | ≤ 100 ms |
| 启动到首页 | 冷启动 | 先测基线，S4 完成后再降低 30% |
| 栈余量 | 所有线程 | 静态最坏深度 ≤ 70%，实测高水位 ≤ 60% |
| 内存 | 1 小时待机加 50 轮对话 | Umem、Kmem 的使用量和最大块都没有单调下降 |

每项都要在同一份固件、同一套脚本下测三次，波动小于 10%。数据记录在 `docs/开发日志/` 中，每一阶段前后各测一次。
