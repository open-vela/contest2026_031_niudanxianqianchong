/****************************************************************************
 * demos/smart_home/src/voice/smart_home_asr.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2026 The NuttX Contributors
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include "smart_home_asr.h"

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

#define ASR_CONFIG_PATH  SMART_HOME_CONFIG_DIR "/asr.json"
#define ASR_KEY_MAX      256
#define ASR_TIMEOUT_MS   30000u

/* ASR 后端预设。首选 MiMo（与 LLM/TTS 的 mimo 预设同 host，secrets 里的
 * mimo key 直接复用）；custom 供自定义 OpenAI 兼容 chat completions
 * 端点（host/model 留空，由 asr.json 提供）。 */
static const smart_home_asr_preset_t g_asr_presets[] =
{
  {
    "mimo", "MiMo ASR",
    "api.xiaomimimo.com", "/v1/chat/completions", "443",
    "mimo-v2.5-asr"
  },
  {
    "custom", "Custom", "", "/v1/chat/completions", "443", ""
  },
};

/* 请求体前后缀（base64 音频之外的固定部分；model 由配置提供）。 */
#define ASR_BODY_PREFIX \
  "\"messages\":[{\"role\":\"user\",\"content\":[" \
  "{\"type\":\"input_audio\",\"input_audio\":{" \
  "\"data\":\"data:audio/wav;base64,"

#define ASR_BODY_SUFFIX \
  "\",\"format\":\"wav\"}}]}]," \
  "\"asr_options\":{\"language\":\""

/* 响应文本缓冲：错误详情按 512 字节截断打印。 */
#define ASR_RESPONSE_MAX 4096

#ifndef CONFIG_SMART_HOME_ASR_AUDIO_MAX_SECONDS
#define CONFIG_SMART_HOME_ASR_AUDIO_MAX_SECONDS 12
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

static smart_home_asr_config_t g_asr_config;
static pthread_mutex_t g_asr_config_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_asr_config_loaded;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

size_t smart_home_asr_preset_count(void)
{
  return sizeof(g_asr_presets) / sizeof(g_asr_presets[0]);
}

const smart_home_asr_preset_t *smart_home_asr_preset_get(size_t index)
{
  if (index >= smart_home_asr_preset_count())
    {
      return NULL;
    }

  return &g_asr_presets[index];
}

const smart_home_asr_preset_t *smart_home_asr_preset_by_id(const char *id)
{
  size_t i;

  if (id == NULL || id[0] == '\0')
    {
      return NULL;
    }

  for (i = 0; i < smart_home_asr_preset_count(); i++)
    {
      if (strcmp(g_asr_presets[i].id, id) == 0)
        {
          return &g_asr_presets[i];
        }
    }

  return NULL;
}

static const smart_home_asr_preset_t *asr_config_resolve_preset(
    const smart_home_asr_config_t *config)
{
  const smart_home_asr_preset_t *preset =
      smart_home_asr_preset_by_id(config->backend_id);

  return preset != NULL ? preset : smart_home_asr_preset_by_id("mimo");
}

/* 端点字段缺失时按 backend_id 预设回填；未知 id 回退 mimo 预设。
 * 旧版 asr.json（仅 enabled/language）因此保持有效。 */
static void asr_config_fill_endpoint(smart_home_asr_config_t *config)
{
  const smart_home_asr_preset_t *preset;

  if (config->backend_id[0] == '\0')
    {
      strcpy(config->backend_id, "mimo");
    }

  preset = asr_config_resolve_preset(config);
  if (preset == NULL)
    {
      return;
    }

  if (config->host[0] == '\0')
    {
      strncpy(config->host, preset->host, sizeof(config->host) - 1);
    }
  if (config->path[0] == '\0')
    {
      strncpy(config->path, preset->path, sizeof(config->path) - 1);
    }
  if (config->port[0] == '\0')
    {
      strncpy(config->port, preset->port, sizeof(config->port) - 1);
    }
  if (config->model[0] == '\0')
    {
      strncpy(config->model, preset->default_model,
              sizeof(config->model) - 1);
    }
}

