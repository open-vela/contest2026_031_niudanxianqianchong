/****************************************************************************
 * demos/smart_home/src/voice/smart_home_tts.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2026 The NuttX Contributors
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include "smart_home_tts.h"
#include "smart_home_voice_resample.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../config/cjson_compat.h"
#include "../config/smart_home_config_store.h"
#include "../config/smart_home_secrets.h"
#include "../smart_home_memory.h"

#include <agent/error.h>
#include <agent/transport.h>
#include <agent_openvela_runtime.h>
#include <agent_openvela_tls.h>
#include <agent_openvela_transport.h>

#include "mbedtls/base64.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifndef CONFIG_SMART_HOME_TTS_AUDIO_MAX_BYTES
#define CONFIG_SMART_HOME_TTS_AUDIO_MAX_BYTES (768 * 1024)
#endif

#define TTS_CONFIG_PATH SMART_HOME_CONFIG_DIR "/voice.json"
#define TTS_KEY_MAX     256
#define TTS_TIMEOUT_MS  30000u

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* TTS 端点预设。首选 MiMo（与 LLM mimo 预设同 host，secrets 里的
 * tts_api_keys.mimo 独立读取）；其余为 OpenAI 兼容 speech 形态备选。 */
static const smart_home_tts_preset_t g_tts_presets[] =
{
  {
    "mimo", "MiMo TTS",
    "api.xiaomimimo.com", "/v1/chat/completions", "443",
    "mimo-v2.5-tts", "mimo_default",
    SMART_HOME_TTS_API_CHAT_COMPLETIONS
  },
  {
    "siliconflow", "SiliconFlow",
    "api.siliconflow.cn", "/v1/audio/speech", "443",
    "fishaudio/fish-speech-1.5", "alex",
    SMART_HOME_TTS_API_SPEECH
  },
  {
    "dashscope", "Aliyun DashScope",
    "dashscope.aliyuncs.com", "/compatible-mode/v1/audio/speech", "443",
    "cosyvoice-v2", "longwan_v2",
    SMART_HOME_TTS_API_SPEECH
  },
  {
    "openai", "OpenAI",
    "api.openai.com", "/v1/audio/speech", "443",
    "gpt-4o-mini-tts", "alloy",
    SMART_HOME_TTS_API_SPEECH
  },
  {
    "custom", "Custom", "", "/v1/audio/speech", "443", "", "",
    SMART_HOME_TTS_API_SPEECH
  },
};

static smart_home_tts_config_t g_tts_config;
static pthread_mutex_t g_tts_config_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_tts_config_loaded;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void tts_config_from_preset(smart_home_tts_config_t *config,
                                   const smart_home_tts_preset_t *preset)
{
  strncpy(config->backend_id, preset->id,
          sizeof(config->backend_id) - 1);
  strncpy(config->host, preset->host, sizeof(config->host) - 1);
  strncpy(config->path, preset->path, sizeof(config->path) - 1);
  strncpy(config->port, preset->port, sizeof(config->port) - 1);
  strncpy(config->model, preset->default_model,
          sizeof(config->model) - 1);
  strncpy(config->voice, preset->default_voice,
          sizeof(config->voice) - 1);
}

static cJSON *tts_config_make_default(void *user_data)
{
  smart_home_tts_config_t config;
  cJSON *root;

  (void)user_data;
  memset(&config, 0, sizeof(config));
  tts_config_from_preset(&config,
                         smart_home_tts_preset_by_id("mimo"));
  config.enabled = 0;
  config.sample_rate = 16000;

  root = cJSON_CreateObject();
  if (root == NULL ||
      !cJSON_AddNumberToObject(root, "version", 1) ||
      !cJSON_AddBoolToObject(root, "enabled", 0) ||
      !cJSON_AddStringToObject(root, "backend_id", config.backend_id) ||
      !cJSON_AddStringToObject(root, "host", config.host) ||
      !cJSON_AddStringToObject(root, "path", config.path) ||
      !cJSON_AddStringToObject(root, "port", config.port) ||
      !cJSON_AddStringToObject(root, "model", config.model) ||
      !cJSON_AddStringToObject(root, "voice", config.voice) ||
      !cJSON_AddNumberToObject(root, "sample_rate",
                               (double)config.sample_rate))
    {
      cJSON_Delete(root);
      return NULL;
    }

  return root;
}

