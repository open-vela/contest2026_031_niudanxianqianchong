# ESP32-P4X SmartHome 语音播报（TTS）接入方案

> 状态：代码已在主线，`smart_home_voice` 配置开启 `CONFIG_SMART_HOME_VOICE_TTS=y`。
> 短句（"你好"）已通过板端听感验收；约 11 个字符的长句两次请求收到 HTTP 200
> 后正文接收不完整，返回 `AGENT_ERROR_TIMEOUT`，根因未定位。24 kHz → 16 kHz
> 重采样只有宿主测试，无板端音质与耗时数据。本文记录四层架构、配置约定、
> 各后端 API 形态差异与验收清单。
>
> 适用目标：ESP32-P4 Function EV Board、OpenVela/NuttX、ES8311 扬声器、
> smart_home LVGL 聊天页。

## 1. 目标与范围

Agent 回复完成后，通过 ES8311 扬声器自动语音播报，设置页可开关。明确
不做：本地 TTS 引擎（P4 上无可用轻量中文 TTS）、流式分块播放（首版整段
下载后播放）、ASR 采集（`src/voice/smart_home_voice_stub.h` 预留的另一侧）。

## 2. 四层架构

```text
聊天页 smart_home_lvgl_agent.c   agent 回复成功 → 开关开启时投递文本
  → voice_play_text(text)                    （stub 预留签名）
    → smart_home_voice_play.c                独立低优先级 worker + 队列 + 打断
      → smart_home_tts.c                     云端合成 + WAV 解析 + voice.json 配置
      → smart_home_voice_player.c            /dev/audio/pcm0 任意采样率 PCM16 播放
```

| 层 | 文件（`demos/smart_home/src/voice/`） | 要点 |
| --- | --- | --- |
| 播放器 | `smart_home_voice_player.c/h` | RESERVE/CONFIGURE/缓冲队列/AUDIOIOC_START 循环，参照 audio_smoke 已验证路径；支持跨线程中止（-EINTR） |
| 合成 | `smart_home_tts.c/h` | 双形态：speech（OpenAI 兼容 `/v1/audio/speech` 二进制 WAV）/ chat（MiMo：chat completions + JSON/base64 WAV）；预设表 + voice.json（config store 原子写）+ secrets.json 密钥（按 backend_id 复用 model_api_keys） |
| 服务 | `smart_home_voice_play.c/h` | PSRAM 栈 worker（32 KiB，mbedTLS 合成需要）、FIFO 队列、`voice_play_stop()` 打断并清队、`voice_play_state()` 三态（idle/synthesizing/playing） |
| UI | settings 卡片 + 聊天钩子 | 设置页"语音播报"卡片：开关（持久化）/后端状态/停止按钮；聊天页在 agent 回复气泡后触发 |

内存约定：合成响应缓冲走 `smart_home_bulk_alloc`（PSRAM），上限
`CONFIG_SMART_HOME_TTS_AUDIO_MAX_BYTES`（默认 1 MiB；speech 形态 ≈
32 s@16 kHz，chat 形态受 base64 膨胀 ≈ 24 s@16 kHz，见 4.2）。

## 3. 配置

```text
/data/smart_home/voice.json    # 开关 + 端点 + 模型 + 音色（config store 管理）
/data/smart_home/secrets.json  # model_api_keys.<backend_id> 的 API key
```

`voice.json` 示例（默认即 MiMo 后端，与 LLM `mimo` 预设同 host、同 key）：

```json
{
  "version": 1,
  "enabled": true,
  "backend_id": "mimo",
  "host": "api.xiaomimimo.com",
  "path": "/v1/chat/completions",
  "port": "443",
  "model": "mimo-v2.5-tts",
  "voice": "mimo_default",
  "sample_rate": 16000
}
```

设置页卡片切换开关会按默认工厂生成/更新该文件；端点字段可手工编辑
（后续可加下拉）。内置预设：`mimo`（**默认**）、`siliconflow`、
`dashscope`、`openai`、`custom`。MiMo 音色可换 冰糖/茉莉/苏打/白桦/
Mia/Chloe/Milo/Dean。

## 4. 后端 API 形态与 MiMo 可行性

### 4.1 当前实现支持的形态（speech 形态）

OpenAI 兼容 `/v1/audio/speech`：POST JSON
`{"model","input","voice","response_format":"wav"}`，**响应体直接是 WAV
二进制流**。siliconflow / dashscope compatible-mode / openai 均为此形态。

### 4.2 MiMo TTS：**已选定并实现**（chat 形态适配）

依据官方文档（mimo.mi.com，speech-synthesis-v2.5，2026-09 查证）：