static void asr_config_from_preset(smart_home_asr_config_t *config,
                                   const smart_home_asr_preset_t *preset)
{
  strncpy(config->backend_id, preset->id,
          sizeof(config->backend_id) - 1);
  strncpy(config->host, preset->host, sizeof(config->host) - 1);
  strncpy(config->path, preset->path, sizeof(config->path) - 1);
  strncpy(config->port, preset->port, sizeof(config->port) - 1);
  strncpy(config->model, preset->default_model,
          sizeof(config->model) - 1);
}

static cJSON *asr_config_make_default(void *user_data)
{
  smart_home_asr_config_t config;
  cJSON *root;

  (void)user_data;
  memset(&config, 0, sizeof(config));
  asr_config_from_preset(&config, smart_home_asr_preset_by_id("mimo"));
  config.enabled = 0;
  strcpy(config.language, "zh");

  root = cJSON_CreateObject();
  if (root == NULL ||
      !cJSON_AddNumberToObject(root, "version", 1) ||
      !cJSON_AddBoolToObject(root, "enabled", 0) ||
      !cJSON_AddStringToObject(root, "backend_id", config.backend_id) ||
      !cJSON_AddStringToObject(root, "host", config.host) ||
      !cJSON_AddStringToObject(root, "path", config.path) ||
      !cJSON_AddStringToObject(root, "port", config.port) ||
      !cJSON_AddStringToObject(root, "model", config.model) ||
      !cJSON_AddStringToObject(root, "language", config.language))
    {
      cJSON_Delete(root);
      return NULL;
    }

  return root;
}

static void asr_config_read(const cJSON *root,
                            smart_home_asr_config_t *config)
{
  const cJSON *item;

  memset(config, 0, sizeof(*config));
  strcpy(config->language, "zh");

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

  item = cJSON_GetObjectItemCaseSensitive(root, "language");
  if (cJSON_IsString(item) && item->valuestring[0])
    {
      strncpy(config->language, item->valuestring,
              sizeof(config->language) - 1);
    }

  asr_config_fill_endpoint(config);
}

static int asr_config_validate(const cJSON *root, void *user_data)
{
  smart_home_asr_config_t config;

  (void)user_data;
  asr_config_read(root, &config);
  if (strcmp(config.language, "zh") != 0 &&
      strcmp(config.language, "en") != 0 &&
      strcmp(config.language, "auto") != 0)
    {
      return AGENT_ERROR_PARSE;
    }

  /* 只有启用语音输入才要求端点可用；关闭状态下允许残缺配置存在。 */
  if (config.enabled &&
      (config.host[0] == '\0' || config.model[0] == '\0'))
    {
      return AGENT_ERROR_PARSE;
    }

  return AGENT_OK;
}

static void asr_config_ensure_loaded(void)
{
  cJSON *root = NULL;
  char error[96];

  pthread_mutex_lock(&g_asr_config_mutex);
  if (g_asr_config_loaded)
    {
      pthread_mutex_unlock(&g_asr_config_mutex);
      return;
    }

  if (smart_home_config_store_load_compatible(ASR_CONFIG_PATH, 2,
                                   asr_config_make_default,
                                   SMART_HOME_CONFIG_RECOVER_DEFAULT,
                                   asr_config_validate, NULL,
                                   &root, error, sizeof(error)) == AGENT_OK)
    {
      asr_config_read(root, &g_asr_config);
      cJSON_Delete(root);
    }
  else
    {
      fprintf(stderr, "asr: config load failed: %s\n", error);
      memset(&g_asr_config, 0, sizeof(g_asr_config));
      g_asr_config.enabled = 0;
      strcpy(g_asr_config.language, "zh");
      asr_config_fill_endpoint(&g_asr_config);
    }

  g_asr_config_loaded = 1;
  pthread_mutex_unlock(&g_asr_config_mutex);
}