static void tts_config_read(const cJSON *root,
                            smart_home_tts_config_t *config)
{
  const cJSON *item;

  memset(config, 0, sizeof(*config));

  item = cJSON_GetObjectItemCaseSensitive(root, "enabled");
  config->enabled = cJSON_IsTrue(item);

  item = cJSON_GetObjectItemCaseSensitive(root, "backend_id");
  if (cJSON_IsString(item) && item->valuestring[0])
    {
      strncpy(config->backend_id, item->valuestring,
              sizeof(config->backend_id) - 1);
    }

  item = cJSON_GetObjectItemCaseSensitive(root, "host");
  if (cJSON_IsString(item))
    {
      strncpy(config->host, item->valuestring, sizeof(config->host) - 1);
    }

  item = cJSON_GetObjectItemCaseSensitive(root, "path");
  if (cJSON_IsString(item) && item->valuestring[0])
    {
      strncpy(config->path, item->valuestring, sizeof(config->path) - 1);
    }

  item = cJSON_GetObjectItemCaseSensitive(root, "port");
  if (cJSON_IsString(item) && item->valuestring[0])
    {
      strncpy(config->port, item->valuestring, sizeof(config->port) - 1);
    }

  item = cJSON_GetObjectItemCaseSensitive(root, "model");
  if (cJSON_IsString(item))
    {
      strncpy(config->model, item->valuestring,
              sizeof(config->model) - 1);
    }

  item = cJSON_GetObjectItemCaseSensitive(root, "voice");
  if (cJSON_IsString(item))
    {
      strncpy(config->voice, item->valuestring,
              sizeof(config->voice) - 1);
    }

  item = cJSON_GetObjectItemCaseSensitive(root, "sample_rate");
  if (cJSON_IsNumber(item) && item->valuedouble >= 8000.0 &&
      item->valuedouble <= 96000.0)
    {
      config->sample_rate = (uint32_t)item->valuedouble;
    }
  else
    {
      config->sample_rate = 16000;
    }
}

static int tts_config_validate(const cJSON *root, void *user_data)
{
  smart_home_tts_config_t config;

  (void)user_data;
  tts_config_read(root, &config);

  if (config.backend_id[0] == '\0' || config.path[0] == '\0' ||
      config.port[0] == '\0')
    {
      return AGENT_ERROR_PARSE;
    }

  /* 只有启用播报才要求端点可用；关闭状态下允许残缺配置存在。 */
  if (config.enabled &&
      (config.host[0] == '\0' || config.model[0] == '\0'))
    {
      return AGENT_ERROR_PARSE;
    }

  return AGENT_OK;
}

static void tts_config_ensure_loaded(void)
{
  cJSON *root = NULL;
  char error[96];

  pthread_mutex_lock(&g_tts_config_mutex);
  if (g_tts_config_loaded)
    {
      pthread_mutex_unlock(&g_tts_config_mutex);
      return;
    }

  if (smart_home_config_store_load(TTS_CONFIG_PATH,
                                   tts_config_make_default,
                                   SMART_HOME_CONFIG_RECOVER_DEFAULT,
                                   tts_config_validate, NULL,
                                   &root, error, sizeof(error)) == AGENT_OK)
    {
      tts_config_read(root, &g_tts_config);
      cJSON_Delete(root);
    }
  else
    {
      /* 连内存默认都构造失败时保持全零；enabled=0 使功能静默关闭。 */
      fprintf(stderr, "tts: config load failed: %s\n", error);
    }

  g_tts_config_loaded = 1;
  pthread_mutex_unlock(&g_tts_config_mutex);
}

/* 解析 WAV（RIFF/WAVE，PCM16）：返回 0 成功并给出 data 偏移、长度与
 * 采样率。只支持最常见的 fmt 在 data 之前的单 data 块布局。 */
static int tts_parse_wav(const uint8_t *data, size_t size,
                         size_t *data_offset, size_t *data_len,
                         uint32_t *rate)
{
  size_t offset;
  uint32_t chunk_size;

  if (size < 44 || memcmp(data, "RIFF", 4) != 0 ||
      memcmp(data + 8, "WAVE", 4) != 0)
    {
      return -EINVAL;
    }

  offset = 12;
  *rate = 0;
  while (offset + 8 <= size)
    {
      chunk_size = (uint32_t)data[offset + 4] |
                   ((uint32_t)data[offset + 5] << 8) |
                   ((uint32_t)data[offset + 6] << 16) |
                   ((uint32_t)data[offset + 7] << 24);

      if (chunk_size > size - offset - 8u) return -EINVAL;
      if (memcmp(data + offset, "fmt ", 4) == 0 && chunk_size >= 16)
        {
          uint16_t format;
          uint16_t channels;
          uint16_t bits;

          format = (uint16_t)(data[offset + 8] |
                              (data[offset + 9] << 8));
          channels = (uint16_t)(data[offset + 10] |
                                (data[offset + 11] << 8));
          bits = (uint16_t)(data[offset + 22] |
                            (data[offset + 23] << 8));
          *rate = (uint32_t)(data[offset + 12] |
                             (data[offset + 13] << 8) |
                             ((uint32_t)data[offset + 14] << 16) |
                             ((uint32_t)data[offset + 15] << 24));
          if (format != 1 || channels != 1 || bits != 16 || *rate == 0)
            {
              return -EINVAL;
            }
        }
      else if (memcmp(data + offset, "data", 4) == 0)
        {
          if (*rate == 0)
            {
              return -EINVAL;
            }

          *data_offset = offset + 8;
          *data_len = chunk_size;
          return 0;
        }

      offset += 8 + chunk_size + (chunk_size & 1u);
    }

  return -EINVAL;
}

