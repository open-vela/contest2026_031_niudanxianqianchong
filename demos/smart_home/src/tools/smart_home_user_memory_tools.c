/* SPDX-License-Identifier: Apache-2.0 */
#include "smart_home_user_memory_tools.h"
#include "../agent/smart_home_tool_metadata.h"
#include "smart_home_tool_io.h"
#include <stdio.h>
#include <stdlib.h>

static const char schema[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"action\":{\"type\":\"string\",\"enum\":[\"remember\",\"list\",\"forget\"]},"
    "\"key\":{\"type\":\"string\",\"description\":\"Stable preference key, lowercase ASCII letters/digits/._-, max 48 bytes. Reuse an existing key for updates.\"},"
    "\"value\":{\"type\":\"string\",\"description\":\"Verbatim preference phrase copied from evidence, max 128 UTF-8 bytes.\"},"
    "\"evidence\":{\"type\":\"string\",\"description\":\"Exact quote from this turn's user input expressing a lasting preference, max 256 UTF-8 bytes.\"},"
    "\"offset\":{\"type\":\"integer\",\"minimum\":0,\"description\":\"List pagination offset, default 0. Follow next_offset until null.\"}},"
    "\"required\":[\"action\"],\"additionalProperties\":false}";

static const char *field(const cJSON *root, const char *name)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static agent_error_t execute(void *data, const agent_tool_context_t *context,
                              const agent_text_sink_t *sink)
{
    cJSON *root = smart_home_tool_parse(context->call.arguments_json);
    const char *action = field(root, "action");
    const char *key = field(root, "key");
    const char *value = field(root, "value");
    const char *evidence = field(root, "evidence");
    char output[256], *json = NULL;
    int changed = 0, ret = AGENT_ERROR_TOOL_ARGUMENT;
    if (!root || !action) goto reply;
    for (const cJSON *item = root->child; item; item = item->next) {
        if (!strcmp(item->string, "action")) continue;
        if (!strcmp(action, "remember") &&
            (!strcmp(item->string, "key") || !strcmp(item->string, "value") ||
             !strcmp(item->string, "evidence"))) continue;
        if (!strcmp(action, "forget") && !strcmp(item->string, "key")) continue;
        if (!strcmp(action, "list") && !strcmp(item->string, "offset")) continue;
        goto reply;
    }
    if (!strcmp(action, "remember") && key && value && evidence) {
        ret = smart_home_user_memory_remember(data, key, value, evidence, &changed);
    } else if (!strcmp(action, "forget") && key) {
        ret = smart_home_user_memory_forget(data, key, &changed);
    } else if (!strcmp(action, "list")) {
        size_t offset = 0;
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "offset");
        if (item) {
            if (!cJSON_IsNumber(item) ||
                item->valuedouble < 0 || item->valuedouble > INT_MAX ||
                item->valuedouble != (double)item->valueint) goto reply;
            offset = (size_t)item->valueint;
        }
        ret = smart_home_user_memory_list(data, offset, 2, AGENT_MAX_TOOL_OUTPUT_BYTES, &json);
        if (ret == AGENT_OK) {
            ret = smart_home_tool_reply(sink, json, AGENT_OK);
            free(json); cJSON_Delete(root);
            return (agent_error_t)ret;
        }
    }
 reply:
    snprintf(output, sizeof(output),
        "{\"ok\":%s,\"changed\":%s,\"code\":%d,\"error\":\"%s\"}",
        ret == AGENT_OK ? "true" : "false", changed ? "true" : "false", ret,
        ret == AGENT_OK ? "" : ret == AGENT_ERROR_POLICY_DENIED ?
        "need_current_user_long_term_preference_quote_or_explicit_forget_no_credentials" :
        ret == AGENT_ERROR_CAPACITY ? "memory_capacity_or_output_limit" :
        ret == AGENT_ERROR_INVALID || ret == AGENT_ERROR_TOOL_ARGUMENT ?
        "invalid_action_or_fields" : "memory_unavailable_or_save_failed");
    cJSON_Delete(root);
    return smart_home_tool_reply(sink, output, (agent_error_t)ret);
}

int smart_home_user_memory_tools_register(agent_t *agent, smart_home_user_memory_t *memory)
{
    static const char description[] =
        "Remember explicit lasting user preferences with a quote from the current user message; "
        "list saved preferences or forget a key on explicit request. Never save temporary wishes or credentials. "
        "Saving preferences never controls devices.";
    const agent_tool_t tool = {
        .name = AGENT_SV_LITERAL("user_memory"),
        .description = AGENT_SV_LITERAL(description),
        .input_schema_json = AGENT_SV_LITERAL(schema),
        .group = AGENT_SV_LITERAL(SMART_HOME_TOOL_GROUP_LOCAL_NAME),
        .category = AGENT_SV_LITERAL(SMART_HOME_TOOL_CATEGORY_CONTROL_NAME),
        .validate = smart_home_tool_validate_object, .execute = execute,
        .user_data = memory, .flags = AGENT_TOOL_SIDE_EFFECT
    };
    return agent_register_tool(agent, &tool);
}
