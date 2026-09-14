# ESP32-P4X Smart Home 端侧目标检测方案

> 状态：方案阶段。当前 smart_home 没有视觉模块；/dev/video0 已通过真机
> 连续取帧验收（video_test 300，app_fps=30.02，sequence_gaps=0）；
> cAGENT 的工具契约已支持逐工具超时与动态启停。本方案只描述设计与验收
> 判据，不代表功能已实现。
>
> 适用对象：ESP32-P4X Function EV Board、SC2336 摄像头、Smart Home。
>
> 相关文档：ESP32-P4X-端侧KWS方案.md（同目录）、
> ../../硬件适配/ESP32-P4X-SC2336-视频通路架构与调用链.md、
> ../网络与连接/ESP32-P4X-SmartHome板载C6-WiFi接入方案.md。

## 1. 目标

在 ESP32-P4 上实现端侧目标检测，并接入 smart_home 应用层，提供两种能力：

| 范式 | 触发方 | 典型场景 | 实现路径 |
| --- | --- | --- | --- |
| 工具（被动） | LLM 在 ReAct turn 内决定调用 | 用户问现在客厅有人吗 | agent 工具 |
| 事件（主动） | 设备在状态跃迁时发起 | 有人进门就主动响应 | 事件队列 + 提交会话 |

必须先划清这两条路径：cAGENT 没有定时器或轮询机制，工具不会自我触发，
因此持续监测只能用事件路径实现，工具只能回答按需查询。

## 2. 现状与可复用资产

| 资产 | 位置 | 状态 |
| --- | --- | --- |
| 视觉输入 | /dev/video0，RGB565 1024x600 | 真机 300 帧验收通过 |
| 视频通路 | SC2336 → CSI → ISP → GDMA → V4L2 三缓冲 | 已验证 |
| 推理运行时 | apps/mlearning/tflite-micro | 在树中可用 |
| 工具契约 | packages/cAGENT/include/cagent/tools.h | 已支持 timeout_ms 与 flags |
| 硬件类工具先例 | demos/smart_home/src/tools/smart_home_tools.c 的 indoor_environment_tool | 已在工具内读传感器 |
| 显示通路 | /dev/fb0（1024x600 RGB565）+ LVGL | 已验证 |

### 2.1 已知性能前提（必须先解决）

| 事实 | 位置 | 影响 |
| --- | --- | --- |
| TFLITEMICRO_ESP_NN 依赖 ARCH_CHIP_ESP32S3 | apps/mlearning/tflite-micro/Kconfig | P4 当前无法使用 ESP-NN 优化算子 |
| apps/mlearning/esp-nn/ 目录为空 | apps/mlearning/ | 优化算子源码未就位 |
| P4 无专用 NPU，双核 400 MHz | 硬件事实 | 检测只能按低帧率设计 |
| 摄像头通路已占约 111 MB/s 内存流量 | 视频通路文档 | 推理与视频争 PSRAM 带宽 |

结论：检测必须按低帧率（有效输出 1~2 FPS 量级）设计，并采用跳帧策略；
不得按 30 FPS 规划。

### 2.2 工具契约已经具备的能力（证据）

    typedef struct {
        const char *name;
        const char *description;         /* 暴露给模型 */
        const char *input_schema_json;   /* JSON Schema */
        agent_tool_fn execute;
        uint32_t flags;                  /* REQUIRES_CONFIRM / DISABLED / PARALLEL_SAFE */
        uint32_t timeout_ms;             /* 单工具超时，0 = 用 limits.per_tool_timeout_ms */
    } agent_tool_t;

另有 agent_register_tool_simple、agent_tool_set_enabled、
agent_register_tool、agent_unregister_tool，以及 agent_limits_t 中的
per_tool_timeout_ms。说明框架明确预期工具会耗时并可能失败。

## 3. 范围和非目标

本期包含：vision 服务生命周期；采集与预处理；推理与后处理（NMS、阈值、
去抖）；结果缓存与查询接口；工具实现（读缓存与按需推理两种形态）；
主动事件产生与分发；UI 叠加显示；与 cAGENT 的集成。

本期不包含：模型训练与调参；多模态视觉大模型上传；云端检测服务；
多路摄像头；目标跟踪与 ReID；在未验收前宣称 mAP 或识别率指标。

## 4. 分层设计

### 4.1 应用层（demos/smart_home/src/vision/）

| 文件 | 动作 | 职责 |
| --- | --- | --- |
| smart_home_vision.h / .c | 新增 | 服务生命周期、共享状态、请求队列 |
| smart_home_vision_capture.c | 新增 | 打开 /dev/video0，DQBUF → 推理 → QBUF 循环 |
| smart_home_vision_preprocess.c | 新增 | RGB565 1024x600 到模型输入；跳帧策略 |
| smart_home_vision_infer.c | 新增 | TFLM 解释器、tensor arena、输出解析 |
| smart_home_vision_event.c | 新增 | 去抖、状态跃迁、事件环形缓存与订阅 |
| Kconfig / CMakeLists.txt | 新增 | 开关、模型、输入尺寸、最大 FPS、阈值 |