/* 解析 MiMo chat 形态响应：缓冲内为 JSON 文本（choices[0].message.
 * audio.data 为 base64 WAV），解码后写回缓冲起始处。成功时
 * *decoded_len 为解码字节数。注意 cJSON 会把 base64 字符串拷贝到堆，
 * 峰值内存约为响应缓冲的两倍。 */
static int tts_decode_chat_response(uint8_t *audio, size_t capacity,
                                    size_t body_len, size_t *decoded_len)
{
  const cJSON *choices;
  const cJSON *choice;
  const cJSON *message;
  const cJSON *audio_obj;
  const cJSON *data;
  cJSON *root;
  size_t olen = 0;
  int ret;

  if (body_len == 0 || body_len + 1 > capacity)
    {
      return -EINVAL;
    }

  audio[body_len] = '\0';
  root = cJSON_Parse((const char *)audio);
  if (root == NULL)
    {
      return -EINVAL;
    }

  choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
  choice = cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0
               ? cJSON_GetArrayItem(choices, 0)
               : NULL;
  message = choice != NULL
                ? cJSON_GetObjectItemCaseSensitive(choice, "message")
                : NULL;
  audio_obj = message != NULL
                  ? cJSON_GetObjectItemCaseSensitive(message, "audio")
                  : NULL;
  data = audio_obj != NULL
             ? cJSON_GetObjectItemCaseSensitive(audio_obj, "data")
             : NULL;

  if (!cJSON_IsString(data) || data->valuestring == NULL ||
      data->valuestring[0] == '\0')
    {
      fprintf(stderr, "tts: chat response has no audio.data\n");
      cJSON_Delete(root);
      return -EINVAL;
    }

  ret = mbedtls_base64_decode(audio, capacity, &olen,
                              (const unsigned char *)data->valuestring,
                              strlen(data->valuestring));
  cJSON_Delete(root);
  if (ret != 0)
    {
      fprintf(stderr, "tts: base64 decode failed: -0x%04x\n", -ret);
      return -EINVAL;
    }

  *decoded_len = olen;
  return 0;
}

