# ESP32-P4X Smart Home 端侧 KWS 接入方案

> 状态：方案阶段。当前 smart_home 的语音模块是桩实现
> （demos/smart_home/src/voice/smart_home_voice_stub.h），无麦克风采集、
> 无端侧推理；P4X 板麦克风硬件通路尚未确认。本方案只描述设计与验收
> 判据，不代表功能已实现。
>
> 适用对象：ESP32-P4X Function EV Board、Smart Home。
>
> 相关文档：ESP32-P4X-端侧目标检测方案.md（同目录）、
> ../网络与连接/ESP32-P4X-SmartHome板载C6-WiFi接入方案.md。

## 1. 目标

在 ESP32-P4 上实现端侧关键词识别（KWS），接入 smart_home 应用层，
使设备在不依赖 Wi-Fi 的前提下具备：

    麦克风常驻监听 → 唤醒词命中 → 本地状态机（UI 提示 / 开麦 / 启动会话）
                        └→ 命令词命中 → 本地工具直执行，或提交 cAGENT

必须区分三类输入，行为不同：

| 类型 | 例子 | 行为 | 消耗 token |
| --- | --- | --- | --- |
| 唤醒词 | 自定义唤醒短语 | 本地状态机：UI 提示、开麦、启动会话 | 否 |
| 命令词 | 开灯、关空调 | 本地工具直执行（确定性映射） | 否 |
| 自由语句 | 客厅有点闷 | 需要理解 → 云端 ASR/LLM | 是 |

端侧 KWS 的价值在于常驻、离线、低延迟、零 token。它不是 ASR 的替代，
而是 ASR 的前置触发器与本地命令的快速通道。

## 2. 现状与可复用资产

| 资产 | 位置 | 状态 |
| --- | --- | --- |
| 应用层扩展点 | demos/smart_home/src/voice/smart_home_voice_stub.h | 桩，接口已定义 |
| 已训练模型 | ccf_audioevent/train/{small,medium,large}_clean/ | int8/float tflite + model.cc |
| 音频前端 | ccf_audioevent/app/audio_event/dsp/feature_extract.c | 可移植 |
| 检测与后处理 | ccf_audioevent/app/audio_event/{model,detector}/ | 可移植 |
| 性能基准参考 | ccf_audioevent/app/audio_event/tflm_benchmark/ | 可移植 |
| 推理运行时 | apps/mlearning/tflite-micro | 在树中可用 |
| 音频后端参考 | ccf_audioevent/board/esp32s3-devkit（INMP441） | 仅参考，硬件不同 |

### 2.1 应用层扩展点已由桩文件固定

    int  voice_capture_start(void);
    int  voice_capture_stop(char *buf, size_t size);
    void voice_capture_cancel(void);
    int  voice_play_text(const char *text);
    /* 桩注释原文：voice_capture_* 在独立线程中运行，voice_play_* 可在任何
     * 线程调用。LVGL 更新通过 lv_async_call 投递。 */

接入方式是同签名替换，不是重新设计接口。

### 2.2 已有模型的类别布局（重要）

ccf_audioevent 的类别在编译期选择，见
ccf_audioevent/app/audio_event/audio_event_config.h：

    4 类（默认）：knock, cough, background, silence
    8 类：        knock, cough, glass_breaking, yes, no, stop, ...
                  └──── 事件类 ────┘  └── 关键词类 ──┘

即 8 类模型已经包含关键词类（yes / no / stop），可作为命令词的直接起点，
无需从零训练。但唤醒词（自定义短语）需要重新训练或另建模型，因为现有
类别中没有唤醒词。

### 2.3 性能约束（必须先解决的已知问题）

| 事实 | 位置 | 影响 |
| --- | --- | --- |
| TFLITEMICRO_ESP_NN 依赖 ARCH_CHIP_ESP32S3 | apps/mlearning/tflite-micro/Kconfig | P4 当前无法使用 ESP-NN 优化算子 |
| apps/mlearning/esp-nn/ 目录为空 | apps/mlearning/ | 优化算子源码未就位 |
| P4 无 S3 的 AI 向量指令，双核 400 MHz | 硬件事实 | 推理耗时须实测，不能照搬 S3 数据 |
| S3 参考配置逐节点指定 esp-nn 掩码 | ccf_audioevent 的 audio_event_espnn_profile | 该模型对算子优化敏感 |

