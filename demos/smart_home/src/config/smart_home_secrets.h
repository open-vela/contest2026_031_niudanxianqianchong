/* SPDX-License-Identifier: Apache-2.0 */
/**
 * secrets.json 凭据加载与受控更新。
 *
 * 模型、ASR、TTS 凭据按服务隔离；Wi-Fi 凭据由网络设置页受控更新。调用者按 backend_id
 * 请求 API key，避免切换模型时复用其他后端的凭据。
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CONFIG_SMART_HOME_MODEL_SECRETS_PATH
#define CONFIG_SMART_HOME_MODEL_SECRETS_PATH "/data/smart_home/secrets.json"
#endif

#ifndef CONFIG_SMART_HOME_MODEL_SECRETS_MAX_FILE_SIZE
#define CONFIG_SMART_HOME_MODEL_SECRETS_MAX_FILE_SIZE 4096u
#endif

typedef enum {
    SMART_HOME_SECRET_LLM = 0,
    SMART_HOME_SECRET_ASR,
    SMART_HOME_SECRET_TTS
} smart_home_secret_service_t;

/* No cross-service fallback, even when the backend IDs are identical. */
const char *smart_home_secrets_key_section(smart_home_secret_service_t service);
const char *smart_home_secrets_error_text(int result);
int smart_home_secrets_get_api_key(smart_home_secret_service_t service,
                                   const char *backend_id,
                                   char *out, size_t out_size);
int smart_home_secrets_api_key_status(smart_home_secret_service_t service,
                                      const char *backend_id);
/* Empty key explicitly clears this service/backend entry. Other fields survive. */
int smart_home_secrets_set_api_key(smart_home_secret_service_t service,
                                   const char *backend_id, const char *key);

/**
 * 从 secrets.json 的 model_api_keys 对象中读取 backend_id 对应的密钥。
 *
 * 成功返回 AGENT_OK。文件/条目不存在或值为空返回 AGENT_ERROR_NOT_FOUND；
 * 非法 JSON、版本或值返回 AGENT_ERROR_PARSE；过大输入/输出缓冲不足返回
 * AGENT_ERROR_LIMIT。任何失败都会清空 out，且不会回退到源码默认值。
 */
int smart_home_secrets_get_model_api_key(const char *backend_id,
                                         char *out,
                                         size_t out_size);

/**
 * 检查一个后端是否拥有可用密钥，不向调用者暴露密钥内容。
 * 返回值与 smart_home_secrets_get_model_api_key 一致。
 */
int smart_home_secrets_model_api_key_status(const char *backend_id);

/* Wi-Fi credentials are sensitive runtime input.  They are stored only in
 * secrets.json, never in settings.json or a tracked board defconfig. */
int smart_home_secrets_get_wifi_credentials(char *ssid, size_t ssid_size,
                                            char *password,
                                            size_t password_size);
int smart_home_secrets_set_wifi_credentials(const char *ssid,
                                            const char *password);

/* Miloco 米家网关（家庭服务器）配置。token 可为空表示无鉴权。 */
#ifndef SMART_HOME_MILOCO_DEFAULT_PORT_SECRET
#define SMART_HOME_MILOCO_DEFAULT_PORT_SECRET 1810
#endif
int smart_home_secrets_get_miloco(char *host, size_t host_size,
                                  uint16_t *port,
                                  char *token, size_t token_size);
int smart_home_secrets_set_miloco(const char *host, uint16_t port,
                                  const char *token);

#ifdef __cplusplus
}
#endif
