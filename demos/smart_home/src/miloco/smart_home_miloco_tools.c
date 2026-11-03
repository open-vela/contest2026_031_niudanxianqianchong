/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Miloco 网关 Agent 工具：模型可通过 miot_device_list 查看米家设备，
 * 通过 miot_device_control 下发电源控制。
 */

#include "smart_home_miloco.h"
#include "smart_home_miloco_protocol.h"

#include "../agent/smart_home_agent.h"
#include "../config/cjson_compat.h"
#include "../smart_home_memory.h"

#include <agent.h>
#include "../tools/smart_home_tool_io.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#define MILOCO_TOOL_LIST_BUFFER_BYTES 4096

#define SCHEMA_MIOT_DEVICE_LIST \
    "{\"type\":\"object\",\"properties\":{\"did\":{\"type\":\"string\",\"description\":\"Optional device filter from get_device_inventory\"},\"control_offset\":{\"type\":\"integer\",\"description\":\"Requires did; continue at next_control_offset when controls are truncated\"}},\"additionalProperties\":false}"

#define SCHEMA_MIOT_DEVICE_CONTROL                                      \
    "{\"type\":\"object\",\"properties\":"                               \
    "{\"did\":{\"type\":\"string\",\"description\":\"Mi Home device id\"}," \
    "\"device_key\":{\"type\":\"string\",\"description\":\"milo:<did>, from discovery; never board/local/node\"}," \
    "\"iid\":{\"type\":\"string\",\"description\":\"control id from miot_device_list controls, e.g. prop.2.1\"}," \
    "\"operation\":{\"type\":\"string\",\"enum\":[\"set\",\"action\"],\"description\":\"set=write property, action=call action\"}," \
    "\"value\":{\"type\":[\"integer\",\"boolean\"],\"description\":\"value to write; omit for action\"}}," \
    "\"required\":[\"device_key\",\"did\",\"iid\"]}"

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

static cJSON *control_json(const smart_home_miloco_control_t *ctrl)
{
    const char *type = ctrl->type == SMART_HOME_MILOCO_CTRL_BOOL ? "bool" :
        ctrl->type == SMART_HOME_MILOCO_CTRL_ENUM ? "enum" :
        ctrl->type == SMART_HOME_MILOCO_CTRL_RANGE ? "range" : "action";
    cJSON *item = cJSON_CreateObject();
    if (!item || !cJSON_AddStringToObject(item, "iid", ctrl->iid) ||
        !cJSON_AddStringToObject(item, "op", ctrl->type == SMART_HOME_MILOCO_CTRL_ACTION ? "action" : "set") ||
        !cJSON_AddStringToObject(item, "type", type) ||
        !cJSON_AddStringToObject(item, "type_name", ctrl->type_name) ||
        !cJSON_AddStringToObject(item, "desc", ctrl->desc) ||
        !cJSON_AddBoolToObject(item, "readable", !ctrl->write_only && ctrl->type != SMART_HOME_MILOCO_CTRL_ACTION) ||
        !cJSON_AddBoolToObject(item, "value_valid", ctrl->value_valid)) goto fail;
    if (ctrl->value_valid && !cJSON_AddNumberToObject(item, "value", ctrl->value)) goto fail;
    if (ctrl->type == SMART_HOME_MILOCO_CTRL_ENUM) {
        cJSON *options = cJSON_AddArrayToObject(item, "options");
        if (!options) goto fail;
        for (unsigned i = 0; i < ctrl->option_count; i++) {
            cJSON *option = cJSON_CreateObject();
            if (!option || !cJSON_AddStringToObject(option, "name", ctrl->options[i].name) ||
                !cJSON_AddNumberToObject(option, "value", ctrl->options[i].value) ||
                !cJSON_AddItemToArray(options, option)) {
                cJSON_Delete(option); goto fail;
            }
        }
    } else if (ctrl->type == SMART_HOME_MILOCO_CTRL_RANGE) {
        if (!cJSON_AddNumberToObject(item, "minimum", ctrl->range.minimum) ||
            !cJSON_AddNumberToObject(item, "maximum", ctrl->range.maximum) ||
            !cJSON_AddNumberToObject(item, "step", ctrl->range.step)) goto fail;
    }
    return item;
fail:
    cJSON_Delete(item);
    return NULL;
}

