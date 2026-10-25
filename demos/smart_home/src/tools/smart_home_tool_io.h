/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <agent.h>
#include "../config/cjson_compat.h"
#include <limits.h>
#include <string.h>

static inline cJSON *smart_home_tool_parse(agent_string_view_t json)
{
    const char *end;
    cJSON *root;
    const cJSON *item;

    if (!json.data || !json.size || memchr(json.data, '\0', json.size))
        return NULL;
    root = cJSON_ParseWithLengthOpts(json.data, json.size, &end, 0);
    if (!root) return NULL;
    while (end < json.data + json.size &&
           (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) ++end;
    if (end != json.data + json.size || !cJSON_IsObject(root)) goto invalid;
    for (item = root->child; item; item = item->next) {
        const cJSON *other;
        for (other = item->next; other; other = other->next)
            if (!strcmp(item->string, other->string)) goto invalid;
    }
    return root;
invalid:
    cJSON_Delete(root);
    return NULL;
}

static inline agent_error_t smart_home_tool_validate_object(
    void *user_data, const agent_tool_context_t *context)
{
    cJSON *root;
    (void)user_data;
    if (!context) return AGENT_ERROR_TOOL_ARGUMENT;
    root = smart_home_tool_parse(context->call.arguments_json);
    if (!root) return AGENT_ERROR_TOOL_ARGUMENT;
    cJSON_Delete(root);
    return AGENT_OK;
}

static inline agent_error_t smart_home_tool_get_string(
    agent_string_view_t json, const char *key, char *out, size_t capacity)
{
    cJSON *root = smart_home_tool_parse(json);
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    agent_error_t status = !root ? AGENT_ERROR_TOOL_ARGUMENT :
        !item ? AGENT_ERROR_NOT_FOUND : AGENT_ERROR_TOOL_ARGUMENT;
    if (cJSON_IsString(item) && out && capacity &&
        strlen(item->valuestring) < capacity) {
        strcpy(out, item->valuestring);
        status = AGENT_OK;
    }
    cJSON_Delete(root);
    return status;
}

static inline agent_error_t smart_home_tool_get_int(
    agent_string_view_t json, const char *key, int *out)
{
    cJSON *root = smart_home_tool_parse(json);
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    agent_error_t status = !root ? AGENT_ERROR_TOOL_ARGUMENT :
        !item ? AGENT_ERROR_NOT_FOUND : AGENT_ERROR_TOOL_ARGUMENT;
    if (out && cJSON_IsNumber(item) && item->valuedouble >= INT_MIN &&
        item->valuedouble <= INT_MAX &&
        item->valuedouble == (double)(int)item->valuedouble) {
        *out = (int)item->valuedouble;
        status = AGENT_OK;
    }
    cJSON_Delete(root);
    return status;
}

static inline agent_error_t smart_home_tool_get_bool(
    agent_string_view_t json, const char *key, int *out)
{
    cJSON *root = smart_home_tool_parse(json);
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    agent_error_t status = !root ? AGENT_ERROR_TOOL_ARGUMENT :
        !item ? AGENT_ERROR_NOT_FOUND : AGENT_ERROR_TOOL_ARGUMENT;
    if (out && cJSON_IsBool(item)) {
        *out = cJSON_IsTrue(item);
        status = AGENT_OK;
    }
    cJSON_Delete(root);
    return status;
}

static inline agent_error_t smart_home_tool_reply(
    const agent_text_sink_t *sink, const char *text, agent_error_t status)
{
    agent_error_t written;
    if (!sink || !sink->write || !text) return AGENT_ERROR_INVALID;
    written = sink->write(sink->context, agent_string_view(text, strlen(text)));
    return written == AGENT_OK ? status : written;
}
