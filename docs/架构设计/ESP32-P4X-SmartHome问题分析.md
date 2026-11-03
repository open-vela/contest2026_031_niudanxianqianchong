# ESP32-P4X SmartHome 问题分析

> 代码基线：`contest2026` @ `491ffa9`（2026-11-01），外加工作区 1 处未提交改动。
> 当前构建产物：`nuttx/nuttx`（`smp_smart_home`，乐鑫工具链）。
> 范围：工程与可复现性、驱动、平台（SMP/内存/网络）、应用架构、安全、质量保障。
> 方法：静态审查源码、配置和构建产物，并在宿主侧复现补丁；**未上板**。
> 每条问题都标了证据等级：
> - **已证实**：代码、配置或构建产物可以直接看到；
> - **推断**：根据代码路径推导，尚未复现；
> - **待验证**：需要上板确认。
>
> 关联文档：[SmartHome 整体架构](ESP32-P4X-SmartHome整体架构.md)、
> [驱动缺口与 BUG 清单](../开发计划/驱动缺口与BUG清单.md)。

## 1. 结论摘要

| 级别 | 数量 | 最需要先处理的 |
| --- | ---: | --- |
| P0（阻塞交付或复现） | 4 | 公共工作区补丁无法重放；manifest 漏链应用包；144 个提交未推送；WiFi 凭据泄露在已提交的日志里 |
| P1（运行期故障风险） | 8 | UI 线程会被 `agent_mutex` 卡住整轮对话；I2S 在自旋锁内分配内存；ES8311 录音从未下发 START；SMP 主配置的网络缓冲退回默认值；双核版 `smart_home` 尚未构建验证 |
| P2（性能、健壮性、可维护性） | 11 | 栈上大数组、第二核闲置、场景执行阻塞 UI、启动时每次重写 secrets、遗留 UI 代码仍在编译 |
| P3（安全与产品化） | 4 | 凭据明文存储、米家网关走明文 HTTP、日志打印 SSID、Kconfig 里写着默认 WiFi |

自 10-25 的缺陷清单以来，下列问题**已修复**：

| 问题 | 修复提交 |
| --- | --- |
| B-01 音频缓冲 8192 越界 | `aff9b2c` |
| B-05 部分入队泄漏、TX carry 越界、APB 引用泄漏 | `5240257` |
| RX 完成判定被 EOF 位阻断 | `48d9556` |
| ROM 浮点函数触发非法指令 | `74cfac4` |
| miloco worker 栈不足 | `e4d1edb` |
| `usrsock.h` 编译错误 | `a5410ac` |

## 2. P0：阻塞交付或复现

### P0-1 公共工作区补丁无法重放（已证实）

- **现象**：
  - 在干净的 `nuttx/drivers/audio/es8311.c` 上按顺序应用 `patches/nuttx/0001–0009`，其中 0003、0006、0007、0008、0009 失败。工作树里的 `es8311.c` 还比这组补丁多出一批 `syslog(LOG_DEBUG, …)`。
  - `drivers/nuttx/patches/0001-v4l2-number-completed-frames.patch` 在当前的 `v4l2_cap.c` 上正向、反向都应用不了。
  - `nuttx/` 下有 26 个修改或新增文件，没有对应的补丁管理。
- **影响**：重新执行 `repo sync` 后，音频驱动和 V4L2 帧序号修复无法恢复，评审方也无法复现当前固件。
- **建议**：以当前工作树为准重新生成 ES8311 和 V4L2 补丁。在 `scripts/` 里加一个 `verify_patches.sh`：在干净副本上重放全部补丁并和工作树比较。把它列为提交前必须通过的检查。

### P0-2 manifest 没有链接应用包（已证实）

- `contest2026_031_niudanxianqianchong.xml` 中 `smart_home`、`cAGENT` 的 linkfile 被注释掉了。`cagent_addons`、`gt911_probe`、`video_test`、`smp_*` 等 8 个包根本没有登记。
- 本机能编译，是因为 `packages/` 下有手工建的软链接。
- **影响**：按 README 执行 `repo sync` 后构建 `smart_home` 会失败。另外，`smart_home/defconfig` 直到 `491ffa9` 之前，`ARCH_BOARD_CUSTOM_DIR` 一直指向临时 worktree `../worktrees/audio-fix-codex/`，说明配置和路径的漂移没有被发现。
- **建议**：补全 linkfile；构建脚本在开始前检查所有 `CONFIG_ARCH_*_CUSTOM_DIR` 和包链接都指向比赛仓。

### P0-3 大量提交没有推送（已证实）

