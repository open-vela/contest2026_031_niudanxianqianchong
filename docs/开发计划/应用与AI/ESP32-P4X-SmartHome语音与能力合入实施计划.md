# ESP32-P4X SmartHome 语音与能力合入实施计划

> 版本：v0.4（2026-11-03）
> 主线审查基线：`contest2026 @ 252812c`；本轮 P0 运行基线：`ceb1c48`
> 执行工作区：`/home/arongw/openvela/worktrees/voice-gates-codex`
> 总体状态：P0 通过；P1 camera 工具已合主线（`e04efab`、`0f64854`），20 张新帧
> 与 agent"拍照"仍未验收；P2 VAD 已开；P3 KWS 生命周期修复已合主线
> （`059c5bc`），板端只跑通单次推理，P95 未取；P4 ESP-NN 已接 P4 内核并配 mask，
> 覆盖 58.3% MAC，实测 3.68 s → 2.38 s；P5 云端 ASR/TTS 短句通过、长句超时未解决；
> P6 SMP 音频基础通过，KWS/ESP-NN 组合未验。
> 范围：本地音频、camera 工具、VAD、KWS、ESP-NN、云端 ASR/TTS、最终 SMP 组合。

关联文档：[本轮验收记录](../../开发日志/ESP32-P4X-语音能力分阶段验收记录_20261102.md)、[问题分析](../../架构设计/ESP32-P4X-SmartHome问题分析.md)、[线程内存方案](../../架构设计/ESP32-P4X-SmartHome卡顿线程内存优化方案.md)、[SMP 音频修复方案](../ESP32-P4X-SMP音频链路排查与修复方案_20261101.md)。

## 1. 目标、完成定义与执行原则

最终交付是一组可以从明确源码、配置和工具链重建的镜像，以及对应板端证据：
麦克风/扬声器可用，相机取得新帧并落盘，VAD 能可靠断句，KWS 前端与模型匹配，
ESP-NN 确实执行并产生可测收益，云端 ASR/TTS 可交互，双核组合不冻结。

每阶段分别记录源码集成、宿主测试、目标构建、板端运行和用户物理验收。
构建成功不代替板测；文件大小正确不代替新帧；推理完成不代替唤醒率；
烧录校验只证明指定文件写入正确。

执行约束：

1. 默认按 P0→P1→P2→P3→P4→P5→P6 推进，一次改变一个主要变量。
2. 阶段不通过时先定位。P0 录放失败必须停止后续音频功能叠加。
3. 网络不可用时，P5 标记 `BLOCKED`，可继续不依赖网络的 P6；整体目标仍未完成。
4. 若正确的 reference 内核超过 20 ms，允许先做 P4 加速诊断，再回补 P3 性能验收；
   不因此放宽 20 ms，也不提前开启常驻 KWS。
5. 保留 `smart_home_voice` 为单核语音基线，KWS 和 ESP-NN 使用独立配置。
6. 不重复合入已进入主线的 camera/VAD 提交，不整支覆盖音频基线。
7. 使用专用 worktree，源码只在 worktree 修改；主检出只读、验证、fast-forward。
   用户已授权构建、烧录、操作板子及提交；无需逐次重复确认。

## 2. 当前状态与证据边界

### 2.1 阶段总表

| 阶段 | 已有实现和本轮结果 | 尚未完成的验收 | 状态 |
| --- | --- | --- | --- |
| P0 音频 | `d3374a3` 音频修复、`ee219f6` voice 缓冲已在主线；本轮 10 秒录音 320000 B，两条回放及固定语音均清楚，无周期咔哒 | 后续改动触及音频/缓存/调度时需回归 | **通过：本次 voice 镜像** |
| P1 camera | `f06d4ca`、`e04efab`、`0f64854` 已在主线；camera 工具注册成功（description 压到 139 B，原 262 B 触发 `AGENT_ERROR_LIMIT`）；`SMART_HOME_CAMERA_TOOL` 默认 n，由 defconfig 显式开启 | 自测任务退出后复用旧帧；20 张新帧、资源清理、agent"拍照"未通过 | **工具已合，功能未通过** |
| P2 VAD | `f0adbc2`、`ee219f6` 已在主线；宿主四场景通过；`record-vad` 子命令可用；`smart_home_voice` 已开 VAD | 受控 A/B 3/3、首尾字完整 | **部分验证** |
| P3 KWS | `kws_infer.cc` resolver 改静态 + placement new，`059c5bc` 已合主线；板端单次推理 3.68 s 输出正确，模型 `scale=0.17718 zp=-20` 与预期一致 | P95、常驻服务验收（`CONFIG_SMART_HOME_KWS` 未在任何 defconfig 开启） | **板端单次通过，P95 缺失** |
| P4 ESP-NN | `third_party/esp-nn/` 80 源文件、`tflm-espnn-adapter` 三内核、`link_esp_nn.sh` 软链覆盖；P4 mask 配 `0xa8000000`/`0x50000000`，覆盖 58.3% MAC；实测 2.38 s（2.54×） | 覆盖上限受 `IsEspNn...Selected()` 的 `tid < 32` 限制；平台吞吐仍是基准 5~20 倍差距 | **已生效，未达预期 4×** |
| P5 云端 | ASR/TTS 主线已有实现；短句"你好"板端听感通过；ASR/TTS 响应头容量、24→16 kHz 重采样已修 | 长句正文接收超时（HTTP 200 但 `AGENT_ERROR_TIMEOUT`）根因未定；DNS 现场不可用 | **部分通过，长句阻塞** |
| P6 SMP | 既有 `smart_home` 双核音频已获用户听感验收 | 本轮新增 KWS/ESP-NN 与双核音频组合、反复启动和资源检查 | **基础通过，组合待验** |