static void asr_put_u32le(uint8_t *dst, uint32_t value)
{
  dst[0] = (uint8_t)value;
  dst[1] = (uint8_t)(value >> 8);
  dst[2] = (uint8_t)(value >> 16);
  dst[3] = (uint8_t)(value >> 24);
}

static void asr_put_u16le(uint8_t *dst, uint16_t value)
{
  dst[0] = (uint8_t)value;
  dst[1] = (uint8_t)(value >> 8);
}

/* 组装 44 字节 PCM16 单声道 WAV 头（fmt 在 data 前的单块布局）。 */
static void asr_make_wav_header(uint8_t *header, uint32_t data_bytes,
                                uint32_t rate)
{
  memcpy(header + 0, "RIFF", 4);
  asr_put_u32le(header + 4, 36u + data_bytes);
  memcpy(header + 8, "WAVE", 4);
  memcpy(header + 12, "fmt ", 4);
  asr_put_u32le(header + 16, 16u);
  asr_put_u16le(header + 20, 1u);           /* PCM */
  asr_put_u16le(header + 22, 1u);           /* mono */
  asr_put_u32le(header + 24, rate);
  asr_put_u32le(header + 28, rate * 2u);    /* byte rate */
  asr_put_u16le(header + 32, 2u);           /* block align */
  asr_put_u16le(header + 34, 16u);          /* bits */
  memcpy(header + 36, "data", 4);
  asr_put_u32le(header + 40, data_bytes);
}

/* 响应解析：choices[0].message.content 为转写文本。 */
static int asr_parse_response(const char *body, size_t body_len,
                              char *out_text, size_t out_size)
{
  const cJSON *choices;
  const cJSON *choice;
  const cJSON *message;
  const cJSON *content;
  cJSON *root;

  if (body_len == 0 || out_size == 0)
    {
      return -EINVAL;
    }

  root = cJSON_Parse(body);
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
  content = message != NULL
                ? cJSON_GetObjectItemCaseSensitive(message, "content")
                : NULL;

  if (!cJSON_IsString(content) || content->valuestring == NULL ||
      content->valuestring[0] == '\0')
    {
      fprintf(stderr, "asr: response has no message.content\n");
      cJSON_Delete(root);
      return -EINVAL;
    }

  strncpy(out_text, content->valuestring, out_size - 1);
  out_text[out_size - 1] = '\0';
  cJSON_Delete(root);
  return 0;
}

/* ── HTTP 通路（cAGENT V2 openvela port：runtime + mbedTLS + webclient）──
 * 与 smart_home_agent_backend 的组合方式一致。识别只被 ASR worker 线程
 * 串行调用，静态上下文不需要加锁。 */

#define ASR_HTTP_URL_MAX           288
#define ASR_HTTP_HEADER_BUFFER     512
#define ASR_HTTP_HEADER_CAPACITY   4
#define ASR_HTTP_IO_BUFFER         1024
#define ASR_HTTP_RESPONSE_HEADERS  8
#define ASR_HTTP_RESP_HEADER_BUF   1024

static struct
{
  bool ready;
  agent_runtime_t runtime;
  agent_port_openvela_tls_t tls;
  agent_port_openvela_transport_t transport_state;
  agent_transport_t transport;
  char url[ASR_HTTP_URL_MAX];
  char request_header_buffer[ASR_HTTP_HEADER_BUFFER];
  const char *request_headers[ASR_HTTP_HEADER_CAPACITY];
  char io_buffer[ASR_HTTP_IO_BUFFER];
  agent_http_header_t response_headers[ASR_HTTP_RESPONSE_HEADERS];
  char response_header_buffer[ASR_HTTP_RESP_HEADER_BUF];
} g_asr_http;