static int list_fits(const cJSON *root)
{
    char *text = cJSON_PrintUnformatted(root);
    if (!text) return -1;
    size_t limit = AGENT_MAX_TOOL_OUTPUT_BYTES < MILOCO_TOOL_LIST_BUFFER_BYTES ?
        AGENT_MAX_TOOL_OUTPUT_BYTES : MILOCO_TOOL_LIST_BUFFER_BYTES;
    int fits = strlen(text) + 32u < limit;
    cJSON_free(text);
    return fits;
}

static agent_error_t miot_device_list_tool(void *user_data,
                                         const agent_tool_context_t *context,
                                         const agent_text_sink_t *sink)
{
    smart_home_agent_app_t *app = user_data;
    smart_home_miloco_device_t *devices = NULL;
    cJSON *root = NULL, *list;
    char filter[24] = "", key[40];
    int offset = 0;
    char *text = NULL;
    int ret = smart_home_tool_get_string(context->call.arguments_json, "did", filter, sizeof(filter));
    if (ret != AGENT_OK && ret != AGENT_ERROR_NOT_FOUND)
        return smart_home_tool_reply(sink, "{\"ok\":false,\"error\":\"invalid_filter\"}", AGENT_ERROR_TOOL_ARGUMENT);
    ret = smart_home_tool_get_int(context->call.arguments_json, "control_offset", &offset);
    if ((ret != AGENT_OK && ret != AGENT_ERROR_NOT_FOUND) || offset < 0 ||
        offset > SMART_HOME_MILOCO_MAX_CONTROLS || (offset && !filter[0]))
        return smart_home_tool_reply(sink, "{\"ok\":false,\"error\":\"invalid_offset\"}", AGENT_ERROR_TOOL_ARGUMENT);
    if (!app || !app->miloco || !smart_home_miloco_reachable(app->miloco))
        return smart_home_tool_reply(sink, "{\"ok\":false,\"source\":\"mijia\",\"error\":\"gateway_unavailable\"}", AGENT_ERROR_IO);
    devices = smart_home_bulk_alloc(sizeof(*devices) * SMART_HOME_MILOCO_MAX_DEVICES);
    if (!devices) return AGENT_ERROR_NOMEM;
    size_t count = smart_home_miloco_list(app->miloco, devices, SMART_HOME_MILOCO_MAX_DEVICES, NULL);
    root = cJSON_CreateObject();
    ret = AGENT_ERROR_NOMEM;
    if (!root || !cJSON_AddBoolToObject(root, "ok", 1) ||
        !cJSON_AddStringToObject(root, "source", "mijia") ||
        !cJSON_AddBoolToObject(root, "truncated", 0) ||
        !cJSON_AddNumberToObject(root, "total_devices", count) ||
        !(list = cJSON_AddArrayToObject(root, "devices"))) goto out;
    for (size_t i = 0; i < count; i++) {
        if (filter[0] && strcmp(filter, devices[i].did)) continue;
        snprintf(key, sizeof(key), "milo:%s", devices[i].did);
        cJSON *device = cJSON_CreateObject(), *controls;
        if (!device || !cJSON_AddStringToObject(device, "device_key", key) ||
            !cJSON_AddStringToObject(device, "did", devices[i].did) ||
            !cJSON_AddStringToObject(device, "name", devices[i].name) ||
            !cJSON_AddStringToObject(device, "room", devices[i].room) ||
            !cJSON_AddStringToObject(device, "category", category_name(devices[i].category)) ||
            !cJSON_AddBoolToObject(device, "online", devices[i].online) ||
            !cJSON_AddNumberToObject(device, "next_control_offset", -1) ||
            !(controls = cJSON_AddArrayToObject(device, "controls")) ||
            !cJSON_AddItemToArray(list, device)) { cJSON_Delete(device); goto out; }
        int fits = list_fits(root);
        if (fits < 0) goto out;
        if (!fits) {
            cJSON_DeleteItemFromArray(list, cJSON_GetArraySize(list) - 1);
            cJSON_GetObjectItemCaseSensitive(root, "truncated")->type = cJSON_True;
            break;
        }
        for (unsigned k = (unsigned)offset; k < devices[i].control_count; k++) {
            cJSON *control = control_json(&devices[i].controls[k]);
            if (!control || !cJSON_AddItemToArray(controls, control)) { cJSON_Delete(control); goto out; }
            fits = list_fits(root);
            if (fits < 0) goto out;
            if (!fits) {
                cJSON_DeleteItemFromArray(controls, cJSON_GetArraySize(controls) - 1);
                cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(device, "next_control_offset"), k);
                cJSON_GetObjectItemCaseSensitive(root, "truncated")->type = cJSON_True;
                break;
            }
        }
    }
    text = cJSON_PrintUnformatted(root);
    ret = text ? smart_home_tool_reply(sink, text, AGENT_OK) : AGENT_ERROR_NOMEM;