### 2.2 本轮重要发现

**相机 A：缓存停用路径的代码放置。** 原始单核 voice 首次拍照启动 CSI 后整机无响应。
JTAG 读到 `mepc=mtval=0x40002d1c`（`riscv_exception`）、指令访问异常，
栈中有 Flash 内的 `nxsched_suspend_scheduler` 与 `esp_cache_resume_ext_mem_cache`。
实际 HAL 文件为 `esp_cache_utils.*`，已有 `esp_cache.*` 链接选择器没有覆盖它。
两个 contest 链接脚本加入 cache utils 和 scheduler switchcontext 的 IRAM 选择器后，
拍照能够返回、`ps` 响应。候选镜像相关函数已在 `0x4ff4xxxx`。
该修复有板端正向证据，但仍需结合下面的 B 问题完成 P1 回归。

**相机 B：自测任务组生命周期。** 修复 A 后，连续 10 次独立 NSH 自测均写出
307200 B，`frame_seq` 却为 `1,396,396,396,396,396,396,396,396,396`。
拍照落盘 `seq` 增加不能证明摄像头继续采集。结合 `ps` 中采集 pthread 消失、
全局状态仍报告运行，定位方向是自测主任务返回时任务组清理了采集线程，
全局预览被后续调用继续使用。需修复启动/退出所有权，并用单次调用的批量新帧验收。
当前已有外部候选补丁，尚未应用和验证。

**KWS：先解决内存生命周期。** TFLM 保存 resolver 引用及注册项指针，原实现却把
resolver 放在初始化函数栈上。真实模型第一次宿主 Invoke 即触发 ASan
`stack-use-after-return`，表现为首次 Invoke 长时间不返回，曾被误判为算力不足。
smoke 还存在 arena 先释放、解释器未销毁的问题。
修复已在 `059c5bc` 合入主线：resolver 改静态对象 + placement new，明确 arena
所有者，先销毁解释器再释放 arena，并新增 `kws_infer_deinit/ready/backend`
与 `-EBUSY` 语义。板端单次推理输出与 `scale=0.17718 zp=-20` 契约一致；
P95 与常驻服务仍无板端数据。

**配置差异。** 最终 `.config` 表明当前 voice/KWS 测试均为单核、默认工具链；
voice 的 `CONFIG_BOARD_INITTHREAD_STACKSIZE=2048`，既有 SMP `smart_home` 为 4096。
不能把主线某一配置的设置推导为所有配置相同。后续统一栈值要有明确配置差异记录，
与 SMP/ESP-NN 开关分别验证。

### 2.3 已保存的镜像

证据根目录：`/home/arongw/openvela/out/voice-gates-20261102/`。

| 文件 | SHA-256 | 当前证明范围 |
| --- | --- | --- |
| `voice.bin` | `1e827ab1537be37773d2fa3a0e4e91f1789a55b853db916e11838d4da041b37d` | P0 用户听感通过，1754260 B |
| `voice-cachefix.bin` | `c8067a66777515f227d77f1cc11a1ad5db757811e65a64ed9cabd5955132f158` | IRAM 候选已烧录并测试；仍有自测旧帧问题 |
| `kws-reference-cachefix.bin` | `02de956f953c1fa729c49c403ecd49dfbaf4dae0db97642021eb125e34c5b5bb` | KWS 生命周期与 IRAM 候选目标构建通过，1862344 B；未烧录 |

本次文档整理未额外烧录。最后已烧录镜像为 `voice-cachefix.bin`；
串口采集进程已停止。下一次操作前仍需确认设备和占用状态。

## 3. 配置矩阵与隔离要求

配置目录均位于 `board/esp32p4/esp32p4-function-ev-board/configs/`。
以下“计划”项尚未建立，不作为当前可直接构建的配置名。

| 配置/用途 | CPU | 音频 | TFLM | 常驻 KWS | ESP-NN | 工具链 |
| --- | --- | --- | --- | --- | --- | --- |
| 已有 `smart_home` | SMP 2 核 | 开 | 以最终配置为准 | 本轮基线关闭 | 关闭 | openvela 默认 |
| 已有 `smart_home_voice` | 1 核 | 开 | 本轮镜像未启用 | 关闭 | 关闭 | `riscv-none-elf` |
| 已有 `smart_home_kws` / reference | 1 核 | 继承 voice；smoke 不开设备 | 开 | **关闭** | **关闭** | `riscv-none-elf` |
| 计划：KWS 常驻验收配置 | 1 核 | 开 | 开 | 开 | 先用已达标后端 | 对应后端 |
| 计划：ESP-NN 对照配置 | 1 核 | 同 reference | 开 | 关闭 | 开 | 仅该配置选择需要的工具链 |
| 计划：最终 SMP KWS 配置 | 2 核 | 开 | 开 | 分步开启 | 已通过后端 | 对应后端 |