/* 按后端形态组装请求体。返回堆缓冲（JSON 文本），调用者释放。 */
static char *tts_build_request(const smart_home_tts_config_t *config,
                               int api_style, const char *text)
{
  cJSON *root;
  cJSON *messages;
  cJSON *message;
  cJSON *audio;
  char *body;

  root = cJSON_CreateObject();
  if (root == NULL)
    {
      return NULL;
    }

  if (api_style == SMART_HOME_TTS_API_CHAT_COMPLETIONS)
    {
      /* MiMo 形态：合成文本放 role:assistant 消息，audio 携带格式与音色。
       * message 先入树再填字段，失败路径由 cJSON_Delete(root) 统一回收。 */
      messages = cJSON_AddArrayToObject(root, "messages");
      message = cJSON_CreateObject();
      if (messages == NULL || message == NULL ||
          !cJSON_AddItemToArray(messages, message) ||
          !cJSON_AddStringToObject(root, "model", config->model) ||
          !cJSON_AddStringToObject(message, "role", "assistant") ||
          !cJSON_AddStringToObject(message, "content", text))
        {
          cJSON_Delete(root);
          return NULL;
        }

      audio = cJSON_AddObjectToObject(root, "audio");
      if (audio == NULL ||
          !cJSON_AddStringToObject(audio, "format", "wav") ||
          !cJSON_AddStringToObject(audio, "voice", config->voice))
        {
          cJSON_Delete(root);
          return NULL;
        }
    }
  else
    {
      if (!cJSON_AddStringToObject(root, "model", config->model) ||
          !cJSON_AddStringToObject(root, "input", text) ||
          !cJSON_AddStringToObject(root, "voice", config->voice) ||
          !cJSON_AddStringToObject(root, "response_format", "wav"))
        {
          cJSON_Delete(root);
          return NULL;
        }
    }

  body = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  return body;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

size_t smart_home_tts_preset_count(void)
{
  return sizeof(g_tts_presets) / sizeof(g_tts_presets[0]);
}

const smart_home_tts_preset_t *smart_home_tts_preset_get(size_t index)
{
  if (index >= smart_home_tts_preset_count())
    {
      return NULL;
    }

  return &g_tts_presets[index];
}

const smart_home_tts_preset_t *smart_home_tts_preset_by_id(const char *id)
{
  size_t index;

  for (index = 0; index < smart_home_tts_preset_count(); index++)
    {
      if (strcmp(g_tts_presets[index].id, id) == 0)
        {
          return &g_tts_presets[index];
        }
    }

  return &g_tts_presets[smart_home_tts_preset_count() - 1];
}

void smart_home_tts_config_load(smart_home_tts_config_t *config)
{
  tts_config_ensure_loaded();
  pthread_mutex_lock(&g_tts_config_mutex);
  memcpy(config, &g_tts_config, sizeof(*config));
  pthread_mutex_unlock(&g_tts_config_mutex);
}

int smart_home_tts_config_set(const smart_home_tts_config_t *config)
{
  int ret;

  if (config == NULL)
    {
      return AGENT_ERROR_INVALID;
    }

  tts_config_ensure_loaded();
  pthread_mutex_lock(&g_tts_config_mutex);
  memcpy(&g_tts_config, config, sizeof(g_tts_config));
  ret = AGENT_OK;
  pthread_mutex_unlock(&g_tts_config_mutex);
  return ret;
}

int smart_home_tts_config_save(const smart_home_tts_config_t *config)
{
  cJSON *root;
  int ret;

  if (config == NULL)
    {
      return AGENT_ERROR_INVALID;
    }

  root = cJSON_CreateObject();
  if (root == NULL ||
      !cJSON_AddNumberToObject(root, "version", 1) ||
      !cJSON_AddBoolToObject(root, "enabled", config->enabled ? 1 : 0) ||
      !cJSON_AddStringToObject(root, "backend_id", config->backend_id) ||
      !cJSON_AddStringToObject(root, "host", config->host) ||
      !cJSON_AddStringToObject(root, "path", config->path) ||
      !cJSON_AddStringToObject(root, "port", config->port) ||
      !cJSON_AddStringToObject(root, "model", config->model) ||
      !cJSON_AddStringToObject(root, "voice", config->voice) ||
      !cJSON_AddNumberToObject(root, "sample_rate",
                               (double)config->sample_rate))
    {
      cJSON_Delete(root);
      return AGENT_ERROR_NOMEM;
    }

  ret = tts_config_validate(root, NULL);
  if (ret == AGENT_OK) ret = smart_home_config_store_save(TTS_CONFIG_PATH, root);
  cJSON_Delete(root);

  if (ret == AGENT_OK)
    {
      smart_home_tts_config_set(config);
    }

  return ret;
}

/* ── HTTP 通路（cAGENT V2 openvela port：runtime + mbedTLS + webclient）──
 * 与 smart_home_agent_backend 的组合方式一致。静态上下文及请求诊断由
 * smart_home_tts_synth_ex() 的互斥锁保护，设置测试与播报不能同时调用。 */

#define TTS_HTTP_URL_MAX           288
#define TTS_HTTP_HEADER_BUFFER     512
#define TTS_HTTP_HEADER_CAPACITY   4
#define TTS_HTTP_IO_BUFFER         2048
/* 与 ASR 同一原因：MiMo 返回 11 个响应头，8 个槽位在解析阶段越界，
 * transport 报 AGENT_ERROR_CAPACITY(-16)。对齐 LLM 后端的 16/2048。 */
#define TTS_HTTP_RESPONSE_HEADERS  16
#define TTS_HTTP_RESP_HEADER_BUF   2048

/* Relative timestamps use the same monotonic clock as the request deadline. */
static uint64_t g_tts_request_started_ms;
static uint64_t tts_http_elapsed_ms(void);

/* TLS 层内部握手/收发失败时只回调这个函数，之前没有接入，导致
 * mbedTLS 的 ssl_setup/ssl_read/ssl_write 错误码全部丢失，上层只能拿到
 * transport 兜底的 AGENT_ERROR_IO(-12)。接入后可直接看到 -0x%04x。 */
static void tts_tls_log(void *context, agent_log_level_t level,
                        agent_string_view_t message)
{
  const char *tag = "?";

  (void)context;
  switch (level)
    {
      case AGENT_LOG_ERROR:   tag = "err";  break;
      case AGENT_LOG_WARNING: tag = "warn"; break;
      case AGENT_LOG_INFO:    tag = "info"; break;
      case AGENT_LOG_DEBUG:   tag = "dbg";  break;
      default:                              break;
    }

  printf("[tts][tls][%s] %.*s elapsed_ms=%llu\n", tag, (int)message.size,
         message.data != NULL ? message.data : "",
         (unsigned long long)tts_http_elapsed_ms());
}

static struct
{
  bool ready;
  agent_runtime_t runtime;
  agent_port_openvela_tls_t tls;
  agent_port_openvela_transport_t transport_state;
  agent_transport_t transport;
  char url[TTS_HTTP_URL_MAX];
  char request_header_buffer[TTS_HTTP_HEADER_BUFFER];
  const char *request_headers[TTS_HTTP_HEADER_CAPACITY];
  char io_buffer[TTS_HTTP_IO_BUFFER];
  agent_http_header_t response_headers[TTS_HTTP_RESPONSE_HEADERS];
  char response_header_buffer[TTS_HTTP_RESP_HEADER_BUF];
} g_tts_http;

static uint64_t tts_http_elapsed_ms(void)
{
  uint64_t now;
  if (g_tts_http.runtime.now_ms == NULL) return 0;
  now = g_tts_http.runtime.now_ms(g_tts_http.runtime.clock_context);
  return now >= g_tts_request_started_ms ? now - g_tts_request_started_ms : 0;
}

static int tts_http_init(void)
{
  agent_port_openvela_tls_config_t tls_config = {0};
  agent_port_openvela_transport_config_t transport_config = {0};
  int ret;

  if (g_tts_http.ready)
    {
      return AGENT_OK;
    }

  ret = agent_port_openvela_runtime_init(&g_tts_http.runtime);
  if (ret != AGENT_OK)
    {
      return ret;
    }

  tls_config.io_timeout_ms = TTS_TIMEOUT_MS;
  tls_config.log = tts_tls_log;
  tls_config.log_context = NULL;
  ret = agent_port_openvela_tls_init(&g_tts_http.tls, &tls_config);
  if (ret != AGENT_OK)
    {
      return ret;
    }

  transport_config.runtime = g_tts_http.runtime;
  transport_config.tls_ops = g_tts_http.tls.ops;
  transport_config.tls_context = &g_tts_http.tls;
  transport_config.request_timeout_ms = TTS_TIMEOUT_MS;
  transport_config.url_buffer = g_tts_http.url;
  transport_config.url_buffer_size = sizeof(g_tts_http.url);
  transport_config.request_header_buffer = g_tts_http.request_header_buffer;
  transport_config.request_header_buffer_size =
      sizeof(g_tts_http.request_header_buffer);
  transport_config.request_headers = g_tts_http.request_headers;
  transport_config.request_header_capacity = TTS_HTTP_HEADER_CAPACITY;
  transport_config.io_buffer = g_tts_http.io_buffer;
  transport_config.io_buffer_size = sizeof(g_tts_http.io_buffer);
  transport_config.response_headers = g_tts_http.response_headers;
  transport_config.response_header_capacity = TTS_HTTP_RESPONSE_HEADERS;
  transport_config.response_header_buffer = g_tts_http.response_header_buffer;
  transport_config.response_header_buffer_size =
      sizeof(g_tts_http.response_header_buffer);

  ret = agent_port_openvela_transport_init(&g_tts_http.transport,
                                           &g_tts_http.transport_state,
                                           &transport_config);
  if (ret != AGENT_OK)
    {
      return ret;
    }

  g_tts_http.ready = true;
  return AGENT_OK;
}

typedef struct
{
  uint8_t *buffer;
  size_t capacity;
  size_t used;
  unsigned int status;
  uint64_t headers_ms;
  uint64_t first_body_ms;
  uint64_t last_body_ms;
  uint64_t progress_ms;
  uint64_t expected_bytes;
  size_t progress_bytes;
  size_t body_callbacks;
  bool headers_seen;
  bool length_known;
  bool length_invalid;
  bool transfer_encoding;
} tts_http_sink_t;

/* Header views are length-delimited. Never assume a trailing NUL. */
static bool tts_header_is(agent_string_view_t name, const char *expected)
{
  size_t i;
  if (name.data == NULL || name.size != strlen(expected)) return false;
  for (i = 0; i < name.size; i++)
    {
      unsigned char c = (unsigned char)name.data[i];
      if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
      if (c != (unsigned char)expected[i]) return false;
    }
  return true;
}

static bool tts_content_length(agent_string_view_t value, uint64_t *length)
{
  size_t i = 0, end = value.size;
  uint64_t number = 0;
  if (value.data == NULL) return false;
  while (i < end && (value.data[i] == ' ' || value.data[i] == '\t')) i++;
  while (end > i && (value.data[end - 1] == ' ' || value.data[end - 1] == '\t')) end--;
  if (i == end) return false;
  for (; i < end; i++)
    {
      unsigned char c = (unsigned char)value.data[i];
      if (c < '0' || c > '9' || number > (UINT64_MAX - (c - '0')) / 10u)
        return false;
      number = number * 10u + (c - '0');
    }
  *length = number;
  return true;
}

/* A matched Content-Length is a byte-count observation, not HTTP success.
 * Transfer-Encoding takes precedence; framing completion is inside webclient.
 */
static const char *tts_length_state(const tts_http_sink_t *sink)
{
  if (sink->transfer_encoding) return "transfer-encoded";
  if (sink->length_invalid) return "invalid";
  if (!sink->length_known) return "unknown";
  if (sink->used < sink->expected_bytes) return "short";
  return sink->used == sink->expected_bytes ? "matched" : "over";
}

static void tts_http_report(const tts_http_sink_t *sink, int ret)
{
  uint64_t elapsed = tts_http_elapsed_ms();
  printf("[tts] http: ret=%d status=%u body_len=%zu elapsed_ms=%llu "
         "headers_seen=%u headers_cb_ms=%llu first_body_ms=%llu "
         "last_body_ms=%llu body_idle_ms=%llu callbacks=%zu "
         "length_known=%u expected_len=%llu length_state=%s deadline_expired=%u\n",
         ret, sink->status, sink->used, (unsigned long long)elapsed,
         (unsigned int)sink->headers_seen, (unsigned long long)sink->headers_ms,
         (unsigned long long)sink->first_body_ms,
         (unsigned long long)sink->last_body_ms,
         (unsigned long long)(sink->body_callbacks ? elapsed - sink->last_body_ms : 0),
         sink->body_callbacks, (unsigned int)sink->length_known,
         (unsigned long long)sink->expected_bytes, tts_length_state(sink),
         (unsigned int)(elapsed >= TTS_TIMEOUT_MS));
}

static agent_error_t tts_sink_headers(void *context, unsigned int status,
                                      const agent_http_header_t *headers,
                                      size_t count)
{
  tts_http_sink_t *sink = context;

  size_t i;
  sink->status = status;
  sink->headers_seen = true;
  sink->headers_ms = tts_http_elapsed_ms();
  printf("[tts] headers: status=%u count=%zu elapsed_ms=%llu\n",
         status, count, (unsigned long long)sink->headers_ms);
  for (i = 0; i < count; i++)
    {
      const agent_http_header_t *h = &headers[i];
      bool is_length = tts_header_is(h->name, "content-length");
      bool is_transfer = tts_header_is(h->name, "transfer-encoding");
      /* Whitelist framing/type headers only; no cookies, keys or body text. */
      if (is_length || is_transfer || tts_header_is(h->name, "connection") ||
          tts_header_is(h->name, "content-type") ||
          tts_header_is(h->name, "content-encoding"))
        printf("[tts] header: %.*s: %.*s\n",
               (int)h->name.size, h->name.data,
               (int)(h->value.size < 160u ? h->value.size : 160u),
               h->value.data != NULL ? h->value.data : "");
      if (is_transfer) sink->transfer_encoding = true;
      if (is_length)
        {
          uint64_t length;
          if (sink->length_known || sink->length_invalid ||
              !tts_content_length(h->value, &length))
            sink->length_invalid = true;
          else
            {
              sink->length_known = true;
              sink->expected_bytes = length;
            }
        }
    }
  return AGENT_OK;
}

static agent_error_t tts_sink_body(void *context, const void *data,
                                   size_t size)
{
  tts_http_sink_t *sink = context;

  if (sink->used < sink->capacity)
    {
      size_t room = sink->capacity - sink->used;
      size_t copy = size > room ? room : size;

      memcpy(sink->buffer + sink->used, data, copy);
      sink->used += copy;
    }

  if (size > 0)
    {
      uint64_t elapsed = tts_http_elapsed_ms();
      if (sink->body_callbacks == 0) sink->first_body_ms = elapsed;
      sink->body_callbacks++;
      sink->last_body_ms = elapsed;
      /* Bound serial overhead: first callback, then 64 KiB or 5 s of progress. */
      if (sink->body_callbacks == 1 ||
          sink->used - sink->progress_bytes >= 65536u ||
          elapsed - sink->progress_ms >= 5000u)
        {
          printf("[tts] receive: elapsed_ms=%llu body_len=%zu callbacks=%zu\n",
                 (unsigned long long)elapsed, sink->used, sink->body_callbacks);
          sink->progress_bytes = sink->used;
          sink->progress_ms = elapsed;
        }
    }

  /* 超出 TTS 音频上限的尾部丢弃；与旧 http_post 的截断行为一致。 */
  return AGENT_OK;
}

static int tts_synth_impl(const char *text,
                         uint8_t **out_pcm, size_t *out_bytes,
                         uint32_t *out_rate,
                         smart_home_voice_result_t *result)
{
  smart_home_tts_config_t config;
  const smart_home_tts_preset_t *preset;
  int api_style;
  char api_key[TTS_KEY_MAX];
  char auth_value[TTS_KEY_MAX + 8];
  char url[TTS_HTTP_URL_MAX];
  agent_http_header_t headers[2];
  agent_http_request_t request;
  agent_http_sink_t http_sink;
  tts_http_sink_t sink;
  char *body = NULL;
  uint8_t *audio = NULL;
  size_t data_offset;
  size_t data_len;
  size_t response_len;
  size_t url_len;
  uint64_t deadline_ms;
  uint32_t rate;
  int ret;

  *result = (smart_home_voice_result_t){AGENT_ERROR_INVALID, 0,
                                       SMART_HOME_VOICE_RESOURCE};
  if (out_pcm) *out_pcm = NULL;
  if (out_bytes) *out_bytes = 0;
  if (out_rate) *out_rate = 0;

  if (text == NULL || text[0] == '\0' || out_pcm == NULL ||
      out_bytes == NULL || out_rate == NULL)
    {
      return AGENT_ERROR_INVALID;
    }

  smart_home_tts_config_load(&config);
  printf("[tts] synth: text_len=%zu backend=%s model=%s voice=%s "
         "host=%s:%s%s enabled=%d\n",
         strlen(text), config.backend_id, config.model, config.voice,
         config.host, config.port, config.path, config.enabled);
  if (config.host[0] == '\0' || config.model[0] == '\0')
    {
      fprintf(stderr, "tts: backend not configured (voice.json)\n");
      return AGENT_ERROR_INVALID;
    }

  /* 请求/响应形态由 backend_id 对应预设决定；未知 id 走 speech 形态。 */
  preset = smart_home_tts_preset_by_id(config.backend_id);
  api_style = preset != NULL ? preset->api_style
                             : SMART_HOME_TTS_API_SPEECH;

  result->stage = SMART_HOME_VOICE_CREDENTIAL;
  ret = smart_home_secrets_get_api_key(SMART_HOME_SECRET_TTS, config.backend_id,
                                             api_key, sizeof(api_key));
  if (ret != AGENT_OK)
    {
      fprintf(stderr, "tts: credential backend=%s ret=%d: %s\n",
              config.backend_id, ret, smart_home_secrets_error_text(ret));
      result->code = ret;
      return ret;
    }

  result->stage = SMART_HOME_VOICE_RESOURCE;
  body = tts_build_request(&config, api_style, text);
  if (body == NULL)
    {
      result->code = AGENT_ERROR_NOMEM;
      return result->code;
    }

  audio = smart_home_bulk_alloc(CONFIG_SMART_HOME_TTS_AUDIO_MAX_BYTES);
  if (audio == NULL)
    {
      free(body);
      result->code = AGENT_ERROR_NOMEM;
      return result->code;
    }

  result->stage = SMART_HOME_VOICE_NETWORK;
  ret = tts_http_init();
  if (ret != AGENT_OK)
    {
      /* OFFLINE_UI 等无网络/TLS 运行时：合成不可用而非崩溃。 */
      fprintf(stderr, "tts: http transport init failed: %d\n", ret);
      free(body);
      smart_home_bulk_free(audio);
      result->code = ret;
      return result->code;
    }

  url_len = (size_t)snprintf(url, sizeof(url), "https://%s:%s%s",
                             config.host, config.port, config.path);
  if (url_len >= sizeof(url))
    {
      fprintf(stderr, "tts: endpoint url too long\n");
      free(body);
      smart_home_bulk_free(audio);
      result->stage = SMART_HOME_VOICE_RESOURCE;
      result->code = AGENT_ERROR_LIMIT;
      return result->code;
    }

  snprintf(auth_value, sizeof(auth_value), "Bearer %s", api_key);
  headers[0].name = agent_string_view("Authorization", 13u);
  headers[0].value = agent_string_view(auth_value, strlen(auth_value));
  headers[1].name = agent_string_view("Content-Type", 12u);
  headers[1].value = agent_string_view("application/json", 16u);

  g_tts_request_started_ms =
      g_tts_http.runtime.now_ms(g_tts_http.runtime.clock_context);
  deadline_ms = g_tts_request_started_ms + TTS_TIMEOUT_MS;

  memset(&request, 0, sizeof(request));
  request.method = agent_string_view("POST", 4u);
  request.url = agent_string_view(url, url_len);
  request.headers = headers;
  request.header_count = 2u;
  request.body = body;
  request.body_size = strlen(body);
  request.max_response_bytes = CONFIG_SMART_HOME_TTS_AUDIO_MAX_BYTES;
  request.max_response_header_bytes = TTS_HTTP_RESP_HEADER_BUF;
  request.deadline_ms = deadline_ms;
  request.cancel = NULL;

  http_sink.headers = tts_sink_headers;
  http_sink.body = tts_sink_body;
  http_sink.context = &sink;
  memset(&sink, 0, sizeof(sink));
  sink.buffer = audio;
  sink.capacity = CONFIG_SMART_HOME_TTS_AUDIO_MAX_BYTES;
  sink.used = 0;
  sink.status = 0;

  printf("[tts] http: POST %s body=%zu deadline=%llums "
         "io_timeout_ms=%u io_buffer=%zu\n",
         url, strlen(body),
         (unsigned long long)(deadline_ms -
           g_tts_http.runtime.now_ms(g_tts_http.runtime.clock_context)),
         (unsigned int)TTS_TIMEOUT_MS, sizeof(g_tts_http.io_buffer));
  ret = g_tts_http.transport.ops->request(g_tts_http.transport.context,
                                          &request, &http_sink);
  free(body);
  response_len = sink.used;
  tts_http_report(&sink, ret);

  if (ret != AGENT_OK)
    {
      fprintf(stderr, "tts: synth http failed: %d\n", ret);
      goto out;
    }

  result->http_status = sink.status;
  result->stage = SMART_HOME_VOICE_HTTP;
  if (sink.status < 200 || sink.status >= 300)
    {
      /* 错误响应是 JSON 文本；截断打印辅助排障。 */
      fprintf(stderr, "tts: synth http status=%u body: %.*s\n",
              sink.status, (int)(response_len < 200 ? response_len : 200),
              response_len > 0 ? (const char *)audio : "(empty)");
      ret = AGENT_ERROR_MODEL_FAILED;
      goto out;
    }

  result->stage = SMART_HOME_VOICE_RESPONSE;
  if (api_style == SMART_HOME_TTS_API_CHAT_COMPLETIONS)
    {
      size_t decoded_len = 0;

      ret = tts_decode_chat_response(
          audio, CONFIG_SMART_HOME_TTS_AUDIO_MAX_BYTES,
          response_len, &decoded_len);
      if (ret != 0)
        {
          printf("[tts] decode: failed ret=%d\n", ret);
          ret = AGENT_ERROR_PARSE;
          goto out;
        }

      printf("[tts] decode: json=%zu base64_decoded=%zu\n",
             response_len, decoded_len);
      response_len = decoded_len;
    }

  ret = tts_parse_wav(audio, response_len,
                      &data_offset, &data_len, &rate);
  if (ret != 0)
    {
      /* 打印前 32 字节，能直接看出是 WAV/MP3/JSON/空数据。 */
      size_t i;
      printf("[tts] wav: parse failed len=%zu head=", response_len);
      for (i = 0; i < 32u && i < response_len; i++)
        {
          printf("%02x", audio[i]);
        }
      printf("\n");
      fprintf(stderr, "tts: response is not PCM16 mono wav "
              "(len=%zu)\n", response_len);
      ret = AGENT_ERROR_PARSE;
      goto out;
    }

  /* 采样率是播放端 CONFIGURE 是否成功的关键：ES8311 的 MCLK/rate 系数表
   * 在开机默认 4096000 下只有 8k/16k/32k/64k，云端给 24k 时会直接失败。 */
  printf("[tts] wav: rate=%lu data_offset=%zu data_len=%zu\n",
         (unsigned long)rate, data_offset, data_len);

  if (data_len < 2 || (data_len & 1u))
    {
      ret = AGENT_ERROR_PARSE;
      goto out;
    }

  if (rate == 24000u)
    {
      size_t converted_bytes = smart_home_voice_resample_bytes(data_len);
      uint8_t *converted;
      result->stage = SMART_HOME_VOICE_RESOURCE;
      converted = smart_home_bulk_alloc(converted_bytes);
      if (converted == NULL) { ret = AGENT_ERROR_NOMEM; goto out; }
      ret = smart_home_voice_resample_24k_16k(audio + data_offset, data_len,
                    converted, converted_bytes, &converted_bytes);
      if (ret != 0)
        {
          smart_home_bulk_free(converted);
          ret = AGENT_ERROR_INVALID;
          goto out;
        }
      printf("[tts] resample: 24000->16000 bytes=%zu->%zu\n",
             data_len, converted_bytes);
      smart_home_bulk_free(audio);
      audio = converted;
      data_offset = 0;
      data_len = converted_bytes;
      rate = 16000;
    }

  /* 原地搬移去掉 WAV 头，缓冲仍由 bulk 堆分配，语义与声明一致。 */
  memmove(audio, audio + data_offset, data_len);
  *out_pcm = audio;
  *out_bytes = data_len;
  *out_rate = rate;
  result->code = AGENT_OK;
  return AGENT_OK;

out:
  result->code = ret;
  smart_home_bulk_free(audio);
  return ret;
}

/* All callers share the static HTTP exchange buffers. Do not let a settings
 * test race the automatic announcement worker or a smoke command.
 */
static pthread_mutex_t g_tts_request_mutex = PTHREAD_MUTEX_INITIALIZER;

int smart_home_tts_synth_ex(const char *text, uint8_t **out_pcm,
                           size_t *out_bytes, uint32_t *out_rate,
                           smart_home_voice_result_t *result)
{
  smart_home_voice_result_t local;
  int ret;
  if (result == NULL) result = &local;
  *result = (smart_home_voice_result_t){AGENT_ERROR_BUSY, 0,
                                       SMART_HOME_VOICE_RESOURCE};
  if (out_pcm) *out_pcm = NULL;
  if (out_bytes) *out_bytes = 0;
  if (out_rate) *out_rate = 0;
  if (pthread_mutex_trylock(&g_tts_request_mutex) != 0) return AGENT_ERROR_BUSY;
  ret = tts_synth_impl(text, out_pcm, out_bytes, out_rate, result);
  pthread_mutex_unlock(&g_tts_request_mutex);
  return ret;
}

int smart_home_tts_synth(const char *text, uint8_t **out_pcm,
                         size_t *out_bytes, uint32_t *out_rate)
{
  return smart_home_tts_synth_ex(text, out_pcm, out_bytes, out_rate, NULL);
}
