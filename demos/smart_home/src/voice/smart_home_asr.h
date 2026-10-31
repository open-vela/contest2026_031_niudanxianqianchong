/****************************************************************************
 * demos/smart_home/src/voice/smart_home_asr.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2026 The NuttX Contributors
 *
 ****************************************************************************/

/* 云端语音识别客户端（OpenAI 兼容 chat completions 形态）。
 *
 * 接口：POST <host><path>，音频以 WAV base64 内嵌 user content 的
 * input_audio，语言 zh/en/auto 经顶层 asr_options 指定，转写文本在
 * choices[0].message.content。
 *
 * 后端预设与 TTS（smart_home_tts）同构：默认 MiMo（与 LLM/TTS 的 mimo
 * 预设同 host，key 直接复用 secrets.json 的 model_api_keys.<backend_id>），
 * custom 供自定义 OpenAI 兼容端点。端点配置持久化在
 * /data/smart_home/asr.json；密钥只放 secrets.json。
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SMART_HOME_ASR_LANGUAGE_MAX    8
#define SMART_HOME_ASR_BACKEND_ID_MAX  32
#define SMART_HOME_ASR_HOST_MAX        128
#define SMART_HOME_ASR_PATH_MAX        128
#define SMART_HOME_ASR_PORT_MAX        8
#define SMART_HOME_ASR_MODEL_MAX       64

typedef struct
{
  int enabled;                              /* 语音输入开关（持久化） */
  char backend_id[SMART_HOME_ASR_BACKEND_ID_MAX];
  char host[SMART_HOME_ASR_HOST_MAX];
  char path[SMART_HOME_ASR_PATH_MAX];
  char port[SMART_HOME_ASR_PORT_MAX];
  char model[SMART_HOME_ASR_MODEL_MAX];
  char language[SMART_HOME_ASR_LANGUAGE_MAX]; /* zh / en / auto */
} smart_home_asr_config_t;

/* ASR 后端预设条目（与 g_tts_presets 同构）。 */
typedef struct
{
  const char *id;
  const char *name;
  const char *host;
  const char *path;
  const char *port;
  const char *default_model;
} smart_home_asr_preset_t;

size_t smart_home_asr_preset_count(void);
const smart_home_asr_preset_t *smart_home_asr_preset_get(size_t index);
const smart_home_asr_preset_t *smart_home_asr_preset_by_id(const char *id);

/* 载入 asr.json；缺失或损坏时恢复默认（enabled=0, mimo 预设, language=zh）。
 * 旧版文件（仅 enabled/language，无端点字段）按 mimo 预设回填端点。 */
void smart_home_asr_config_load(smart_home_asr_config_t *config);

/* 保存 asr.json（原子写）。仅设置页调用。 */
int smart_home_asr_config_save(const smart_home_asr_config_t *config);

/* 识别一段 PCM16 单声道 16 kHz 录音。
 *
 * pcm/bytes: 录音数据（voice_capture_record 的产物）
 * out_text:  成功时写入 UTF-8 转写文本（截断到 out_size-1）
 *
 * 返回 0 成功；负值为 AGENT_ERROR_* / errno 风格错误码。
 */
int smart_home_asr_recognize(const uint8_t *pcm, size_t bytes,
                             char *out_text, size_t out_size);

#ifdef __cplusplus
}
#endif