`smart_home_kws` 的 TFLM/C++ 依赖、TLS 配置和 smoke 已进入主线，当前 arena 保留
393216 B。配置注释中的“不开音频”仅指 smoke 不打开设备；该配置仍继承音频驱动，
不能写成无音频构建。主线 Kconfig 的服务默认 arena 为 196608 B，开启常驻前须依据
**目标**实际占用单独配置，不能只改 smoke arena。

构建源隔离必须检查四处：最终 chip 路径、board 路径、
`board/esp32p4/common/board`、外层 `packages/demos/smart_home` 符号链接。
`scripts/build_p4x.sh` 能找到外层构建根和编译器，但不自动改写 defconfig 中指向
主检出的源码路径，也不自动切换应用链接。worktree 路径不等于源码已隔离。

本轮临时配置及构建脚本保存在证据目录：`config-voice/`、`config-kws-reference/`、
`build-voice-cachefix.py`、`build-kws-reference-cachefix.py`。它们会临时切换应用链接，
构建结束恢复，使用共享 `nuttx/`，只允许独占运行。后续新构建必须换日志名，
构建前检查其路径和恢复逻辑；不把这些临时脚本视作已经交付的通用隔离工具。

## 4. P0：本地音频闸门与回归

### 4.1 操作顺序

启动 `smart_home_voice`，先检查设备，录放顺序执行，不同时占用麦克风：

```text
ls /dev/audio
audio_smoke play 1
audio_smoke record 10 /data/voice_gate_10s.pcm
ls -l /data/voice_gate_10s.pcm
audio_smoke playfile /data/voice_gate_10s.pcm
tts_smoke play /data/voice_gate_10s.pcm 16000
tts_smoke play /data/hi_openvela.pcm 16000
ps
```

应出现 `pcm0`、`pcm_in0`。格式为 16000 Hz、单声道、PCM16，小端裸 PCM；
目标数据率为 32000 B/s，10 秒恰为 320000 B。录音期间用户连续说话并轻拍一次桌面。
复测前先确认同名旧文件已归档，避免把原验收样本覆盖掉。

### 4.2 通过和失败处理

- 命令全部返回，录音长度正确，串口持续可响应，无残留录放任务。
- 用户确认两条播放器的人声和固定语音均清楚，无周期性咔哒。
- PCM 数据检查只补充削波、空槽、零值、周期突变证据，不代替听感。
- 出现 0 B、短录音、无声或整机失响应即停止后续叠加。记录最后日志；串口无响应时
  使用匹配该镜像的 ELF 读取 PC/异常寄存器/栈，保存后再复位。

本轮已通过。修改 I2S/codec、共享 capture/player、缓冲、缓存停用路径、任务生命周期、
工具链或 SMP 后，至少重新执行短录放与固定语音；最终组合再执行完整 P0。

## 5. P1：相机 IRAM 与自测生命周期收敛

### 5.1 修改范围

| 变更组 | 文件 | 目的 |
| --- | --- | --- |
| P1-A IRAM | `board/esp32p4/common/scripts/esp32p4_sections.ld`、`esp32p4_sections.rev3.ld` | 把 `libarch.a:esp_cache_utils.*` 与 `libsched.a:sched_switchcontext.*` 的代码和依赖只读数据放入可用内部区域 |
| P1-B 自测 | `src/tools/smart_home_camera_tools.[ch]`、`src/app/smart_home_main.c`（均在 `demos/smart_home/`） | 批量自测保持同一任务组；退出前清理自己启动的相机；严格检查业务结果与新帧 |
| 必要时服务修正 | `src/camera/smart_home_camera_control.c`、`smart_home_camera_service.c` | 仅在自测修正后仍复现生命周期问题时修改，保持 UI 相机所有权清楚 |

A 候选已在工作区并完成构建/板测；B 只有外部补丁草稿。A/B 应分别验证，
最终同一主线镜像通过全部 P1 验收后再提交对应功能修复。

### 5.2 实施步骤

1. 审核两个链接脚本都覆盖实际 HAL 文件名，用最终 ELF/map 确认 suspend/resume
   cache、suspend/resume scheduler 及必要调用路径位于 IRAM；核对 IRAM 剩余空间。
2. 完成自测参数解析：默认一次，批量范围 1..100，拒绝非法输入和不支持的 action。
3. 记录调用前相机状态。只清理本次自测启动的相机，使用既有 control shutdown/join；
   不接管已有 UI/服务相机，不留下指向已销毁 pthread 的全局“运行”状态。
4. 批量 shot 在一次命令内循环，每次解析 `ok`、`pending`、`bytes`、`seq`、`frame_seq`；
   使用尺寸常量计算预期字节，禁止只匹配固定字符串。失败/超时必须返回非零。
5. 新帧等待有界，超过当前帧序号后才能落盘。每次都检查任务存活及可响应性；
   不靠额外 sleep 或文件序号伪造采集持续运行。
6. 检查磁盘余量。512×300×2=307200 B，一次原子替换至少需要一个新临时文件及元数据
   余量；双槽保存还需考虑两个既有文件。记录 `df`，不得全片擦除或删除用户语音。
7. 先单张启动/停止两轮，再一次任务内拍 20 张；再独立启动两次验证没有孤儿状态。

### 5.3 命令与验收

当前已有命令：

```text
smart_home --selftest-camera-tool status
smart_home --selftest-camera-tool shot &
```

计划新增，**当前主线尚不支持次数参数**：

```text
smart_home --selftest-camera-tool shot 20 &
```

