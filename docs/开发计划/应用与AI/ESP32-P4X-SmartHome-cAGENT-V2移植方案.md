# SmartHome cAGENT V2 移植方案

状态：已实施。V2 API 在 `packages/cAGENT/` 中，`demos/smart_home` 直接使用，
无 V1 兼容层。Host/ASan/UBSan 与五项业务回归通过，完整固件构建通过；
真机、语音与可选扩展的验收见
[语音与能力合入实施计划](ESP32-P4X-SmartHome语音与能力合入实施计划.md)。
移植起点：`8d344ee`。

## 范围

- 将 SmartHome 接入 `packages/cAGENT/` 的 V2 API，不增加 V1 兼容层。
- 保留设备服务、米家协议、页面布局、摄像头和音频驱动。
- 会话默认按 RAM 多轮方案规划，重启清空；持久化另行处理。
- 先恢复 P4 标准 SmartHome，再适配语音和可选扩展；构建使用 Make。

## 实施顺序

| 步骤 | 工作内容 |
| --- | --- |
| 1．生命周期 | 保留 `smart_home_agent_app_*` 业务入口，管理 Core、模型、Transport、TLS 和缓冲区；适配初始化、启动、销毁与失败清理。 |
| 2．模型 | 接入 V2 OpenAI Provider，保留后端切换及密钥加载；配置字符串由应用持有；缺少密钥时仍可进入设置页。 |
| 3．工具与上下文 | 迁移本地及米家工具、策略、枚举、设备上下文和 Skill；显式映射标志位，保留 `read_skill` 按需加载。 |
| 4．会话 | 接入有界 RAM Session Storage，配置历史轮数，支持完整回合保留和清空。 |
| 5．UI | 映射 V2 事件、错误及输出截断状态；回调数据复制后入队，恢复思考状态、工具轨迹和设备卡片刷新。 |
| 6．语音 | 将 TTS/ASR 的旧 `runtime.http_post` 调用迁入 V2 Transport/TLS；并发请求使用独立缓冲区。 |
| 7．扩展 | 分别适配 Node、MCP、App Bridge 和 QuickApp 的工具与状态接口。 |

## 验收

1. Host/mock：初始化与销毁、模型切换、工具成功/失败/拒绝、多轮及清空、容量不足与输出截断。
2. 构建：更新 Kconfig、Make 和容量项，分别通过 P4 `smart_home`、`smart_home_voice` 及目标扩展配置构建。
3. 真机：问答、米家控制、工具开关、模型切换、断网恢复、UI、语音及内存峰值。
4. 分别记录 Host、完整固件和真机结果，不以其中一项代替其他验收。

## Git 提交

- `refactor(smart_home): 将标准中控应用接入 V2 内核`
- `refactor(voice): 将语音网络调用迁移至 V2 传输层`
- `refactor(addons): 适配 V2 远程工具接口`

每组通过相应验收后提交；中途保存须标记阶段快照。原始演示视频不纳入迁移提交，`v0.1.0` 保持不变。