out:
    cJSON_free(text);
    cJSON_Delete(root);
    smart_home_bulk_free(devices);
    return (agent_error_t)ret;
}

static agent_error_t miot_device_control_tool(void *user_data,
                                             const agent_tool_context_t *context,
                                             const agent_text_sink_t *sink)
{
    smart_home_agent_app_t *app = user_data;
    char did[24], iid[16], operation[16] = "set", key[48], expected_key[48];
    smart_home_miloco_device_t device;
    int value = 0;
    agent_error_t ret;
    cJSON *root;
    const cJSON *item;

    if (!app || !app->miloco)
        return smart_home_tool_reply(sink, "{\"ok\":false,\"error\":\"gateway_unavailable\"}", AGENT_ERROR_IO);
    if (smart_home_tool_get_string(context->call.arguments_json, "did", did, sizeof(did)) ||
        smart_home_tool_get_string(context->call.arguments_json, "iid", iid, sizeof(iid)) ||
        !did[0] || !iid[0]) goto invalid;
    snprintf(expected_key, sizeof(expected_key), "milo:%s", did);
    if (smart_home_tool_get_string(context->call.arguments_json, "device_key", key, sizeof(key)) ||
        strcmp(key, expected_key)) goto invalid;
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
    if (!smart_home_miloco_reachable(app->miloco) ||
        !smart_home_miloco_copy_device(app->miloco, did, &device) || !device.online)
        return smart_home_tool_reply(sink, "{\"ok\":false,\"source\":\"mijia\",\"error\":\"device_unavailable\"}", AGENT_ERROR_NOT_FOUND);
    bool allowed = false;
    for (unsigned i = 0; i < device.control_count; i++) {
        const smart_home_miloco_control_t *ctrl = &device.controls[i];
        if (strcmp(ctrl->iid, iid)) continue;
        allowed = !strcmp(operation, "action") ? ctrl->type == SMART_HOME_MILOCO_CTRL_ACTION :
            ctrl->type != SMART_HOME_MILOCO_CTRL_ACTION && smart_home_miloco_value_allowed(ctrl, value);
        break;
    }
    if (!allowed)
        return smart_home_tool_reply(sink, "{\"ok\":false,\"source\":\"mijia\",\"error\":\"unsupported_capability_or_value\"}", AGENT_ERROR_TOOL_ARGUMENT);
    if (smart_home_miloco_submit_control(app->miloco, did, iid, operation, value))
        return smart_home_tool_reply(sink, "{\"ok\":false,\"error\":\"submit_failed\"}", AGENT_ERROR_TOOL_FAILED);
    return smart_home_tool_reply(sink,
        "{\"ok\":true,\"source\":\"mijia\",\"accepted\":true,\"note\":\"control submitted; state updates on next poll\"}", AGENT_OK);
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
                        "home-server gateway. Use did to focus; follow next_control_offset if truncated. Includes exact control types, options and valid cached values.",
                        SCHEMA_MIOT_DEVICE_LIST,
                        miot_device_list_tool, app,
                        AGENT_TOOL_READ_ONLY);
    if (ret != AGENT_OK) {
        return ret;
    }
    return register_tool(agent, "miot_device_control",
                         "[mijia] Submit only a discovered Mi Home property write (set) or exact action. "
                         "Use did and iid from miot_device_list controls. "
                         "Success means queued; verify state on the next poll.",
                         SCHEMA_MIOT_DEVICE_CONTROL,
                         miot_device_control_tool, app,
                         AGENT_TOOL_SIDE_EFFECT);
}