通过条件：20/20 业务成功，全部 307200 B；单个连续采集会话内 `frame_seq` 严格递增，
落盘 `seq` 也递增；`latest.json` 指向有效文件；无 OOM、无资源持续下降、串口不冻结。
重启采集后帧序号可能归零，应使用会话标识区分，不能跨会话机械比较。
20 次不代表需保存 20 个文件，允许复用原有双槽。

本地自测通过后，再从实际 agent 输入“拍照”，核对工具调用、业务结果、文件与帧序号。
网络缺失时只标记“本地 handler 路径通过，agent 闭环待验”，不得标为完整 P1 通过。
当前单次落盘约 27–45 秒，需保留耗时记录并排查文件系统/流水线影响；
本轮没有约定快拍时延，不能把“最终返回”等同于交互体验达标。

失败保存 `ps`、堆信息、磁盘余量、相机状态、完整串口与精确 ELF；冻结时补 JTAG。
本阶段退出条件同时包含新帧、文件、清理、agent 路径；仅增加 `seq` 不算通过。

## 6. P2：VAD 参数与受控断句验收

已有 VAD 已合入，实施重点为诊断可测性和真实断句，不再重放早期两个提交。

### 6.1 修改与宿主验证

- `src/voice/asr_smoke_main.c`：为 `silence_ms` 使用独立毫秒解析，范围与接口约定一致；
  拒绝非法参数并打印生效值。当前误用最大 30 的秒数解析器，传入 500 会失败后静默保留
  默认 500；日志相同不能证明参数已生效。覆盖 0/500/1000、越界和非数字行为。
- 为采集诊断增加可选 PCM 导出能力及起止时间/字节数。复用生产 capture，避免另写
  一条“看起来通过”的采集实现。新增命令格式在实现后更新帮助和本计划。
- 审核 `smart_home_voice_capture.c` 的消费者替换/恢复、退出及分配失败路径，
  确保断句后 KWS 消费者可恢复，避免指向栈上 collector 的回调残留。
- 保留现有四场景宿主回归；宿主通过后再进行板测。

```bash
cd /home/arongw/openvela/worktrees/voice-gates-codex/demos/smart_home/tests/vad
gcc -O2 -Wall -Wextra -Iinclude -I../../src/voice \
  vad_selftest.c ../../src/voice/smart_home_vad.c \
  -lm -o /home/arongw/openvela/out/voice-gates-20261102/vad_selftest
/home/arongw/openvela/out/voice-gates-20261102/vad_selftest
```

### 6.2 板端 A/B 协议

当前命令为 `asr_smoke record 6`、`asr_smoke record-vad 6 500`。
先修正参数诊断再用其调参；当前命令会释放录音，不提供文件回放证据。

采用同一位置、相同增益、相同话术，配对测 3 轮。每次从采集就绪后立即说约 1 秒
“你好，打开客厅灯”，随后保持安静；同时记录采集就绪、实际发声起止、VAD 事件、
停止采集和命令返回时间。

| 条件 | 通过标准 |
| --- | --- |
| 固定时长 | 约 6 秒采集，报告实际字节/32000；不能仅看墙钟时间 |
| VAD 开启 | 以同一采集起点计算录音长度 1.3–1.9 秒，3/3；若就绪后延迟说话，应另列等待时长 |
| 内容完整 | 导出并回放，首字“你”和尾字“灯”均完整，3/3；用户确认 |
| 无语音 | 有界达到最大时长返回，不误报一次成功人声断句 |
| 恢复 | 再录音正常，消费者和设备可继续使用，无残留任务 |

默认 VAD 结束累计约 300 ms，采集器还等待约 500 ms 尾静音，需分别记录。
历史 1.6/3.8/5.9 秒结果不足以证明 3/3 达标。未达标先对齐计时、样本与有效参数，
再决定阈值和尾静音调整；每次只改一个参数。

## 7. P3：KWS 先修生命周期，再对齐前端，最后常驻

### 7.1 P3a：推理所有权与目标 reference 基线

修改范围：`src/voice/kws/kws_infer.[ch]`、`kws_smoke_main.cc`、
`smart_home_kws_service.c`，新增宿主回归位于 `tests/kws_infer/`。
上述路径均相对 `demos/smart_home/`。

实现要求：resolver 存续到解释器销毁；初始化失败完整清理；同一实例只由所属 arena
销毁；释放 arena 前调用 deinit；其他 arena 接管返回 `-EBUSY`。
接口当前为单实例，必须串行使用；所有权检查不等于线程安全，常驻与 smoke 不可并发
调用。常驻前补充入口互斥/拒绝策略及退出等待，不能用释放内存解除运行中推理。

候选宿主测试已覆盖 arena 不足后恢复、未对齐 arena、外来所有者拒绝、
10 次重建/200 次相同输出，真实 TFLM + ASan/泄漏检查通过。
候选测试脚本尚在工作区，合入后可用：

```bash
cd /home/arongw/openvela/worktrees/voice-gates-codex
python3 demos/smart_home/tests/kws_infer/run_host.py \
  --build-dir /home/arongw/openvela/out/voice-gates-20261102/host-kws
```

下一步使用已构建 `kws-reference-cachefix.bin` 上板；不要把宿主约 205616 B
arena 占用当作目标 ABI 测量。目标需要打印 reserved/used、地址和实际内存区域，
确认 PSRAM、对齐及内部堆余量。arena 地址本身还需结合链接/内存映射判断。