static int asr_http_init(void)
{
  agent_port_openvela_tls_config_t tls_config = {0};
  agent_port_openvela_transport_config_t transport_config = {0};
  int ret;

  if (g_asr_http.ready)
    {
      return AGENT_OK;
    }

  ret = agent_port_openvela_runtime_init(&g_asr_http.runtime);
  if (ret != AGENT_OK)
    {
      return ret;
    }

  tls_config.io_timeout_ms = ASR_TIMEOUT_MS;
  ret = agent_port_openvela_tls_init(&g_asr_http.tls, &tls_config);
  if (ret != AGENT_OK)
    {
      return ret;
    }

  transport_config.runtime = g_asr_http.runtime;
  transport_config.tls_ops = g_asr_http.tls.ops;
  transport_config.tls_context = &g_asr_http.tls;
  transport_config.request_timeout_ms = ASR_TIMEOUT_MS;
  transport_config.url_buffer = g_asr_http.url;
  transport_config.url_buffer_size = sizeof(g_asr_http.url);
  transport_config.request_header_buffer = g_asr_http.request_header_buffer;
  transport_config.request_header_buffer_size =
      sizeof(g_asr_http.request_header_buffer);
  transport_config.request_headers = g_asr_http.request_headers;
  transport_config.request_header_capacity = ASR_HTTP_HEADER_CAPACITY;
  transport_config.io_buffer = g_asr_http.io_buffer;
  transport_config.io_buffer_size = sizeof(g_asr_http.io_buffer);
  transport_config.response_headers = g_asr_http.response_headers;
  transport_config.response_header_capacity = ASR_HTTP_RESPONSE_HEADERS;
  transport_config.response_header_buffer = g_asr_http.response_header_buffer;
  transport_config.response_header_buffer_size =
      sizeof(g_asr_http.response_header_buffer);

  ret = agent_port_openvela_transport_init(&g_asr_http.transport,
                                           &g_asr_http.transport_state,
                                           &transport_config);
  if (ret != AGENT_OK)
    {
      return ret;
    }

  g_asr_http.ready = true;
  return AGENT_OK;
}

typedef struct
{
  uint8_t *buffer;
  size_t capacity;
  size_t used;
  unsigned int status;
} asr_http_sink_t;

static agent_error_t asr_sink_headers(void *context, unsigned int status,
                                      const agent_http_header_t *headers,
                                      size_t count)
{
  asr_http_sink_t *sink = context;

  (void)headers;
  (void)count;
  sink->status = status;
  return AGENT_OK;
}

static agent_error_t asr_sink_body(void *context, const void *data,
                                   size_t size)
{
  asr_http_sink_t *sink = context;

  if (sink->used < sink->capacity)
    {
      size_t room = sink->capacity - sink->used;
      size_t copy = size > room ? room : size;

      memcpy(sink->buffer + sink->used, data, copy);
      sink->used += copy;
    }

  /* 超出容量的尾部直接丢弃；错误详情打印只取前 200 字节，
   * 与旧 http_post 的截断行为一致。 */
  return AGENT_OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void smart_home_asr_config_load(smart_home_asr_config_t *config)
{
  if (config == NULL)
    {
      return;
    }

  asr_config_ensure_loaded();
  pthread_mutex_lock(&g_asr_config_mutex);
  memcpy(config, &g_asr_config, sizeof(*config));
  pthread_mutex_unlock(&g_asr_config_mutex);
}

int smart_home_asr_config_save(const smart_home_asr_config_t *config)
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
      !cJSON_AddStringToObject(root, "language", config->language))
    {
      cJSON_Delete(root);
      return AGENT_ERROR_NOMEM;
    }

  ret = asr_config_validate(root, NULL);
  if (ret == AGENT_OK) ret = smart_home_config_store_save(ASR_CONFIG_PATH, root);
  cJSON_Delete(root);

  if (ret == AGENT_OK)
    {
      asr_config_ensure_loaded();
      pthread_mutex_lock(&g_asr_config_mutex);
      memcpy(&g_asr_config, config, sizeof(g_asr_config));
      pthread_mutex_unlock(&g_asr_config_mutex);
    }

  return ret;
}