- 本地 `contest2026` 比远端 `dev-ai-contest-2026` 多 **144** 个提交，SMP、V9 UI、工具链、I2S 修复等都只在本机。
- **建议**：先完成 P0-4 的日志脱敏，再推送；推送前跑一遍 P0-1 的补丁重放检查。

### P0-4 WiFi 凭据泄露在已提交的日志里（已证实）

- HEAD 中 `logs/` 下有 4 个 jsonl 文件包含明文 SSID 和密码。
- **建议**：推送前脱敏（同时重写相关历史提交），或者直接更换该 WiFi 的密码。

## 3. P1：运行期故障风险

### P1-1 UI 线程会被 `agent_mutex` 卡住整轮对话（已证实代码路径，现象待验证）

- `smart_home_agent_run()` 在整个 `agent_run` 期间都持有 `app->agent_mutex`，包括 TLS 往返，最长到模型超时。
- 以下几个接口同样要拿这把锁，而且都在 LVGL 线程上被直接调用：

  | 调用位置 | 接口 |
  | --- | --- |
  | `smart_home_lvgl_settings_v9.c:837` 工具开关回调 | `set_local_tool_access` |
  | `smart_home_lvgl_settings.c:483` `apply_settings_event` | `apply_model_config` |
  | `smart_home_lvgl_settings.c:591` `local_tool_switch_event` | `set_local_tool_access` |
  | `smart_home_lvgl_settings.c:732` `refresh_tool_directory`（切页时也会调用） | `enumerate_tools` |
  | `smart_home_lvgl_history.c:78` | `visit_history` |

- **影响**：对话进行中拨动工具开关或切到相关页面，整屏会冻结，直到本轮对话结束。
- **建议**：工具访问策略换用独立的小锁，不再和 Agent 内核共用一把锁。其余调用改到 worker 上执行，或者改为 `trylock`，失败时提示“对话进行中”。

### P1-2 I2S 在自旋锁内分配内存（B-03，已证实）

- `esp_i2s.c` 的 `i2s_buf_allocate()` 在 `spin_lock_irqsave(&priv->slock)` 持锁期间，循环调用 `kmm_memalign()`。`5240257` 补上了失败回滚，但分配仍然在锁内。
- **影响**：关中断期间进入堆分配器。堆分配器内部用的是 mutex，可能睡眠；SMP 下还会拉长另一个核的自旋等待时间，属于死锁风险。
- **建议**：描述符在初始化时为每个 container 一次性分配好；运行期只从空闲链表里取。

### P1-3 ES8311 录音从未下发 START（B-02，已证实代码路径）

- `priv->streaming` 只会在 `esp_i2s.c` 的 `AUDIOIOC_START` 分支里置为 true，而 `es8311.c` 从不调用 `I2S_IOCTL(START/STOP)`。
- 按 10-25 清单的分析，RX 的每个包都会被标记为 `AUDIO_APB_FINAL`，录音大约 4 KB 后就会“完成”。`48d9556` 修的是 EOF 判定，没有修这里。
- **建议**：在 `es8311_start/stop` 中补上 I2S 的 START/STOP。上板用 `record 10` 验证输出字节数不少于 320000。

### P1-4 SMP 主配置的网络缓冲退回了默认值（已证实）

- `smart_home/defconfig` 配置的是 `IOB_NBUFFERS=256`、`IOB_BUFSIZE=256`。这是 09-20 米家链路排障时，为 17 KB spec 突发流量专门调大的（见米家链路七层排障闭环）。
- `smp_smart_home/defconfig` 没有写这几项，当前镜像实际是默认的 `36 × 196 B`，池深只有原来的 1/9。
- **影响**：在双核主配置上，拉取摄像机 spec、TLS 大响应时可能重新出现丢包和超时。
- **建议**：把网络缓冲配置同步到 `smp_smart_home`；后续合并成一份产品配置（见 P2-9）。

### P1-5 双核版 `smart_home` 还没构建验证（已证实）

- `491ffa9` 把 `smart_home` 切换到了 SMP。提交说明写明“隔离全量构建因第三方依赖重复补丁未完成”，“SMP 音频关闭路径的已知阻塞风险仍需单独修复”。
- 工作区里还有一处未提交改动：删除了 `CONFIG_SCHED_WAITPID`。
- **影响**：当前主线上的主配置没有一次完整构建记录，更没有板端验证；音频在双核下的关闭路径存在已知风险，但没有文档描述是什么风险。
- **建议**：完成一次全量构建，并把这个风险写成具体条目；在修复前，`smart_home` 的 SMP 版本不作为演示固件。

### P1-6 DMA 丢一次 EOF 就会永久挂死（B-04，已证实代码路径）