板端分两步：

```text
kws_smoke --warmup 0 --repeat 1
kws_smoke --warmup 20 --repeat 100
```

第一步能返回且输出稳定后，再执行第二步。多次独立命令重建，检查没有失效解释器、
堆持续下降或串口冻结；出错返回后能重新运行。P3a 完成要求 100/100 成功、P95≤20000 µs。
使用现有 nearest-rank P95：排序后的第 `ceil(0.95*N)` 项。

smoke 使用确定性伪特征，仅测模型路径。它不能证明“hi openvela”可识别，
也不覆盖 FFT/特征前端和音频采集。首个 Invoke 再卡住时保留现场及精确 ELF，
不直接改 ESP-NN 或扩大线程栈来掩盖问题。

### 7.2 P3b：模型来源和训练前端 golden 对照

当前编译模型与本地部署归档字节一致：37408 B，SHA-256
`e3adfe563ae5febe3d1fd89cc13a972f2da5f634fd6aed78661d0e1c26004ec9`。
归档为 `/home/arongw/kws_models/wake_large_deploy.tar.gz`；训练实现位于
`/home/arongw/Documents/audio/micro_model/train_s3.py`。
找到代码不等于证明这是该模型的精确导出 revision，需把源码 SHA、部署 metadata、
导出参数和模型 SHA 一起固化；未证实项继续标为来源缺口。

对照契约：

| 项目 | 训练实现/部署配置要求 |
| --- | --- |
| PCM | 16000 Hz，2.5 秒/40000 点，归一化浮点；长音频中心裁剪，短音频左对齐并在尾部补零 |
| STFT | 窗长 480，步长 320，FFT 512，periodic Hann，`pad_end=False` |
| 频谱 | 幅度谱 `abs(STFT)` |
| mel | 40 维，125–7500 Hz，对照 TensorFlow `linear_to_mel_weight_matrix` 的真实数值 |
| 基础特征 | `clip((ln(mel + 1e-6) + 12) * 1.625, 0, 26)` |
| 差分 | `d[1:]=base[1:]-base[:-1]`；`d2[1:]=d[1:]-d[:-1]`，初帧为零 |
| 布局 | 124×40×3，按每个 time/mel 的 base、d、d2 排列，共 14880 值；核对实际 tensor shape |
| 量化/分类 | 从实际 TFLite tensor 读取 scale/zero-point；类别顺序 wake/hard_neg/other_speech/background/silence |

`ee274d7` 改善了多个前端差异，但不直接整提交合入。已发现两个待 golden 复核点：
其 mel 三角斜率在 Hz 轴计算；二阶差分仅在 `t>1` 计算，第二帧与训练实现不同。
旧主线 126 帧/40480 点与候选 124 帧/40000 点也要显式记录，避免输入长度差异掩盖算法差异。

实施步骤：

1. 建立独立宿主 golden 生成器，锁定 Python/TensorFlow/依赖版本、随机种子、模型和
   训练源码 SHA，禁用训练增强。临时环境存在于 `/run/user/1000/`，不能当作持久交付。
2. 固定测试集：数字静音、低幅噪声、单频/双频、脉冲、真实唤醒词、相似词、其他语音。
   用户提供的 `/data/hi_openvela.pcm` 可加入，但需先确认格式、长度及对应标签。
3. 同一 PCM 跑训练 golden、旧 C 前端、候选 C 前端；保存各层浮点、量化 int8、
   5 类原始输出与概率。旧前端所需额外尾部填充单独记录。
4. 比较窗、幅度谱、mel 权重/输出、log、delta/delta2、量化值，再比较模型分数。
   差异首次出现的层决定修复位置，禁止先调整唤醒阈值补偿前端错误。
5. 预先采用浮点 `atol=1e-4, rtol=1e-4` 为核查阈值；超出必须定位和说明。
   最终喂模型的 int8 张量与同一量化规则的 golden 要逐字节一致。
   边界舍入差异也必须留证并修正；不能通过事后扩大容差宣布通过。
6. 对同一输入验证板端特征/模型输出与宿主一致，再提交前端修复与 golden 回归材料。

此处的完整 golden A/B 尚未执行；现有前端宿主自测不等价于训练链路对齐。

### 7.3 P3c：常驻服务与唤醒验收

前置：生命周期、前端正确性和选用后端的性能均达标。然后在独立配置开启
`CONFIG_SMART_HOME_KWS=y`，为服务单独设置经目标测量确认的 arena。

检查 capture 消费者交接、录音期间暂停/恢复、停止等待、callback 生命周期、
worker 栈余量和静态前端缓冲串行访问。完整窗口计算的 FFT+前端+Invoke 耗时
需低于配置的推理间隔，另报端到端延迟；Invoke≤20 ms 不代表整条链路≤20 ms。

验收：

- 10 次真正断电冷启动均进 NSH、服务就绪、串口响应、无冻结。只有软件/RTS 复位时
  另记“复位 10 次”，不冒充断电验收；断电需可控电源或用户配合。
- 安静室内、1.5 m、预定音量和方位，唤醒词 10 次至少 8 次；记录每次得分、投票、
  触发和冷却，不把重复回放同一个样本解释为多人的泛化能力。
- 连续 10 分钟静音场景误唤醒≤1，另记录实际背景声。
- 常驻运行时再执行录音/回放，退出后能重启，无内存持续下降、无残留线程。