### 4.2 与摄像头驱动的关系（关键约束）

vision 服务是 /dev/video0 的唯一流式使用者。V4L2 三缓冲由采集线程持有
（DQBUF 与 QBUF 循环），CSI 为单例且缓冲环带有背压逻辑（详见视频通路
架构文档）。

因此：

- 工具不得自行 open /dev/video0 或 DQBUF，否则与流式采集争抢缓冲，
  会触发 dma_paused 背压路径导致视频卡顿；
- 工具读取结果只有两条合法途径：读共享的最新结果（形态 A），或向
  vision 线程发起请求并等待应答（形态 B）；
- 与 csi_probe 诊断程序的互斥关系沿用现有 Kconfig 约定。

### 4.3 模型与资源

| 项 | 方案 |
| --- | --- |
| 模型格式 | int8 TFLite 优先 |
| 输入尺寸 | 160x160 或 224x224（按实测耗时才决定） |
| 权重放置 | PSRAM |
| tensor arena | 优先内部 SRAM |
| 模型资源 | LittleFS 资源或内嵌 C 数组 |

## 5. 数据流

    SC2336 → CSI Host → ISP(RGB565) → Bridge → GDMA → V4L2 三缓冲
        ↓
    /dev/video0（RGB565 1024x600 = 1,228,800 B）
        ↓ vision 采集线程 DQBUF（独立线程，不复用摄像头 HPWORK 帧 worker）
    ① 取到帧
        ↓ 跳帧策略：例如每 5 帧取 1 帧
    ② 预处理：裁剪/缩放至模型输入、RGB565 转 RGB888、量化
        ↓
    ③ TFLM int8 推理
        ↓
    ④ 后处理：置信度过滤 → NMS → 类别框
        ↓
    ⑤ 去抖与聚合：连续 N 帧命中或状态跃迁（无 → 有）
        ↓
    ⑥ 分发
        ├ UI：lv_async_call 投递，画框或更新状态
        ├ 事件缓存：供 get_vision_state 工具读取
        └ 状态跃迁：提交会话（受第 6.4 节限制）或本地规则直执行
        ↓
    ⑦ QBUF 归还缓冲

线程与优先级要求：推理线程优先级低于摄像头帧 worker；宁可检测掉帧，
不可让视频流卡顿。

## 6. 与 cAGENT 的集成

### 6.1 工具与事件的边界

工具在 turn 内被 LLM 调用，适合回答按需查询；事件由设备发起，适合主动响应。
两者互补，不可互相替代。

### 6.2 三种工具形态

| 形态 | 工具名示例 | 行为 | 阻塞 | 定位 |
| --- | --- | --- | --- | --- |
| A 读缓存状态 | get_vision_state | 读 vision 线程维护的最新去抖结果 | 微秒级 | 默认首选 |
| B 按需推理 | detect_objects | 请求 vision 线程立即推理一次，带超时 | 0.3~2 s | 需要最新一帧时 |
| C 抓拍留档 | capture_snapshot | 存一帧到 LittleFS 并返回路径 | 约百毫秒 | 配合云端或多模态 |

推荐默认组合：以形态 A 为主工具，形态 B 作兜底，主动响应走事件路径。

### 6.3 工具实现约束

1. 缓冲所有权：不得自持 /dev/video0 或 DQBUF（见 4.2）。
2. 必须声明 timeout_ms：建议形态 B 设 1500~2000 ms，超时返回明确错误 JSON，
   而不是让整个 turn 超时。
3. 结果必须去抖稳定化：单帧结果抖动会导致模型行为不可预测，应返回
   连续命中或状态机稳定后的状态。
4. 返回文本 JSON 而非图像：cAGENT 是文本 ReAct，模型无法消费二进制帧。
5. 视觉不可用时禁用工具：使用 agent_tool_set_enabled 或
   AGENT_TOOL_FLAG_DISABLED，避免模型调用必然失败的工具。
6. 并发安全：工具运行在 agent worker 线程，vision 运行在自己的线程，
   共享结果需加锁或双缓冲加原子指针交换；只读缓存的工具可标
   AGENT_TOOL_FLAG_PARALLEL_SAFE。
7. 成本意识：每次工具调用多一轮 LLM 往返，持续监测不得用工具轮询实现。

### 6.4 主动事件所需的改造（应用层，非 cAGENT 核心）

cAGENT 核心已具备 agent_request_t.session_id、agent_cancel、
agent_set_limits、agent_get_stats、agent_tool_set_enabled、
agent_set_event_callback，无需修改。真正需要改造的是 smart_home 的运行服务：

    /* demos/smart_home/src/agent/smart_home_agent_run_service.c，submit() */
    if (service->queued || strcmp(service->status, "running") == 0)
        return AGENT_ERROR_BUSY;      /* 单槽：忙时直接拒绝 */

并且 worker 调用 smart_home_agent_run(app, input, ...) 未透传 conversation_id。

改造项（按优先级）：