结论：K1/K2 阶段必须先量出无优化算子下的单帧推理耗时。若超出实时预算
（每帧 20~30 ms），优先解决算子优化问题，而不是先换更小的模型。

## 3. 范围和非目标

本期包含：麦克风采集（I2S/PDM，硬件待确认）与环形缓冲；前端特征提取与
端侧推理；后处理（置信度、连续命中、平滑、去抖）；唤醒词与命令词分流；
最小 UI 反馈（经 lv_async_call 投递）；与 cAGENT 的接入。

本期不包含：TTS 实现（保留 voice_play_text 接口，后端可继续为云端）；
云端 ASR 接入；远场降噪、回声消除、波束成形、多麦克风阵列、声源定位；
说话人识别；在硬件通路未确认前宣称唤醒率/误唤醒率指标。

## 4. 分层设计

### 4.1 应用层（demos/smart_home/src/voice/）

| 文件 | 动作 | 职责 |
| --- | --- | --- |
| smart_home_voice.h / .c | 新增 | 实现桩的四函数；持采集线程与状态机 |
| smart_home_voice_capture.c | 新增 | I2S/PDM 采集到环形缓冲；cancel 丢弃已录数据 |
| smart_home_voice_frontend.c | 新增 | 分帧/加窗/FFT/Mel/MFCC（移植 feature_extract） |
| smart_home_voice_kws.c | 新增 | TFLM 解释器、tensor arena、单帧推理与得分 |
| smart_home_voice_post.c | 新增 | 阈值、连续命中、去抖、唤醒/命令分流 |
| Kconfig / CMakeLists.txt | 新增 | 开关、模型路径、阈值、线程栈与优先级 |

### 4.2 板级与内核层（前置条件，不属应用层）

| 项 | 位置 | 职责 |
| --- | --- | --- |
| 麦克风驱动配置 | board/esp32p4/.../configs/*/defconfig | I2S/PDM 实例、采样率、位宽、DMA 缓冲 |
| 板级引脚与时钟 | board/esp32p4/.../include/board.h | 麦克风引脚与时钟常量 |
| 音频设备节点 | /dev/audio 或 I2S 字符设备 | 应用层采集入口 |

前置未决：P4X 板是否板载麦克风、是 I2S 数字麦还是 PDM、走哪个实例，
必须先看原理图或用户指南确认，不得凭猜测写 GPIO。

### 4.3 模型与资源

| 项 | 方案 |
| --- | --- |
| 模型格式 | int8 TFLite 优先；LittleFS 资源或内嵌 C 数组 |
| 类别集 | 命令词沿用 8 类模型的关键词类；唤醒词需新训练 |
| tensor arena | 优先内部 SRAM（模型小） |
| 采样率与帧长 | 必须与训练时前端参数一致，不得单方面改动 |

## 5. 数据流

    ① 麦克风（I2S/PDM）
         ↓ 采集线程（独立线程，优先级高于 UI）
    ② 环形缓冲（20~30 ms PCM 帧）
         ↓
    ③ 前端：预加重 → 分帧加窗 → FFT → Mel → log → DCT → MFCC
         ↓ 滑窗特征（帧数×特征维，与训练一致）
    ④ TFLM int8 推理（apps/mlearning/tflite-micro）
         ↓ 每类得分
    ⑤ 后处理：置信度阈值 + 连续命中计数 + 平滑
         ↓
    ⑥ 分流
         ├ 唤醒词 → 本地状态机：lv_async_call 更新 UI + 开麦/启动会话
         ├ 命令词 → 本地工具直执行（set_light / set_ac 等，零 token）
         └ 需要理解 → smart_home_agent_run_service_submit(文本)  ※见第 6 节
    ⑦ voice_play_text() → 语音反馈（后端可仍为云端 TTS）

实时性预算：帧长 30 ms 时，每帧必须在 30 ms 内完成 ③④⑤，且不能被其他
线程长时间抢占。

## 6. 与 cAGENT 的接入

唤醒词路径不经过 cAGENT：只触发本地状态机，不修改 cAGENT，也不产生模型调用。

命令词路径优先本地执行：开灯这类确定性映射直接调用本地工具函数（与 agent
工具共用实现），不走 LLM，零延迟、零 token、行为可复现。

提交会话时的已知限制（必须注意）。demos/smart_home/src/agent/
smart_home_agent_run_service.c 的 submit() 为单槽实现：

    if (service->queued || strcmp(service->status, "running") == 0)
        return AGENT_ERROR_BUSY;      /* 单槽：忙时直接拒绝 */