未通过保持常驻关闭，保留 smoke 和可复现样本；不把“能打印分数”写成 KWS 完成。

## 8. P4：ESP-NN 真正接入 P4 与性能验收

### 8.1 集成策略和修改边界

参考 `/home/arongw/openvela/ccf_audioevent/docs/优化文档/ESP-NN移植到openvela实施指南.md`
的 wrapper、fallback、逐节点校验和计数方式；S3 的收益不作为 P4 结果。

保留主线 `patches/nuttx/0012-espressif-riscv-toolchain.patch`：它按可选配置分别支持
Make/CMake。丢弃移植分支中强制全平台工具链及 `xespv` 的旧版本，不能按同名文件
整体覆盖。默认 voice/reference 继续使用 openvela 提供的 `riscv-none-elf`。

| 区域 | 计划工作 |
| --- | --- |
| contest `packages/esp-nn/` | 锁定上游版本/提交、LICENSE、源码清单；导入 P4 必需源，排除 `.o` 等生成物；配置关闭时不拉入代码 |
| 计划新增 `packages/esp-nn/Kconfig`、`Makefile`、`Make.defs` 及对应包发现入口 | 按实际包入口接线，明确 P4 后端开关及工具链依赖；默认关闭 |
| 外层 `apps/mlearning/tflite-micro/` | 审核 Kconfig/Makefile 及 Conv/DW wrapper，放开 P4 受控依赖；现有未提交实验先比对后复用 |
| KWS resolver/smoke | 注册到实际 wrapper，报告真实 backend 与命中计数，增加原始输出对照 |
| 补丁交付 | apps 修改也需固化可重放补丁/明确版本；若再改 nuttx，同提交更新 contest `patches/nuttx/` |

第一轮优先 Conv2D、DepthwiseConv2D。按真实模型节点参数判断适用性，不支持形状、
量化或 scratch 时回 reference，记录 fallback 原因；不为增加命中数强行走错误内核。
Mean/FC/Softmax 是否接入由 profile 决定。不启用破坏逐字节一致性的近似取整捷径。

工具链先使用本地已下载的独立 prebuilt 验证，记录版本、校验和、许可、下载来源。
`.espressif` 是安装位置，不是算法依赖本身；需要 vendor 指令的是具体后端。
验证后再决定 manifest/独立二进制仓库分发方式，不把大工具链打包进比赛源码仓库。
若 P4 内核要求状态寄存器/PIE 上下文，核实初始化和线程切换约束，最终在 SMP 另验。

### 8.2 正确性先于计时

1. 修正现有 `backend=reference` 硬编码，输出构建配置和实际分派后端。
2. 区分 wrapper 进入、ESP-NN C 内核、P4 汇编内核和 fallback 次数。
   至少目标热点实际命中；仅链接出符号或打印 ESP-NN 名称不算接入。
3. 核对真实编译命令包含 P4 源与所需 ISA；检查 objdump/map 的函数与调用。
4. 用同一模型、输入、量化参数逐节点比较 reference 与 ESP-NN 原始 int8 张量；
   核查 scratch 大小/对齐、padding、offset、per-channel multiplier/shift 及激活边界。
5. DW scratch 断言必须从 API 计算、分配、传递和释放链路修复；禁止关断言掩盖问题。
6. 全模型原始 int8 输出逐字节一致，5 类概率一致；仅 argmax 一样不算通过。
   多种真实/合成输入均覆盖，不能只测一份伪特征。

### 8.3 性能试验

先开 trace/逐节点校验确认路径，正式计时关闭打印、张量导出和 VERIFY。
两态保持模型、输入、CPU 频率、CPU 数、亲和性、arena 区域及后台负载一致；
说明工具链改变带来的混杂。如需归因 ESP-NN 内核，再补“相同 vendor 工具链、ESP-NN 关”对照。

```text
kws_smoke --warmup 20 --repeat 100
```

reference/off 与 ESP-NN/on 各独立运行 3 轮，每轮 100 次；保存逐次时间及原始输出，
报告每轮 min/P50/mean/P95/max、arena/scratch、命中/fallback 和镜像 SHA。
仅现有汇总日志不足时先增加原始数据导出，再正式测试。

以三轮 P95 的中位数计算：

```text
改善率 = (reference_P95 - espnn_P95) / reference_P95 × 100%
```

通过要求：每轮全部成功，输出一致、目标内核真实命中；中位数改善≥15%，
且每一对轮次也达到≥15%，避免单轮偶然收益。选用后端最终仍需满足 P3 的 P95≤20 ms。
同时报告端到端前端+推理，不能把 Invoke 收益当作完整语音链路收益。

未达到收益时保留 reference 默认，给出热点/fallback/带宽分析；“包已导入”最多标记
集成候选，不标为 ESP-NN 加速完成。

## 9. P5：云端 ASR/TTS 闭环

网络预检与 API 功能分开记录。读取实际配置中的 ASR/TTS endpoint，脱敏只保留域名、
端口和服务类型；不输出 secrets/token。历史其他域名解析失败不能直接代表当前服务状态。

依次确认：Wi-Fi 关联→IP/网关/路由→实际服务域名 DNS→系统时间/证书→TCP/TLS→API。
`ping` 不通可能是 ICMP 禁用，不能单凭此判 DNS 失败；DNS 通也不代表鉴权通过。
按当前镜像的 NSH 工具帮助选择 DNS 检查方式；无 DNS 命令时增加脱敏应用预检，
不要填写不存在的 `nslookup` 指令。