| 序 | 改造 | 理由 |
| --- | --- | --- |
| 1 | 单槽改为环形队列，用户 turn 优先 | 否则用户对话期间的检测事件被静默丢弃 |
| 2 | 事件去抖、合并、过期丢弃 | 检测稳态频率远高于 LLM 调用速率 |
| 3 | session_id 透传到 agent_run | 事件走独立会话，避免污染 chat 上下文 |
| 4 | 限流与预算（agent_set_limits、agent_get_stats） | 控制主动事件带来的 token 成本 |
| 5 | 过期事件取消（agent_cancel） | 人已离开时不应继续执行过期动作 |
| 6 | 本地优先：确定性规则不走 LLM | 有人开灯一类联动零 token、零延迟 |

事件输入的语义：cAGENT 请求只有 input 文本。先用约定前缀（例如
[event] person_detected count=1）加 res/skills 中的策略说明实现，属零改动
路径；只有在出现真实语义歧义时才考虑扩展 cAGENT 的请求结构。

## 7. 资源与性能预算

| 项 | 现状或要求 |
| --- | --- |
| 摄像头内存流量 | 约 111 MB/s（含每帧 1.2 MB memcpy） |
| 显示帧缓冲 | 2457600 字节（已占用） |
| 推理线程优先级 | 低于摄像头帧 worker |
| 有效检测帧率 | 目标 1~2 FPS，须实测确认 |
| 跳帧策略 | 预处理前跳帧，避免浪费带宽与 CPU |

## 8. 配置方向（拟新增，当前不存在）

    CONFIG_SMART_HOME_VISION=y
    CONFIG_SMART_HOME_VISION_MODEL_PATH=/data/models/detect_int8.tflite
    CONFIG_SMART_HOME_VISION_INPUT_SIZE=160
    CONFIG_SMART_HOME_VISION_MAX_FPS=2
    CONFIG_SMART_HOME_VISION_SCORE_THRESHOLD=50
    CONFIG_SMART_HOME_VISION_CONSECUTIVE_HITS=2
    CONFIG_SMART_HOME_VISION_THREAD_STACKSIZE=16384
    CONFIG_SMART_HOME_VISION_EVENTS=y

以上符号名是本方案拟新增的命名方向，不代表已实现。

## 9. 分阶段实施与验收

D0 模型与性能基线：选定 int8 模型；在 P4 上量出单帧推理耗时与 arena 峰值。
验收：给出耗时均值/P95 与可行的最大 FPS 结论。

D1 视觉服务：采集线程打通，取帧与预处理正确（可先不做推理）。
验收：连续取帧 N 分钟无卡顿，video_test 300 仍通过。

D2 推理与事件：接入推理、后处理、去抖与事件缓存。
验收：日志可见检测结果与事件跃迁；事件频率受控。

D3 工具接入：实现 get_vision_state 与 detect_objects；注册并设超时与 flags。
验收：通过对话触发工具调用并返回 JSON；视觉不可用时工具被禁用。

D4 主动响应与共存：run_service 队列改造；本地规则联动；与摄像头、显示、
Wi-Fi 共存测试。
验收：状态跃迁触发一次受控的主动响应；video_test 300 与检测并存不崩。

## 10. 风险和定位

| 现象 | 优先检查 | 禁止的误判 |
| --- | --- | --- |
| 视频卡顿或丢帧 | 线程优先级、跳帧策略、内存带宽 | 归因于摄像头驱动回归 |
| 检测帧率过低 | 算子优化、输入尺寸、预处理开销 | 直接判定模型不可用 |
| 结果忽有忽无 | 去抖参数、光照、曝光 | 归因于模型精度 |
| 用户对话期间事件丢失 | run_service 单槽 BUSY | 归因于网络或 LLM 后端 |
| 主动响应过于频繁 | 事件合并与限流 | 提高模型阈值掩盖 |
| 工具调用超时 | timeout_ms、vision 请求队列积压 | 归因于模型后端 |

## 11. 提交与验收纪律

| 阶段 | 最低验证 |
| --- | --- |
| D0 | 推理耗时与精度数据（含模型版本与输入尺寸） |
| D1 | 长时间取帧稳定，video_test 300 通过 |
| D2 | 检测事件日志，事件频率符合预期 |
| D3 | 工具调用返回 JSON 的实机日志 |
| D4 | 主动响应与 video_test 共存复测成功 |

## 12. 待决问题

1. 检测的类别集与验收标准（比赛评分点是类别数、精度还是响应速度）？
2. 是否为 P4 移植 esp-nn，或放开 tflite-micro 的 S3 依赖（与 KWS 方案共用）？
3. 模型由谁训练、是否需要自定义数据集？
4. 主动响应走独立会话还是主会话（影响上下文预算与用户体验）？
5. 是否需要多模态视觉大模型路线（抓帧上传），作为检测的补充？

## 13. 维护记录

| 日期 | 内容 |
| --- | --- |
| 2026-09 | 建立文档：确认 /dev/video0 为唯一视频输入且已验收；确定工具与
事件两种范式；记录工具契约的 timeout_ms 与动态启停能力；列出 run_service
单槽改造项；给出 D0~D4 阶段与验收判据。 |
