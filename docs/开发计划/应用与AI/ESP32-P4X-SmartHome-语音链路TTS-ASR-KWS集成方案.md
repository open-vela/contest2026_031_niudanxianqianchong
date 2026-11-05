# ESP32-P4X SmartHome 语音链路集成方案（TTS + ASR + KWS）

> 状态：三段代码均在主线。ASR/TTS 在 `smart_home_voice` 配置开启并已板端跑通
> 短句；KWS 常驻（`CONFIG_SMART_HOME_KWS`）尚未在任何 defconfig 开启，
> 因此"唤醒 → 自动 PTT → 播报"的完整链路只有分段证据，没有一次跑通的板端记录。
> 目标：开启三开关时，用户通过唤醒词唤醒 agent，用语音对话，回复自动播报。

## 1. 链路与模块

```text
KWS 常驻监听 ──唤醒──▶ 跳转聊天页 + 自动 PTT 录音
                                   │
                     voice_capture（独占 /dev/audio/pcm_in0）
                                   │ 点击结束 / ASR worker
                     MiMo-V2.5-ASR（base64 WAV → chat completions）
                                   │ 转写文本自动发送
                     cAGENT（LLM + 米家工具）
                                   │ 回复气泡
                     MiMo TTS（chat 形态 base64 WAV / speech 形态）
                                   │ ES8311 播放期间 KWS 自动暂停
                     回到常驻监听
```

| 模块 | 文件 | 职责 |
| --- | --- | --- |
| 采集服务 | `src/voice/smart_home_voice_capture.c` | 独占录音设备，单消费者让位（ASR 会话 ↔ KWS 监听） |
| TTS | `src/voice/smart_home_tts.c` + `voice_player.c` + `voice_play.c` | 云端合成（复用 cAGENT TLS）、PCM16 播放、队列化播报 |
| ASR | `src/voice/smart_home_asr.c` | MiMo-V2.5-ASR，大字段手工拼接防内存四倍峰值 |
| KWS | `src/voice/smart_home_kws_service.c` + `kws/` | wake_large int8 常驻唤醒（TFLite reference kernel） |
| 会话联动 | `src/ui/lvgl/smart_home_lvgl_voice_session.c` | 唤醒→对话→播报状态机，pending 队列回投 LVGL |

## 2. 开关与裁剪

编译期（Kconfig，全部默认 n，主配置 `smart_home` 不开启=零影响）：

- `SMART_HOME_VOICE_TTS`（+`_TTS_SMOKE`）：仅文字聊天时不需要
- `SMART_HOME_VOICE_ASR`（+`_ASR_SMOKE`）：PTT 语音输入
- `SMART_HOME_KWS`（+`_KWS_SMOKE`）：常驻唤醒，引入 C++/TFLite 依赖
- 演示配置 `configs/smart_home_voice/`：三开 + TFLite Micro 全家桶

运行期（设置页"语音"卡，持久化）：

- 播报开关 → `voice.json`（asr 关闭时 TTS 不播报）
- 语音输入开关 + 语言 → `asr.json`
- 唤醒开关 → `kws.json`（boot 决定是否启动；运行期切换=暂停/恢复）

密钥全部复用 `secrets.json` 的 `model_api_keys.mimo`（LLM/TTS/ASR 同 key）。

## 3. 真机验证路径（按序）

```text
nsh> tts_smoke play <pcm文件> 16000      # 播放基线（audio_smoke record 可生成）
nsh> tts_smoke speak 你好，我是智能家居管家  # 云端合成全链路
nsh> asr_smoke record 4                   # 采集基线
nsh> asr_smoke recognize 4                # 云端识别全链路
nsh> kws_smoke --repeat 20                # 模型/算子/Invoke 耗时基准
```

逐项通过后再验证 UI：PTT 按钮 → 唤醒词 → 三开组合演示。

## 4. 已知风险（诚实清单）

1. **TTS/ASR 云端请求真机未验证**：走 cAGENT 生产 TLS 运行时（LLM 聊天在用），
   风险低于 ai_agent 独立栈，但仍须冒烟先行——这是 ai_agent 蓝屏教训的对策。
2. **KWS 特征前端未经训练管线对拍**：`wake_large_deploy.tar.gz` 不含前端源码
   与测试 PCM，当前常数为行业默认（宿主已验证 FFT/mel 数学正确）。真机唤醒率
   在对拍通过前不可解释；PTT 按钮是保底交互。
3. **PSRAM 预算**：TTS 1MB + ASR 峰值约 750KB（瞬时）+ KWS arena 192KB +
   2.5s 环 81KB，错峰可容纳，需实测峰值。
4. **KWS reference kernel 耗时未知**：默认 500ms 推理间隔起步，
   `kws_smoke` 实测后可收紧。

## 5. 构建命令

```bash
# 全关回归（与主线行为一致）
contest2026_031_niudanxianqianchong/scripts/build_p4x.sh smart_home -j8

# 语音演示（三开）
contest2026_031_niudanxianqianchong/scripts/build_p4x.sh smart_home_voice -j8
```

注意：Kconfig 与 Makefile 有变更，首次构建前建议 `make clean` 全量重编
（陈旧对象教训）。