- `es8311.c` 中有 3 处 `pthread_join(priv->threadid)`，I2S 等待用的是不可中断的信号量。只要 DMA 丢一次完成中断，worker 就永远不会退出，调用 STOP 的线程也会跟着被卡住。
- 这和 P1-5 提到的“SMP 音频关闭路径阻塞”很可能是同一个问题（推断）。
- **建议**：等待改为带超时；STOP 路径先强制停止 DMA，再 join。

### P1-7 Agent 控制米家时把“已入队”当成成功（已证实）

- `miot_device_control` 工具返回的是 `{"ok":true,"note":"control submitted; state updates on next poll"}`，真实结果要等最多 5 s 后的下一次轮询。
- **影响**：控制失败、设备离线或队列满（深度 8）时，模型仍然可能回复“已完成”。
- **建议**：工具内用 `submit_control_tracked` 加上 2 s 以内的 `control_result` 等待，把真实结果交给模型；超时就明确返回“未确认”。

### P1-8 首页场景按钮在 UI 线程同步执行（已证实）

- `8afde66` 新增的 `v9_scene_run()` 在 LVGL 回调里直接调用 `smart_home_miloco_run_scene()`。这个函数在栈上放了一个 `smart_home_miloco_device_t[16]` 快照（约 19 KB），并逐台调用 `submit_control`。
- 虽然 `submit_control` 只是入队、不做网络 I/O，但这条路径会把主线程栈的最坏深度从约 41 KB 再推高（推断）。而且场景动作超过 8 个时，后面的动作会因队列满被拒绝，界面上却提示“已提交”。
- **建议**：快照改为堆缓冲；提交结果按条统计，返回给界面显示。

## 4. P2：性能、健壮性、可维护性

| ID | 问题 | 证据 | 建议 |
| --- | --- | --- | --- |
| P2-1 | 栈上大数组：12 处把 `smart_home_miloco_device_t[16]`（每个 1188 B）放在栈上。静态最坏深度：主线程约 41 KB / 64 KB，miloco worker 约 40 KB / 48 KB | 已证实（反汇编静态分析） | 服务持有一份快照缓冲并复用；CI 加入基于反汇编的栈深检查 |
| P2-2 | 第二个核基本闲置：`SMP_DEFAULT_CPUSET=0x1`，只有 miloco worker 绑到了 CPU1；agent、TTS、ASR worker 都和 LVGL、摄像头挤在 CPU0 | 已证实 | agent、voice、ASR worker 显式绑到 CPU1 |
| P2-3 | PSRAM 带宽紧张：DSI 扫描约 74 MB/s，摄像头采集加拷贝约 75 MB/s，`FBIO_UPDATE` 每次按整页 clean 1.2 MB（B-12） | 推断 | 摄像头缩放交给 PPA；DSI 按脏矩形做 cache clean |
| P2-4 | 每次开机都重写 `secrets.json`：拿到 DHCP 后无条件调用 `set_wifi_credentials`，内容不变也写 | 已证实 | 与现有内容比较，相同则跳过。项目曾出现“flash 写挂死”，`MILOCO_VOLATILE_CONFIG` 就是为此加的临时规避，少写一次就少一次风险 |
| P2-5 | Flash 写入与双核并发：`ESP32P4_SMP_SPIFLASH` 标注为实验性；“摄像头与文件系统同时高负载”在 SMP 四项接入记录中列为未验证 | 待验证 | 先在 `smp_fs` 上做摄像头、写文件、UI 三路并发压测，再在主配置上启用后台写入 |
| P2-6 | 板级初始化失败不汇总：`esp_bringup` 每一步失败只打日志 | 已证实 | 输出 `ready_mask`，应用启动时打印，并在系统健康页显示 |
| P2-7 | 遗留 UI 代码仍在编译：`smart_home_lvgl_settings.c`（2211 行）、`panel.c`（2773 行）、`home.c`、`pages.c` 和 V9 版本并存，P1-1 的大部分锁调用点就在旧设置页 | 已证实 | 确认旧页面已无入口后删除，或者放到 `LEGACY_UI` 开关下 |
| P2-8 | UI 状态靠 1 s 拉取：设备、米家状态每秒复制一次快照（米家约 19 KB） | 已证实 | 改为事件推送入 pending 队列，保留低频轮询作兜底 |
| P2-9 | 配置矩阵分裂：`smart_home`（SMP、音频、xPack 工具链、大 IOB）和 `smp_smart_home`（SMP、显式 GT911、乐鑫工具链、默认 IOB）各缺一部分；`smart_home` 的 GT911 依赖 `gt911_probe` 的 `select` 间接打开，不够稳 | 已证实 | 合并成一份产品配置，再派生 `_voice`、`_local` |
| P2-10 | I2S 理论空指针（B-10）：`esp_i2s.c:1355` 的 `while (bfdesc->eof == 0 && bfdesc != NULL)` 先解引用后判空 | 已证实 | 调换判断顺序 |
| P2-11 | 错误码合并（B-08）：`es8311.c` 里有 `ret \|= writereg()`，负的 errno 按位或之后失去原值 | 已证实 | 遇到首个错误立即返回 |

