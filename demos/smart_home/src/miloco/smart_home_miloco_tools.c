/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Miloco 网关 Agent 工具：模型可通过 miot_device_list 查看米家设备，
 * 通过 miot_device_control 下发电源控制。
 */

#include "smart_home_miloco.h"

#include "../agent/smart_home_agent.h"
#include "../config/cjson_compat.h"

#include <agent.h>
#include "../tools/smart_home_tool_io.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#define MILOCO_TOOL_LIST_BUFFER_BYTES 4096

#define SCHEMA_MIOT_DEVICE_LIST \
    "{\"type\":\"object\",\"properties\":{}}"

#define SCHEMA_MIOT_DEVICE_CONTROL                                      \
    "{\"type\":\"object\",\"properties\":"                               \
    "{\"did\":{\"type\":\"string\",\"description\":\"Mi Home device id\"}," \
    "\"iid\":{\"type\":\"string\",\"description\":\"control id from miot_device_list controls, e.g. prop.2.1\"}," \
    "\"operation\":{\"type\":\"string\",\"enum\":[\"set\",\"action\"],\"description\":\"set=write property, action=call action\"}," \
    "\"value\":{\"type\":\"number\",\"description\":\"value to write; omit for action\"}}," \
    "\"required\":[\"did\",\"iid\"]}"

static const char *category_name(smart_home_miloco_category_t category)
{
    switch (category) {
    case SMART_HOME_MILOCO_CATEGORY_LIGHT:
        return "light";
    case SMART_HOME_MILOCO_CATEGORY_AC:
        return "air-conditioner";
    case SMART_HOME_MILOCO_CATEGORY_OUTLET:
        return "outlet";
    case SMART_HOME_MILOCO_CATEGORY_CAMERA:
        return "camera";
    case SMART_HOME_MILOCO_CATEGORY_FAN:
        return "fan";
    case SMART_HOME_MILOCO_CATEGORY_OTHER:
        return "other";
    default:
        return "unknown";
    }
}

/* 有界 JSON 转义拷贝（含 " 与 \ 及控制字符），返回写入长度；
 * 溢出时截断到 cap-1。 */
static size_t json_escape_bounded(char *dst, size_t cap, const char *src)
{
    size_t used = 0;

    while (*src && used + 1 < cap) {
        char c = *src++;

        if (c == '"' || c == '\\') {
            if (used + 2 >= cap) {
                break;
            }
            dst[used++] = '\\';
            dst[used++] = c;
        } else if ((unsigned char)c >= 0x20u) {
            dst[used++] = c;
        } else {
            dst[used++] = ' ';
        }
    }
    dst[used] = '\0';
    return used;
}

/* snprintf 计数安全包装：返回新 used；截断时钳制到 cap-1 并置
 * *truncated。修复原 used += snprintf(...) 在截断时返回期望长度
 * 导致 (sizeof-output - used) 下溢越界写的隐患。 */
static size_t append_count(char *buf, size_t cap, size_t used,
                           const char *fmt, ...) __attribute__((format(printf, 4, 5)));

static size_t append_count(char *buf, size_t cap, size_t used,
                           const char *fmt, ...)
{
    va_list ap;
    int n;

    if (used + 1 >= cap) {
        return used;
    }

    va_start(ap, fmt);
    n = vsnprintf(buf + used, cap - used, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return used;
    }
    if ((size_t)n >= cap - used) {
        return cap - 1;   /* 截断：停在缓冲末尾（NUL 已由 vsnprintf 写） */
    }
    return used + (size_t)n;
}

static agent_error_t miot_device_list_tool(void *user_data,
                                         const agent_tool_context_t *context,
                                         const agent_text_sink_t *sink)
{
    static char output[MILOCO_TOOL_LIST_BUFFER_BYTES];
    smart_home_agent_app_t *app = user_data;
    smart_home_miloco_device_t devices[SMART_HOME_MILOCO_MAX_DEVICES];
    size_t count;
    size_t used;
    size_t i;
    uint8_t k;

    agent_error_t status = AGENT_ERROR_IO;

    (void)context;
    if (!app || !app->miloco) {
        snprintf(output, sizeof(output),
                 "{\"ok\":false,\"error\":\"miloco_gateway_not_configured\"}");
        return smart_home_tool_reply(sink, output, status);
    }
    if (!smart_home_miloco_reachable(app->miloco)) {
        snprintf(output, sizeof(output),
                 "{\"ok\":false,\"error\":\"miloco_gateway_unreachable\"}");
        return smart_home_tool_reply(sink, output, status);
    }

    count = smart_home_miloco_list(app->miloco, devices,
                                   SMART_HOME_MILOCO_MAX_DEVICES, NULL);
    used = append_count(output, sizeof(output), 0,
                        "{\"ok\":true,\"source\":\"miloco\",\"devices\":[");
    for (i = 0; i < count && used < sizeof(output) - 256u; i++) {
        char name[88];
        char room[56];

        json_escape_bounded(name, sizeof(name), devices[i].name);
        json_escape_bounded(room, sizeof(room), devices[i].room);
        used = append_count(
            output, sizeof(output), used,
            "%s{\"did\":\"%s\",\"name\":\"%s\",\"room\":\"%s\","
            "\"category\":\"%s\",\"online\":%s,\"controls\":[",
            i > 0 ? "," : "", devices[i].did, name, room,
            category_name(devices[i].category),
            devices[i].online ? "true" : "false");
        for (k = 0; k < devices[i].control_count &&
                    used < sizeof(output) - 192u; k++) {
            const smart_home_miloco_control_t *ctrl =
                &devices[i].controls[k];
            char desc[52];

            json_escape_bounded(desc, sizeof(desc), ctrl->desc);
            used = append_count(
                output, sizeof(output), used,
                "%s{\"iid\":\"%s\",\"op\":\"%s\",\"desc\":\"%s\"}",
                k > 0 ? "," : "", ctrl->iid,
                ctrl->type == SMART_HOME_MILOCO_CTRL_ACTION ?
                    "action" : "set",
                desc);
        }
        used = append_count(output, sizeof(output), used, "]}");
    }
    used = append_count(output, sizeof(output), used, "]}");
    if (used >= sizeof(output) - 1u)
        return smart_home_tool_reply(sink, "{\"ok\":false,\"error\":\"device_list_capacity\"}", AGENT_ERROR_CAPACITY);
    return smart_home_tool_reply(sink, output, AGENT_OK);
}