并且 worker 调用 smart_home_agent_run(app, input, ...) 未透传
conversation_id（agent_request_t.session_id 存在但未被使用）。

对本方案的影响：语音触发的会话提交在用户正在对话时会被拒绝，调用方必须
处理 AGENT_ERROR_BUSY 并给出本地提示，不能静默丢弃；若将来需要语音事件
与文本对话隔离上下文，需要把 session_id 透传到 agent_run，属应用层改造
（与目标检测方案共用同一改造）。

## 7. 配置方向（拟新增，当前不存在）

    CONFIG_SMART_HOME_VOICE=y
    CONFIG_SMART_HOME_VOICE_KWS=y
    CONFIG_SMART_HOME_VOICE_MODEL_PATH=/data/models/kws_int8.tflite
    CONFIG_SMART_HOME_VOICE_THRESHOLD=70
    CONFIG_SMART_HOME_VOICE_CONSECUTIVE_HITS=3
    CONFIG_SMART_HOME_VOICE_THREAD_STACKSIZE=8192
    CONFIG_SMART_HOME_MIC_I2S=y

以上符号名是本方案拟新增的命名方向，不代表已实现。

## 8. 分阶段实施与验收

K0 硬件与基线：确认麦克风型号、接线、I2S/PDM 实例；打通采集并保存 PCM。
验收：录到可播放的 PCM；采样率/位宽/通道数与训练前端参数一致。

K1 前端移植与对拍：移植 feature_extract，与参考实现对同一段录音求 MFCC
并逐值比对。验收：特征差异在可接受范围，否则不得进入 K2。

K2 推理接入与性能量化：接入 TFLM 与现有 int8 模型，输出类别与置信度。
验收：记录单帧推理耗时（均值/P95）与 arena 峰值，给出实时余量结论。

K3 应用接入：替换 voice 桩；唤醒与命令分流；UI 反馈。
验收：说命令词后对应设备状态改变；日志可见命中类别、置信度与耗时。

K4 联动与共存：与 agent 会话联动；与摄像头/显示共存测试。
验收：video_test 300 运行期间 KWS 不丢帧、不误触发。

## 9. 风险和定位

| 现象 | 优先检查 | 禁止的误判 |
| --- | --- | --- |
| 推理太慢 | 算子优化是否存在、arena 放置、实测耗时 | 直接换更小模型而不先量数据 |
| 唤醒率低 | 麦克风增益、采样率、前端参数与训练是否一致 | 先怀疑模型质量 |
| 误唤醒高 | 阈值、连续命中数、环境噪声档次 | 直接抬高阈值掩盖前端问题 |
| 与摄像头共存时丢音 | 线程优先级、内存带宽、DMA 冲突 | 归因于 KWS 模型 |
| 命令词命中但设备无反应 | 本地工具映射、BUSY 是否被处理 | 归因于语音识别 |

## 10. 提交与验收纪律

| 阶段 | 最低验证 |
| --- | --- |
| K0 | PCM 采集与回放成功（实板日志或文件） |
| K1 | 特征对拍一致（含对比数据） |
| K2 | 推理耗时与精度数据（含模型版本） |
| K3 | 命令词实板触发成功日志 |
| K4 | 与 video_test 共存复测成功 |

## 11. 待决问题

1. P4X 板的麦克风硬件是什么（板载或外接、I2S 或 PDM、哪个实例）？
2. 唤醒词是什么、由谁训练、需要多少正负样本？
3. 是否为 P4 移植 esp-nn，或放开 tflite-micro 的 S3 依赖？这是性能关键。
4. 是否评估 ESP-SR（WakeNet/MultiNet）？当前无证据表明其支持 P4，
   需先核实，不得在方案中假定可用。
5. 命令词的本地映射表由谁维护（工具层还是 skill 层）？

## 12. 维护记录

| 日期 | 内容 |
| --- | --- |
| 2026-09 | 建立文档：确认 voice 桩接口为扩展点；确认 ccf_audioevent 可复用
资产（含 8 类模型已含关键词类）；记录 ESP-NN 在 P4 不可用这一性能前提；
给出 K0~K4 阶段与验收判据。 |
