/* SPDX-License-Identifier: Apache-2.0 */
#include "llm_internal.h"

#include <string.h>

static const char *after_prefix(const char *text, const char *prefix)
{
    size_t len;

    if (!text || !prefix) {
        return NULL;
    }

    len = strlen(prefix);
    return strncmp(text, prefix, len) == 0 ? text + len : NULL;
}

static int parse_tool_pipe(const char *text, agent_llm_parse_result_t *result)
{
    const char *id;
    const char *name;
    const char *args;
    const char *sep;
    static char id_buf[64];
    static char name_buf[64];
    size_t len;

    id = after_prefix(text, "tool|");
    if (!id) {
        return AGENT_ERROR_PARSE;
    }

    sep = strchr(id, '|');
    if (!sep) {
        return AGENT_ERROR_PARSE;
    }
    len = (size_t)(sep - id);
    if (len == 0u || len >= sizeof(id_buf)) {
        return AGENT_ERROR_PARSE;
    }
    memcpy(id_buf, id, len);
    id_buf[len] = '\0';

    name = sep + 1;
    sep = strchr(name, '|');
    if (!sep) {
        return AGENT_ERROR_PARSE;
    }
    len = (size_t)(sep - name);
    if (len == 0u || len >= sizeof(name_buf)) {
        return AGENT_ERROR_PARSE;
    }
    memcpy(name_buf, name, len);
    name_buf[len] = '\0';

    args = sep + 1;
    result->tool_calls[0].id = id_buf;
    result->tool_calls[0].name = name_buf;
    result->tool_calls[0].arguments_json = args[0] ? args : "{}";
    result->tool_call_count = 1u;
    return AGENT_OK;
}

static const char *find_json_string_value(const char *json, const char *key)
{
    const char *p;

    p = strstr(json, key);
    if (!p) {
        return NULL;
    }
    p += strlen(key);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ':') {
        p++;
    }
    return *p == '"' ? p + 1 : NULL;
}

int llm_parse_response_text(const char *text, agent_llm_parse_result_t *result)
{
    const char *content;

    if (!text || !result) {
        return AGENT_ERROR_INVALID;
    }

    memset(result, 0, sizeof(*result));

    content = after_prefix(text, "final|");
    if (content) {
        result->content = content;
        return AGENT_OK;
    }

    if (after_prefix(text, "tool|")) {
        return parse_tool_pipe(text, result);
    }

    content = find_json_string_value(text, "\"content\"");
    if (content) {
        /* 4096 对齐 session content 与 max_output_tokens=768 的中文
         * 上限（~2.3KB）；512 曾把回复切半个汉字，随会话历史进入
         * 下轮请求被 LLM 服务端以 invalid unicode 拒绝（400）。 */
        static char content_buf[4096];
        size_t i = 0u;

        while (content[i] && content[i] != '"' && i + 1u < sizeof(content_buf)) {
            content_buf[i] = content[i];
            i++;
        }
        /* UTF-8 字符边界回退（4096 截断可能切半个汉字）：
          * 先退到续字节序列的开头，再看首字节的期望长度是否被截断。
          * 判据 (b & 0xC0)==0x80 仅匹配续字节(10xxxxxx)——
          * 曾误写 (b & 0x80)!=0 导致纯中文被整句删空。 */
        if (i > 0 && ((unsigned char)content_buf[i - 1] & 0x80) != 0u) {
            size_t s = i;

            /* 退掉尾部的续字节 */
            while (s > 0 &&
                   ((unsigned char)content_buf[s - 1] & 0xC0) == 0x80u) {
                s--;
            }
            /* s 指向首字节（或 0）：判断该字符是否完整 */
            if (s > 0) {
                unsigned char lead = (unsigned char)content_buf[s - 1];
                size_t need = (lead >= 0xF0u) ? 4u :
                             (lead >= 0xE0u) ? 3u :
                             (lead >= 0xC0u) ? 2u : 1u;

                if (s - 1 + need > i) {
                    i = s - 1;   /* 该字符不完整，整体丢弃 */
                }
            } else {
                i = 0;   /* 整串都是非 ASCII 且无法定位首字节 */
            }
        }
        content_buf[i] = '\0';
        result->content = content_buf;
        return AGENT_OK;
    }

    return AGENT_ERROR_PARSE;
}