static agent_error_t miot_device_control_tool(void *user_data,
                                             const agent_tool_context_t *context,
                                             const agent_text_sink_t *sink)
{
    smart_home_agent_app_t *app = user_data;
    char did[24], iid[16], operation[16] = "set";
    int value = 0;
    agent_error_t ret;
    cJSON *root;
    const cJSON *item;

    if (!app || !app->miloco)
        return smart_home_tool_reply(sink, "{\"ok\":false,\"error\":\"gateway_unavailable\"}", AGENT_ERROR_IO);
    if (smart_home_tool_get_string(context->call.arguments_json, "did", did, sizeof(did)) ||
        smart_home_tool_get_string(context->call.arguments_json, "iid", iid, sizeof(iid)) ||
        !did[0] || !iid[0]) goto invalid;
    ret = smart_home_tool_get_string(context->call.arguments_json, "operation", operation, sizeof(operation));
    if ((ret && ret != AGENT_ERROR_NOT_FOUND) ||
        (strcmp(operation, "set") && strcmp(operation, "action"))) goto invalid;
    root = smart_home_tool_parse(context->call.arguments_json);
    item = cJSON_GetObjectItemCaseSensitive(root, "value");
    if (!strcmp(operation, "set")) {
        if (cJSON_IsBool(item)) value = cJSON_IsTrue(item);
        else if (smart_home_tool_get_int(context->call.arguments_json, "value", &value)) {
            cJSON_Delete(root);
            goto invalid;
        }
    }
    cJSON_Delete(root);
    if (smart_home_miloco_submit_control(app->miloco, did, iid, operation, value))
        return smart_home_tool_reply(sink, "{\"ok\":false,\"error\":\"submit_failed\"}", AGENT_ERROR_TOOL_FAILED);
    return smart_home_tool_reply(sink,
        "{\"ok\":true,\"note\":\"control submitted; state updates on next poll\"}", AGENT_OK);
invalid:
    return smart_home_tool_reply(sink, "{\"ok\":false,\"error\":\"invalid_arguments\"}", AGENT_ERROR_TOOL_ARGUMENT);
}

static int register_tool(agent_t *agent,
                         const char *name,
                         const char *description,
                         const char *input_schema_json,
                         agent_tool_execute_fn execute,
                         void *user_data,
                         uint32_t flags)
{
    agent_tool_t tool = {0};

    tool.name = agent_string_view(name, strlen(name));
    tool.group = agent_string_view("local", 5u);
    tool.category = (flags & AGENT_TOOL_SIDE_EFFECT) ? agent_string_view("control", 7u) : agent_string_view("query", 5u);
    tool.description = agent_string_view(description, strlen(description));
    tool.input_schema_json = agent_string_view(input_schema_json, strlen(input_schema_json));
    tool.validate = smart_home_tool_validate_object;
    tool.execute = execute;
    tool.user_data = user_data;
    tool.flags = flags;
    return agent_register_tool(agent, &tool);
}

int smart_home_miloco_tools_register(agent_t *agent,
                                     smart_home_agent_app_t *app)
{
    int ret;

    if (!agent || !app) {
        return AGENT_ERROR_INVALID;
    }
    ret = register_tool(agent, "miot_device_list",
                        "List Mi Home devices bridged through the Miloco "
                        "home-server gateway, with online and power state.",
                        SCHEMA_MIOT_DEVICE_LIST,
                        miot_device_list_tool, app,
                        AGENT_TOOL_READ_ONLY);
    if (ret != AGENT_OK) {
        return ret;
    }
    return register_tool(agent, "miot_device_control",
                         "Submit a Mi Home property write (set) or action. "
                         "Use did and iid from miot_device_list controls. "
                         "Success means queued; verify state on the next poll.",
                         SCHEMA_MIOT_DEVICE_CONTROL,
                         miot_device_control_tool, app,
                         AGENT_TOOL_SIDE_EFFECT);
}
