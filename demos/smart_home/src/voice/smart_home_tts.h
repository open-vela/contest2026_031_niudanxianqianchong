/****************************************************************************
 * demos/smart_home/src/voice/smart_home_tts.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2026 The NuttX Contributors
 *
 ****************************************************************************/

/* 云端 TTS 合成客户端。
 *
 * 后端预设与 LLM 预设（smart_home_backends）同构但独立成表：端点、
 * 默认模型与音色不同。首选 MiMo（chat completions + base64 wav 形态，
 * 与 LLM mimo 预设同 host，key 独立配置）；siliconflow/dashscope/openai
 * 为 OpenAI 兼容 /v1/audio/speech 二进制形态备选，custom 供自定义端点。
 *
 * API key 使用 secrets.json 的 tts_api_keys 对象，按 backend_id 查询，
 * 与 LLM 后端的凭据管理保持同一套约定。
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include "smart_home_voice_result.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SMART_HOME_TTS_BACKEND_ID_MAX 32
#define SMART_HOME_TTS_HOST_MAX       128
#define SMART_HOME_TTS_PATH_MAX       128
#define SMART_HOME_TTS_PORT_MAX       8
#define SMART_HOME_TTS_MODEL_MAX      64
#define SMART_HOME_TTS_VOICE_MAX      64

/* TTS 后端的请求/响应形态。
 *
 * SPEECH：OpenAI 兼容 /v1/audio/speech，请求 {"model","input","voice"}，
 *         响应体直接是 WAV 二进制流（siliconflow/dashscope/openai）。
 * CHAT_COMPLETIONS：MiMo 形态，复用 chat completions（合成文本放
 *         role:assistant 消息，audio={"format","voice"}），响应为
 *         JSON 内嵌 base64 WAV（choices[0].message.audio.data）。 */
typedef enum
{
  SMART_HOME_TTS_API_SPEECH = 0,
  SMART_HOME_TTS_API_CHAT_COMPLETIONS,
} smart_home_tts_api_style_t;

typedef struct
{
  int enabled;                       /* 语音播报开关（持久化） */
  char backend_id[SMART_HOME_TTS_BACKEND_ID_MAX];
  char host[SMART_HOME_TTS_HOST_MAX];
  char path[SMART_HOME_TTS_PATH_MAX];
  char port[SMART_HOME_TTS_PORT_MAX];
  char model[SMART_HOME_TTS_MODEL_MAX];
  char voice[SMART_HOME_TTS_VOICE_MAX];
  uint32_t sample_rate;              /* 非 WAV 响应时的兜底采样率 */
} smart_home_tts_config_t;

/* TTS 后端预设条目（与 g_llm_presets 同构）。 */
typedef struct
{
  const char *id;
  const char *name;
  const char *host;
  const char *path;
  const char *port;
  const char *default_model;
  const char *default_voice;
  int api_style;                    /* smart_home_tts_api_style_t */
} smart_home_tts_preset_t;

size_t smart_home_tts_preset_count(void);
const smart_home_tts_preset_t *smart_home_tts_preset_get(size_t index);
const smart_home_tts_preset_t *smart_home_tts_preset_by_id(const char *id);

/* 载入 voice.json；文件缺失或损坏时恢复内存默认（enabled=0，mimo）。 */
void smart_home_tts_config_load(smart_home_tts_config_t *config);

/* 保存 voice.json（原子写）。仅设置页调用。 */
int smart_home_tts_config_save(const smart_home_tts_config_t *config);

/* 当前生效配置的只读快照（内部互斥保护）。 */
void smart_home_tts_config_get(smart_home_tts_config_t *out);
int smart_home_tts_config_set(const smart_home_tts_config_t *config);

/* 合成一段文本。
 *
 * text:     UTF-8 文本
 * out_pcm:  成功时输出堆缓冲（smart_home_bulk_alloc 分配，调用者
 *           smart_home_bulk_free 释放），内容为 WAV 去壳后的
 *           PCM16 little-endian 单声道数据（24 kHz 响应会抗混叠重采样到 16 kHz）
 * out_bytes/out_rate: PCM 字节数与采样率
 *
 * 返回 0 成功；-ENOKEY 之类不另造码，统一 AGENT_ERROR_* 或 errno 风格
 * 负值，错误原因打印到 stderr/日志。
 */
int smart_home_tts_synth(const char *text,
                         uint8_t **out_pcm, size_t *out_bytes,
                         uint32_t *out_rate);

/* Same ownership as synth(), with stage/status for UI and diagnostics. */
int smart_home_tts_synth_ex(const char *text, uint8_t **out_pcm,
                           size_t *out_bytes, uint32_t *out_rate,
                           smart_home_voice_result_t *result);

#ifdef __cplusplus
}
#endif