int smart_home_asr_recognize_ex(const uint8_t *pcm, size_t bytes,
                             char *out_text, size_t out_size,
                             smart_home_voice_result_t *result)
{
  smart_home_asr_config_t config;
  char api_key[ASR_KEY_MAX];
  char auth_value[ASR_KEY_MAX + 8];
  char url[ASR_HTTP_URL_MAX];
  char model_field[SMART_HOME_ASR_MODEL_MAX + 16];
  agent_http_header_t headers[2];
  agent_http_request_t request;
  agent_http_sink_t http_sink;
  asr_http_sink_t sink;
  uint8_t *wav = NULL;
  char *body = NULL;
  uint8_t *response_buf = NULL;
  size_t model_field_len;
  size_t prefix_len;
  size_t suffix_len;
  size_t b64_len;
  size_t olen = 0;
  size_t body_size;
  size_t url_len;
  uint64_t deadline_ms;
  int ret;

  smart_home_voice_result_t local_result = {AGENT_ERROR_INVALID, 0,
                                            SMART_HOME_VOICE_CREDENTIAL};
  if (result == NULL) result = &local_result;
  *result = local_result;

  if (pcm == NULL || bytes == 0 || out_text == NULL || out_size == 0)
    {
      return AGENT_ERROR_INVALID;
    }

  out_text[0] = '\0';
  smart_home_asr_config_load(&config);
  asr_config_fill_endpoint(&config);

  ret = smart_home_secrets_get_api_key(SMART_HOME_SECRET_ASR, config.backend_id,
                                             api_key, sizeof(api_key));
  if (ret != AGENT_OK)
    {
      result->code = ret;
      fprintf(stderr, "asr: credential backend=%s ret=%d: %s\n",
              config.backend_id, ret, smart_home_secrets_error_text(ret));
      return ret;
    }

  result->stage = SMART_HOME_VOICE_RESPONSE;
  result->code = AGENT_ERROR_NOMEM;

  /* WAV 组装：44 字节头 + 原始 PCM。 */
  wav = smart_home_bulk_alloc(bytes + 44u);
  if (wav == NULL)
    {
      return AGENT_ERROR_NOMEM;
    }

  asr_make_wav_header(wav, (uint32_t)bytes, 16000u);
  memcpy(wav + 44, pcm, bytes);

  /* 请求体 = {"model":"<model>", + 前缀 + base64(WAV) + 语言后缀 + 收尾。
   * 大字段手工拼接而非 cJSON，避免 4 倍峰值内存。 */
  model_field_len = (size_t)snprintf(model_field, sizeof(model_field),
                                     "{\"model\":\"%s\",", config.model);
  if (model_field_len >= sizeof(model_field))
    {
      smart_home_bulk_free(wav);
      return AGENT_ERROR_LIMIT;
    }

  prefix_len = model_field_len + strlen(ASR_BODY_PREFIX);
  b64_len = ((bytes + 44u + 2u) / 3u) * 4u + 4u;
  suffix_len = strlen(ASR_BODY_SUFFIX) + strlen(config.language) + 4u;

  body = smart_home_bulk_alloc(prefix_len + b64_len + suffix_len + 1u);
  if (body == NULL)
    {
      smart_home_bulk_free(wav);
      return AGENT_ERROR_NOMEM;
    }

  memcpy(body, model_field, model_field_len);
  memcpy(body + model_field_len, ASR_BODY_PREFIX, strlen(ASR_BODY_PREFIX));
  ret = mbedtls_base64_encode((unsigned char *)body + prefix_len, b64_len,
                              &olen, wav, bytes + 44u);
  smart_home_bulk_free(wav);
  if (ret != 0)
    {
      fprintf(stderr, "asr: base64 encode failed: -0x%04x\n", -ret);
      smart_home_bulk_free(body);
      return AGENT_ERROR_NOMEM;
    }

  sprintf(body + prefix_len + olen, ASR_BODY_SUFFIX "%s\"}}",
          config.language);

  result->stage = SMART_HOME_VOICE_NETWORK;
  ret = asr_http_init();
  if (ret != AGENT_OK)
    {
      fprintf(stderr, "asr: http transport init failed: %d\n", ret);
      smart_home_bulk_free(body);
      result->code = ret;
      return ret;
    }

  response_buf = smart_home_bulk_alloc(ASR_RESPONSE_MAX);
  if (response_buf == NULL)
    {
      smart_home_bulk_free(body);
      return AGENT_ERROR_NOMEM;
    }

  url_len = (size_t)snprintf(url, sizeof(url), "https://%s:%s%s",
                             config.host, config.port, config.path);
  if (url_len >= sizeof(url))
    {
      fprintf(stderr, "asr: endpoint url too long\n");
      smart_home_bulk_free(response_buf);
      smart_home_bulk_free(body);
      return AGENT_ERROR_LIMIT;
    }

  snprintf(auth_value, sizeof(auth_value), "Bearer %s", api_key);
  headers[0].name = agent_string_view("Authorization", 13u);
  headers[0].value = agent_string_view(auth_value, strlen(auth_value));
  headers[1].name = agent_string_view("Content-Type", 12u);
  headers[1].value = agent_string_view("application/json", 16u);

  body_size = prefix_len + olen + strlen(body + prefix_len + olen);
  deadline_ms = g_asr_http.runtime.now_ms(g_asr_http.runtime.clock_context) +
                ASR_TIMEOUT_MS;

  memset(&request, 0, sizeof(request));
  request.method = agent_string_view("POST", 4u);
  request.url = agent_string_view(url, url_len);
  request.headers = headers;
  request.header_count = 2u;
  request.body = body;
  request.body_size = body_size;
  request.max_response_bytes = ASR_RESPONSE_MAX;
  request.max_response_header_bytes = ASR_HTTP_RESP_HEADER_BUF;
  request.deadline_ms = deadline_ms;
  request.cancel = NULL;

  http_sink.headers = asr_sink_headers;
  http_sink.body = asr_sink_body;
  http_sink.context = &sink;
  sink.buffer = response_buf;
  sink.capacity = ASR_RESPONSE_MAX;
  sink.used = 0;
  sink.status = 0;

  ret = g_asr_http.transport.ops->request(g_asr_http.transport.context,
                                          &request, &http_sink);
  smart_home_bulk_free(body);

  if (ret != AGENT_OK)
    {
      fprintf(stderr, "asr: http failed: %d\n", ret);
      goto out;
    }

  result->http_status = sink.status;
  result->stage = SMART_HOME_VOICE_HTTP;
  if (sink.status < 200 || sink.status >= 300)
    {
      fprintf(stderr, "asr: http status=%u body: %.200s\n",
              sink.status,
              sink.used > 0 ? (const char *)response_buf : "(empty)");
      ret = AGENT_ERROR_MODEL_FAILED;
      goto out;
    }

  if (sink.used >= ASR_RESPONSE_MAX)
    {
      sink.used = ASR_RESPONSE_MAX - 1;
    }

  response_buf[sink.used] = '\0';
  result->stage = SMART_HOME_VOICE_RESPONSE;
  ret = asr_parse_response((const char *)response_buf,
                           sink.used, out_text, out_size);
  if (ret != 0)
    {
      ret = AGENT_ERROR_PARSE;
    }

out:
  result->code = ret;
  smart_home_bulk_free(response_buf);
  return ret;
}

int smart_home_asr_recognize(const uint8_t *pcm, size_t bytes,
                             char *out_text, size_t out_size)
{
    return smart_home_asr_recognize_ex(pcm, bytes, out_text, out_size, NULL);
}
