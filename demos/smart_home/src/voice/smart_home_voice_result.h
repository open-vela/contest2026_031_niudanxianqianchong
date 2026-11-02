/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <agent/error.h>
#include "../config/smart_home_secrets.h"

typedef enum {
    SMART_HOME_VOICE_CAPTURE,
    SMART_HOME_VOICE_CREDENTIAL,
    SMART_HOME_VOICE_NETWORK,
    SMART_HOME_VOICE_HTTP,
    SMART_HOME_VOICE_RESPONSE
} smart_home_voice_stage_t;

typedef struct {
    int code;
    unsigned int http_status;
    smart_home_voice_stage_t stage;
} smart_home_voice_result_t;

static inline const char *smart_home_voice_result_text(
    const smart_home_voice_result_t *result)
{
    if (!result) return "语音处理失败";
    if (result->stage == SMART_HOME_VOICE_CREDENTIAL)
        return smart_home_secrets_error_text(result->code);
    if (result->stage == SMART_HOME_VOICE_CAPTURE) return "录音失败或没有有效语音";
    if (result->stage == SMART_HOME_VOICE_NETWORK)
        return "网络请求失败或超时，请检查 DNS/TLS 连接";
    if (result->stage == SMART_HOME_VOICE_HTTP) {
        if (result->http_status == 401 || result->http_status == 403)
            return "云端鉴权或访问权限失败，请检查本服务密钥";
        if (result->http_status == 429) return "云端限流或额度不足";
        return "云端服务返回错误";
    }
    return "云端结果为空或响应格式不支持";
}
