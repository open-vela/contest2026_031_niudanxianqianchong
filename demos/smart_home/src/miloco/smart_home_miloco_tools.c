/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Miloco 网关 Agent 工具：模型可通过 miot_device_list 查看米家设备，
 * 通过 miot_device_control 下发电源控制。
 */

#include "smart_home_miloco.h"

#include "../agent/smart_home_agent.h"
#include "../config/cjson_compat.h"

#include <cagent/types.h>
#include <cagent/tools.h>

#include <stdio.h>
#include <string.h>

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

static int miot_device_list_tool(const agent_tool_call_t *call,
                                 agent_tool_result_t *result,
                                 void *user_data)
{
    static char output[MILOCO_TOOL_LIST_BUFFER_BYTES];
    smart_home_agent_app_t *app = user_data;
    smart_home_miloco_device_t devices[SMART_HOME_MILOCO_MAX_DEVICES];
    size_t count;
    size_t used;
    size_t i;
    uint8_t k;

    (void)call;
    result->status = AGENT_ERROR;
    result->content_json = output;
    result->error_message = "gateway unavailable";
    if (!app || !app->miloco) {
        snprintf(output, sizeof(output),
                 "{\"ok\":false,\"error\":\"miloco_gateway_not_configured\"}");
        return result->status;
    }
    if (!smart_home_miloco_reachable(app->miloco)) {
        snprintf(output, sizeof(output),
                 "{\"ok\":false,\"error\":\"miloco_gateway_unreachable\"}");
        return result->status;
    }

    count = smart_home_miloco_list(app->miloco, devices,
                                   SMART_HOME_MILOCO_MAX_DEVICES, NULL);
    used = (size_t)snprintf(output, sizeof(output),
                            "{\"ok\":true,\"source\":\"miloco\",\"devices\":[");
    for (i = 0; i < count && used < sizeof(output) - 256u; i++) {
        used += (size_t)snprintf(
            output + used, sizeof(output) - used,
            "%s{\"did\":\"%s\",\"name\":\"%s\",\"room\":\"%s\","
            "\"category\":\"%s\",\"online\":%s,\"controls\":[",
            i > 0 ? "," : "", devices[i].did, devices[i].name,
            devices[i].room, category_name(devices[i].category),
            devices[i].online ? "true" : "false");
        for (k = 0; k < devices[i].control_count &&
                    used < sizeof(output) - 192u; k++) {
            const smart_home_miloco_control_t *ctrl =
                &devices[i].controls[k];

            used += (size_t)snprintf(
                output + used, sizeof(output) - used,
                "%s{\"iid\":\"%s\",\"op\":\"%s\",\"desc\":\"%s\"}",
                k > 0 ? "," : "", ctrl->iid,
                ctrl->type == SMART_HOME_MILOCO_CTRL_ACTION ?
                    "action" : "set",
                ctrl->desc);
        }
        used += (size_t)snprintf(output + used, sizeof(output) - used,
                                 "]}");
    }
    snprintf(output + used, sizeof(output) - used, "]}");
    result->status = AGENT_OK;
    result->error_message = NULL;
    return AGENT_OK;
}

static int miot_device_control_tool(const agent_tool_call_t *call,
                                    agent_tool_result_t *result,
                                    void *user_data)
{
    static char output[192];
    smart_home_agent_app_t *app = user_data;
    cJSON *root;
    const cJSON *did;
    const cJSON *iid;
    const cJSON *value;
    const cJSON *operation;

    result->status = AGENT_ERROR;
    result->content_json = output;
    result->error_message = "control failed";
    if (!app || !app->miloco || !call || !call->arguments_json) {
        snprintf(output, sizeof(output),
                 "{\"ok\":false,\"error\":\"miloco_gateway_not_configured\"}");
        return result->status;
    }

    root = cJSON_Parse(call->arguments_json);
    if (!root) {
        snprintf(output, sizeof(output),
                 "{\"ok\":false,\"error\":\"invalid_arguments\"}");
        return result->status;
    }
    did = cJSON_GetObjectItemCaseSensitive(root, "did");
    iid = cJSON_GetObjectItemCaseSensitive(root, "iid");
    value = cJSON_GetObjectItemCaseSensitive(root, "value");
    operation = cJSON_GetObjectItemCaseSensitive(root, "operation");
    if (!cJSON_IsString(did) || !did->valuestring[0] ||
        !cJSON_IsString(iid) || !iid->valuestring[0]) {
        cJSON_Delete(root);
        snprintf(output, sizeof(output),
                 "{\"ok\":false,\"error\":\"invalid_arguments\","
                 "\"hint\":\"run miot_device_list first to get valid iid\"}");
        return result->status;
    }

    /* did/iid 的 valuestring 挂在 cJSON 树上，cJSON_Delete 后即释放。
     * 曾在 delete 之后才格式化回执，读已释放内存（UAF）——free-list
     * 指针字节（0x48b4.. 含孤立续字节）随会话进入请求体，被 LLM
     * 服务端以 400 invalid unicode 拒绝，且间歇复现。先拷贝再释放。 */
    {
        char did_copy[24];
        char iid_copy[16];

        snprintf(did_copy, sizeof(did_copy), "%s", did->valuestring);
        snprintf(iid_copy, sizeof(iid_copy), "%s", iid->valuestring);

        result->status = smart_home_miloco_submit_control(
            app->miloco, did_copy, iid_copy,
            (cJSON_IsString(operation) &&
             strcmp(operation->valuestring, "action") == 0) ?
                "action" : "set",
            cJSON_IsNumber(value) ? (int32_t)value->valueint :
            cJSON_IsTrue(value) ? 1 : 0);
        cJSON_Delete(root);
        root = NULL;

        if (result->status != AGENT_OK) {
            snprintf(output, sizeof(output),
                     "{\"ok\":false,\"error\":\"submit_failed\",\"code\":%d,"
                     "\"hint\":\"iid must come from miot_device_list "
                     "controls\"}",
                     result->status);
            result->status = AGENT_ERROR;
            return result->status;
        }
        snprintf(output, sizeof(output),
                 "{\"ok\":true,\"did\":\"%s\",\"iid\":\"%s\","
                 "\"note\":\"control submitted; state updates on next "
                 "poll\"}",
                 did_copy, iid_copy);
    }
    result->status = AGENT_OK;
    result->error_message = NULL;
    return result->status;
}

static int register_tool(agent_t *agent,
                         const char *name,
                         const char *description,
                         const char *input_schema_json,
                         agent_tool_fn execute,
                         void *user_data,
                         uint32_t flags)
{
    agent_tool_t tool = {0};

    tool.name = name;
    tool.description = description;
    tool.input_schema_json = input_schema_json;
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
                        AGENT_TOOL_FLAG_LLM_VISIBLE |
                            AGENT_TOOL_FLAG_READ_ONLY);
    if (ret != AGENT_OK) {
        return ret;
    }
    return register_tool(agent, "miot_device_control",
                         "Unified Mi Home device control through the "
                         "Miloco gateway: write a property (operation=set) "
                         "or call an action (operation=action). Valid iid "
                         "values are listed per device in the controls "
                         "array of miot_device_list.",
                         SCHEMA_MIOT_DEVICE_CONTROL,
                         miot_device_control_tool, app,
                         AGENT_TOOL_FLAG_LLM_VISIBLE);
}