网络通过后：

```text
asr_smoke record 4
asr_smoke recognize 4
tts_smoke speak 你好，我是智能家居管家
```

固定采集理论值为 128000 B（4×32000）；以实际时长/字节和明确短读原因判定。
ASR 3 次至少 2 次命中预定关键短语，保存脱敏结果；TTS 3 次至少 2 次合成并完整播出，
用户确认内容清楚且无周期咔哒。记录首字节、合成、播放各阶段耗时，目标交互 10 秒内完成，
失败场景 30 秒内退出并可重试。若现有超时策略不能满足，先修复再验收。

DNS/出网不可用标 `BLOCKED_NETWORK`，鉴权失败另记 `BLOCKED_CREDENTIALS`；
保留本地录音和播放通过证据，修复外部条件后只补相关云端试验。
不得把网络失败判为音频驱动失败，也不能用本地固定 PCM 代替云端 TTS。
agent“拍照”若依赖云端，在此补完 P1 的自然语言路径。

## 10. P6：最终 SMP 组合复验

以已通过的双核 `smart_home` 和单核 KWS 结果为起点，不直接同时开启全部服务。
最终配置核对 `CONFIG_SMP=y`、`CONFIG_SMP_NCPUS=2`，并记录调度亲和性；
CPU1 ready 只证明启动，不证明音频/KWS 长期稳定。

| 步骤 | 本次新增变量 | 验收 |
| --- | --- | --- |
| 6a | 当前功能基线的 SMP 配置 | 进 NSH，CPU0/CPU1 存活，串口持续响应，配置/资源路径正确 |
| 6b | 顺序音频 | 完整 P0，录放结束设备释放，用户听感确认 |
| 6c | 独立 `kws_smoke` | 100/100、输出正确、P95 复测；不同时启动常驻实例 |
| 6d | 常驻 KWS | 10 次启动测试、录音暂停/恢复、唤醒验收、停止重启 |
| 6e | 相机及 agent 组合 | 新帧落盘、语音交互后仍可拍照，音频/相机依次操作无冻结 |

每步保存 CPU 负载、线程状态/亲和性、栈高水位、内部 SRAM/PSRAM 可用量与最大连续块。
通过短时后做至少 30 分钟组合运行，记录采集丢帧、推理周期超时、内存趋势和错误。
若 vendor 内核受每核状态影响，分别验证固定核与允许迁移；不能只在 CPU0 跑通便宣称
双核上下文保存正确。资源不足则先回到单核已通过配置定位，保留现场再复位。

P6 不取代 P3c 的真实断电冷启动和声学验收。只有所有本地阶段通过，且云端阶段取得
约定证据，才能标记整体完成；外部阻塞必须单独保留。

## 11. 构建、烧录、日志和证据规范

### 11.1 基线构建命令

以下命令用于**已合入主线源码**的重建；未提交 worktree 候选必须使用已检查的源隔离路径，
不能用这些命令声称测试了候选代码。与当前板子配置切换前先归档现有 `.config`/产物。

```bash
cd /home/arongw/openvela
source myenv/bin/activate
./build.sh \
  contest2026_031_niudanxianqianchong/board/esp32p4/esp32p4-function-ev-board/configs/smart_home_voice \
  -j2
# KWS reference 时将配置目录改为 smart_home_kws；构建后必须重新核对最终 .config。
```

每次产物立即复制到唯一阶段目录，保存 `.config`、ELF、BIN、map（如生成）、
编译日志、源码 commit/未提交差异 SHA、外层 apps/nuttx 状态、工具链版本和完整命令。
日志文件不可复用，防止后续构建覆盖证据。

### 11.2 指定镜像烧录示例

下例是当前 P0 已验收镜像的恢复命令。新候选只能在核对清单后替换 `P4_IMAGE`；
先退出 picocom/采集器并确认无进程占用。使用当前设备发现结果，设备名可能改变。

```bash
cd /home/arongw/openvela
source myenv/bin/activate
ls -l /dev/serial/by-id/
P4_PORT=/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_E8:F6:0A:E3:A6:58-if00
P4_IMAGE=/home/arongw/openvela/out/voice-gates-20261102/voice.bin
sha256sum "$P4_IMAGE"
esptool --chip esp32p4 image-info "$P4_IMAGE"
fuser "$P4_PORT"
esptool --chip esp32p4 --port "$P4_PORT" chip-id
esptool --chip esp32p4 --port "$P4_PORT" --baud 921600 \
  write-flash -fs 16MB -fm dio -ff 80m 0x2000 "$P4_IMAGE"
picocom -b 115200 "$P4_PORT"
```

烧录前核对芯片 ESP32-P4 rev3.2 / MAC、Simple Boot、应用偏移及分区表。
本轮已验证应用偏移 `0x2000`，16MB/DIO/80MHz；保留 `/data`（`0x600000`、`0xa00000`）。
不执行全片擦除，不覆盖资源分区。烧录后等待正常启动、按回车进入 NSH，执行对应阶段命令。
`Hash of data verified` 不代替启动验收。