| 项目 | MiMo 实际形态 | 实现方式 |
| --- | --- | --- |
| 端点 | `https://api.xiaomimimo.com/v1` + **chat completions 形式**（合成文本放 `role:assistant` 消息，`audio={"format":"wav","voice":...}`） | `tts_build_request()` 按 `api_style=CHAT_COMPLETIONS` 构造 messages+audio |
| 认证 | Bearer（同 OpenAI 风格） | 复用现有 auth 头拼装 |
| 响应（非流式） | **JSON 内嵌 base64 WAV**（`choices[0].message.audio.data`） | `tts_decode_chat_response()`：cJSON 解析 → `mbedtls_base64_decode`（mbedTLS 已随 cAGENT 链接）→ 解码回写响应缓冲 → 复用 WAV 解析 |
| 音频 | 非流式 wav；流式 pcm16 24 kHz mono | WAV 解析器直接复用；流式暂不接 |
| 模型 | `mimo-v2.5-tts`（另有 voicedesign/voiceclone 变体） | 预设默认 `mimo-v2.5-tts` |
| 音色 | 冰糖/茉莉/苏打/白桦/Mia/Chloe/Milo/Dean/`mimo_default` | 预设默认 `mimo_default`，voice.json 可改 |
| 计费 | 限时免费（文档口径） | 比赛期验证友好 |

实现要点与约束：

- host 与现有 LLM `mimo` 预设相同（`api.xiaomimimo.com`），secrets.json
  的 `model_api_keys.mimo` **直接复用**，无需新凭据；
- `api_style` 由 backend_id 经预设表推导（mimo → chat，其余 → speech），
  voice.json 不额外存形态字段；
- cJSON 会把 base64 字符串拷贝到堆节点，**解码峰值内存约为响应缓冲的
  2 倍**（1 MiB 缓冲 → ~2 MiB 峰值），P4 PSRAM 可承受；base64 膨胀使
  有效音频上限约为缓冲的 3/4（默认 1 MiB → ~768 KiB ≈ 24 s@16 kHz）；
- 流式 pcm16 24 kHz 分片暂不接，与非流式 wav 同走整段缓冲路径。

### 4.3 后端选择（已定：MiMo 首选）

- **MiMo（首选，已实现）**：key 复用、限时免费、中文音色齐；
- **DashScope CosyVoice**：speech 形态零适配，需在 secrets 增加条目；
- **SiliconFlow / OpenAI**：speech 形态零适配，需各自注册 key。

## 5. 边界与纪律

- 播放器独占 `/dev/audio/pcm0`；与 `audio_smoke` 不可同时使用，与未来
  KWS 采集共存依赖全双工 GDMA（已具备），但**无 AEC**：常驻 KWS 落地后
  播报期间需挂起唤醒判定，钩子在 `voice_play_state()` 基础上补；
- 合成与播放均在 worker 线程，LVGL 线程零阻塞；UI 线程只调用
  `voice_play_text/stop/state`；
- 失败静默降级：合成失败仅 stderr 日志，不影响聊天气泡展示；
- 密钥只存 secrets.json，voice.json 不含密钥，均不入 Git。

## 6. 构建与验收（用户执行）

构建前提：工作区 `packages/demos/smart_home` 符号链接需临时改指 tts
worktree，build.sh 传 worktree 的 board 路径（见交接说明）。验收顺序：

1. `tts_smoke play /data/es8311_16k_mono.pcm 16000`：出声、打印
   `play complete`（无网络依赖的播放器基线）；
2. 设置页"语音播报"开关：`/data/smart_home/voice.json` 生成且 `enabled`
   正确；关闭时正在播报的内容立即停止；
3. `tts_smoke speak <文本>`：打印 `synth ok: bytes=... rate=...` 并播报
   （需 voice.json 端点 + secrets key）；
4. 聊天页一问一答：开关开启时回复自动播报，关闭时不播报；
5. 保留完整串口日志（含 `[voice]`/`tts:` 前缀行）与固件 commit。

验证通过后按仓库纪律单提交落盘：`feat(voice): 接入 Agent 回复语音播报`。

## 7. 后续事项

- [x] MiMo chat 形态适配（4.2，已实现，待真机验证）；
- [ ] 设置卡后端下拉选择（替代手工编辑 voice.json）；
- [ ] 聊天页"播报中"实时角标（`voice_play_state()` + lv_timer）；
- [ ] 流式分块合成播放（降低首音延迟）；
- [ ] 与 KWS 采集共存的自声抑制策略（播放期挂起唤醒判定）。

## 8. 关联资料

- `demos/smart_home/src/voice/`：四层实现（本分支）；
- `docs/开发计划/应用与AI/ESP32-P4X-SmartHome-wake_large唤醒词模型接入方案.md`：
  语音交互的采集/唤醒侧方案；
- MiMo TTS 文档：mimo.mi.com/docs/zh-CN/quick-start/usage-guide/audio/
  speech-synthesis-v2.5（2026-09 查证）。