另外有 3 项低优先级的板级问题仍未处理，与 10-25 清单一致：

- B-06：ES8311 注册失败时没有回滚；
- B-09：`esp_i2s.c:3764` 创建 PM lock 后句柄没有写回，当前 `CONFIG_PM` 未开，不会触发；
- B-14：以太网没有 PHY 复位和链路等待。

## 5. P3：安全与产品化

| ID | 问题 | 证据 | 建议 |
| --- | --- | --- | --- |
| P3-1 | `/data/smart_home/secrets.json` 明文保存 WiFi 密码、模型 API Key、米家 token | 已证实 | 产品化时用 Flash 加密或 eFuse 派生密钥；比赛阶段在文档中说明 |
| P3-2 | 米家网关走明文 HTTP，Bearer token 在局域网内可以被嗅探 | 已证实 | 网关改用 HTTPS，或在请求上签名 |
| P3-3 | `smart_home_wifi.c:134/218` 以 INFO 级别打印 SSID | 已证实 | 降为 DEBUG，或者打码 |
| P3-4 | Kconfig `SMART_HOME_WIFI_SSID/PASSWORD` 默认值是 `"123"` / `"88888888"`，会被编进固件 | 已证实 | 默认值改为空，强制走运行时配网 |

已确认做得好的地方：

- 数据镜像默认不打包 `secrets.json`（需要 `WITH_SECRETS=1` 才会打包）；
- 本地凭据配置 `*_local/` 已被 gitignore；
- 模型 Key 用完后会 `secure_clear`；
- App Bridge 有 shared token 鉴权。

## 6. 质量保障缺口

| 项 | 现状 | 缺口 |
| --- | --- | --- |
| 宿主测试 | `demos/smart_home/tests` 下有 17 个 C 测试、8 个 LVGL 控件测试；`scripts/test_*.py` 9 个；agent 测试启用了 ASan/UBSan | 没有 CI，只靠手工运行。`.github/workflows` 里只有 CLA 检查 |
| 构建验证 | 各提交说明里记录了单个配置构建通过 | 没有矩阵构建，配置漂移（P0-2、P1-4、P2-9）要靠人工发现 |
| 板端验证 | 大量提交注明“未烧录/未做板端验证”：I2S 两处修复、音频缓冲、SMP 主配置、房间管理、设备弹窗、摄像头弹窗 | 缺一份板端回归清单，每次合入后按清单执行并记录 |
| 静态检查 | 无 | 加入栈深分析（P2-1）、补丁重放（P0-1）、“镜像中无 ROM 浮点符号”检查（防止 `74cfac4` 那类问题再出现） |

## 7. 建议处理顺序

1. **当天**：P0-4 脱敏，P0-2 补全 manifest，P0-1 重新生成补丁并加上重放检查，然后推送（P0-3）。
2. **本周（运行期正确性）**：P1-4 同步 IOB 配置 → P1-1 拆分锁 → P1-3、P1-6 音频 START 和超时 → P1-2 描述符预分配 → P1-5 完成 SMP 全量构建并上板。
3. **下一阶段**：P2-9 合并配置 → P2-1、P1-8 消除栈上大数组 → P2-2 绑核 → P1-7 Agent 控制结果确认 → P2-7 清理遗留 UI。
4. **产品化前**：第 5 节全部条目，以及第 6 节的 CI。

## 8. 复核方法

| 问题 | 复核命令或方法 |
| --- | --- |
| P0-1 | 在临时目录用 `git -C nuttx show HEAD:<file>` 取原始文件，按序号依次 `git apply`，再和工作树 `cmp` |
| P0-3 | `git rev-list --count contest2026_031_niudanxianqianchong/dev-ai-contest-2026..contest2026` |
| P0-4 | `git grep -l "<密码>" HEAD -- logs` |
| P1-1 | `grep -n "smart_home_agent_app_\(set_local_tool_access\|apply_model_config\|enumerate_tools\|visit_history\)" demos/smart_home/src/ui/lvgl/*.c` |
| P1-4 | `grep -E "^CONFIG_IOB_(NBUFFERS\|BUFSIZE)=" nuttx/.config` |
| P2-1 | 对镜像 `objdump -d`，统计每个函数序言里 `addi sp,sp,-N` 与 `lui t0` 的组合，再沿调用图累加 |