串口日志采用带时间戳的持续读取器，后台始终读串口，避免按请求读取造成 USB 回压和
错误耗时判断。现有 `console_continuous.py` 在证据目录，属诊断脚本；正式交付应固化
参数、超时和日志格式。删除旧文档对不存在 `/tmp/fbtime.py` 的依赖。

JTAG 仅在实际权限错误时根据当前 USB bus/device 节点请求临时 ACL。
本轮 `003/056` 权限已解决，不能继续写“等待权限”；重新枚举后再按实际错误处理。
现场分析必须使用该次烧录的 ELF，目标配置改变后重新生成 TCB 偏移。

### 11.3 每阶段必须保存的记录

| 类别 | 内容 |
| --- | --- |
| 来源 | contest/apps/nuttx 版本与差异、配置、模型/工具链 SHA、构建与烧录命令 |
| 日志 | 带时间戳原始串口、退出码、设备身份、启动日志；冻结时 JTAG 寄存器/栈 |
| 数据 | PCM/特征/原始 int8 输出/拍照 metadata，样本 SHA 与格式 |
| 性能 | 原始计时、统计脚本/P95 定义、CPU/负载/亲和性、arena/scratch/堆/栈 |
| 验收 | 预定标准、实际结果、用户听感/冷启动配合结果、未完成项 |

当前可核对证据包括：`serial-v3-events.jsonl`、`p0-voice-stats.json`、
`jtag-camera-3.log`、`jtag-camera-4.log`、`cachefix-iram-symbols.json`、
`flash-cachefix-2.log`、`serial-continuous-events.jsonl`、`camera-shots-stale.json`、
`host-kws/baseline-asan.log`、`host-kws/lifecycle.log`、
`build-kws-reference-cachefix.log`、`kws-reference-cachefix-artifacts.json`。
首次 `flash-cachefix.log` 因串口占用失败，成功证据是后缀 `-2` 的日志。
原 P0 `build-voice.log` 被后续构建覆盖，P0 的镜像、配置、烧录与串口证据仍在；
不得将后续构建日志冒充原 P0 日志。

## 12. 提交、主线推进与回退

1. 继续使用 `feat/voice-gates-codex`；每次操作先检查主线和工作区状态。
2. 功能按完整验收单元提交，建议依次为：camera 修复、VAD 诊断与验收、
   KWS 生命周期、KWS 前端对齐、常驻配置、ESP-NN 集成、最终 SMP 配置。
   每组包含其必需测试/配置/文档，不提交“失败尝试已完成”的中间状态。
3. 纯计划文档可独立提交，验证为链接、命令、状态与内容审查；本次文档更新不表示
   未提交修复已经合入。尚未验证源码收工前命名 stash 保存，不混入文档提交。
4. 主线推进前确认干净、检查分支关系；只 `merge --ff-only`。
   若主线已前进，在 worktree 更新并重新验证，不在主检出 rebase/cherry-pick/reset。
5. nuttx 改动同提交维护补丁系列，并在干净指定基线按顺序重放检查；
   apps 集成同样交付可重放补丁和基线。已有外层改动先辨认来源，不覆盖。
6. 故障回退优先切回已保存、已验证的指定镜像和配置；源码回退在 worktree 生成
   针对性 revert，验证后 fast-forward，禁止用 hard reset 抹掉其他成果。
7. 继承实验 stash `c5da9b035b2c2b82fa23980bbf8b28f1e1b593f8` 与 KWS 候选快照
   `cf1afbdf89da0d31056456bb464e869690007096` 保留，归档和差异在证据目录。
   不整包 pop 到主线，选择性审查后复用。

## 13. 下一轮按顺序执行的任务清单

- [x] P0 独立构建/烧录、10 秒录音、两条播放器与固定语音听感验收。
- [x] 相机失响应现场读取，IRAM 候选构建/烧录并恢复响应。
- [x] KWS 真实 TFLM ASan 负对照、生命周期候选宿主回归与 P4 构建。
- [ ] 完成相机自测批量/所有权清理修复；两轮独立调用 + 20 张新帧，无 OOM。
- [ ] 将相机修复后的镜像做 P0 回归，满足条件后提交；agent 路径缺条件时明确待验。
- [ ] 修正 VAD 毫秒解析并提供样本证据；完成 3 轮固定/断句 A/B 与首尾字验收。
- [ ] 烧录 KWS 生命周期候选，先单次 Invoke，再 20 warmup/100 次计时及重复初始化。
- [ ] 完成模型来源固化、训练 golden 与旧/新 C 前端 A/B，修正剩余差异并验证板端。
- [ ] 若 reference 性能不达标，先做 ESP-NN 正确性/性能后回补 P3；常驻保持关闭。
- [ ] 完成 ESP-NN P4 实际内核命中、逐字节对照和开/关各 3 轮性能。
- [ ] 开启常驻并验唤醒率、静音误触发、设备交接、10 次真实冷启动。
- [ ] 对实际 ASR/TTS endpoint 做 DNS/TLS/API 分层检查，完成各 2/3；补 agent“拍照”。
- [ ] 一次一个变量完成双核 NSH→音频→smoke→常驻→相机组合和 30 分钟运行。
- [ ] 汇总所有阶段证据、失败/阻塞与版本清单，更新问题分析和交付说明。

以上为执行顺序，不承诺未经测量的完成日期。每个未完成项只有取得约定证据后才能勾选；
任何网络、供电或听感配合缺失都应明确记录，不能替换成宿主结果。
