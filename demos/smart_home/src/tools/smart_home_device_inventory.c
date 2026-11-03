/* SPDX-License-Identifier: Apache-2.0 */
#include "smart_home_tools.h"
#include "smart_home_tool_io.h"
#include "../agent/smart_home_agent.h"
#include "../smart_home_memory.h"
#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
#include "../miloco/smart_home_miloco.h"
#endif
#ifdef CONFIG_SMART_HOME_CAMERA_TOOL
#include "../camera/smart_home_camera_service.h"
#endif
#include <stdio.h>
#include <stdlib.h>

static const char inventory_schema[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"source\":{\"type\":\"string\",\"enum\":[\"all\",\"board\",\"demo\",\"mijia\",\"node\",\"mcp\"]},"
    "\"kind\":{\"type\":\"string\",\"description\":\"Optional device type, e.g. camera\"}},"
    "\"additionalProperties\":false}";

/* Registry enumeration is forbidden inside a Core tool callback. Snapshot it
 * under agent_mutex before the turn, while remote catalog changes are excluded. */
static agent_error_t collect_route(void *data, const agent_tool_t *tool)
{
    smart_home_agent_app_t *app = data;
    uint16_t group = smart_home_tool_group_id(tool->group);
    if (group != SMART_HOME_TOOL_GROUP_NODE && group != SMART_HOME_TOOL_GROUP_MCP)
        return AGENT_OK;
    if (tool->flags & (AGENT_TOOL_DISABLED | AGENT_TOOL_HIDDEN)) return AGENT_OK;
    if (app->remote_route_count >= AGENT_MAX_TOOLS ||
        tool->name.size >= sizeof(app->remote_routes[0].name)) return AGENT_ERROR_LIMIT;
    size_t i = app->remote_route_count++;
    memcpy(app->remote_routes[i].name, tool->name.data, tool->name.size);
    app->remote_routes[i].name[tool->name.size] = '\0';
    app->remote_routes[i].group = group;
    app->remote_routes[i].read_only = (tool->flags & AGENT_TOOL_READ_ONLY) != 0;
    return AGENT_OK;
}

int smart_home_device_inventory_prepare(smart_home_agent_app_t *app)
{
    app->remote_route_count = 0;
    return agent_tool_enumerate(app->agent, collect_route, app);
}

static cJSON *entry(const char *source, const char *key, const char *name,
                    const char *kind, const char *detail)
{
    cJSON *item = cJSON_CreateObject();
    if (item && cJSON_AddStringToObject(item, "source", source) &&
        cJSON_AddStringToObject(item, "device_key", key) &&
        cJSON_AddStringToObject(item, "name", name) &&
        cJSON_AddStringToObject(item, "kind", kind) &&
        cJSON_AddStringToObject(item, "detail_tool", detail)) return item;
    cJSON_Delete(item);
    return NULL;
}

/* Keep every response valid JSON and explicitly report omissions. */
static int append_entry(cJSON *root, cJSON *array, cJSON *item)
{
    char *text;
    if (!item || !cJSON_AddItemToArray(array, item)) {
        cJSON_Delete(item);
        return AGENT_ERROR_NOMEM;
    }
    text = cJSON_PrintUnformatted(root);
    if (!text) return AGENT_ERROR_NOMEM;
    size_t length = strlen(text);
    cJSON_free(text);
    if (length + 32u >= AGENT_MAX_TOOL_OUTPUT_BYTES) {
        cJSON_DeleteItemFromArray(array, cJSON_GetArraySize(array) - 1);
        cJSON_GetObjectItemCaseSensitive(root, "truncated")->type = cJSON_True;
    }
    return AGENT_OK;
}

static int matches(const char *selected, const char *value)
{
    return !strcmp(selected, "all") || !strcmp(selected, value);
}

static agent_error_t inventory_tool(void *data, const agent_tool_context_t *context,
                                     const agent_text_sink_t *sink)
{
    smart_home_agent_app_t *app = data;
    char source[12] = "all", kind[32] = "all";
    cJSON *root = NULL, *devices, *routes;
    char *text = NULL;
    int ret = smart_home_tool_get_string(context->call.arguments_json,
                                          "source", source, sizeof(source));
    if (ret != AGENT_OK && ret != AGENT_ERROR_NOT_FOUND) goto invalid;
    if (strcmp(source, "all") && strcmp(source, "board") && strcmp(source, "demo") &&
        strcmp(source, "mijia") && strcmp(source, "node") && strcmp(source, "mcp")) goto invalid;
    ret = smart_home_tool_get_string(context->call.arguments_json, "kind", kind, sizeof(kind));
    if (ret != AGENT_OK && ret != AGENT_ERROR_NOT_FOUND) goto invalid;
    root = cJSON_CreateObject();
    if (!root || !cJSON_AddBoolToObject(root, "ok", 1) ||
        !cJSON_AddBoolToObject(root, "cached", 1) ||
        !cJSON_AddBoolToObject(root, "truncated", 0) ||
        !(devices = cJSON_AddArrayToObject(root, "devices")) ||
        !(routes = cJSON_AddArrayToObject(root, "remote_tools"))) goto nomem;
    if (matches(source, "demo")) {
        smart_home_state_t *state = smart_home_bulk_alloc(sizeof(*state));
        if (!state) goto nomem;
        ret = smart_home_device_service_copy_state(&app->device_service, state);
        if (ret == AGENT_OK) for (unsigned i = 0; i < SMART_HOME_MAX_DEVICES; i++) {
            const smart_home_device_t *d = &state->devices[i];
            const char *type = d->type == SMART_HOME_DEVICE_LIGHT ? "light" : "air-conditioner";
            char key[32];
            if (!d->used || !matches(kind, type)) continue;
            snprintf(key, sizeof(key), "local:%d", d->id);
            cJSON *item = entry("demo", key, d->name, type, "get_home_status");
            if (!item || !cJSON_AddNumberToObject(item, "device_id", d->id) ||
                !cJSON_AddStringToObject(item, "room", d->room)) {
                cJSON_Delete(item); ret = AGENT_ERROR_NOMEM; break;
            }
            ret = append_entry(root, devices, item);
            if (ret != AGENT_OK) break;
        }
        smart_home_bulk_free(state);
        if (ret != AGENT_OK) goto out;
    }
#ifdef CONFIG_SMART_HOME_CAMERA_TOOL
    if (matches(source, "board") && matches(kind, "camera")) {
        struct smart_home_camera_status_s status;
        cJSON *item = entry("board", SMART_HOME_BOARD_CAMERA_KEY, "本机摄像头", "camera", "board_camera");
        int status_ret = smart_home_camera_get_status(&status);
        if (!item || !cJSON_AddStringToObject(item, "actions", "status,on,off,shot") ||
            !cJSON_AddBoolToObject(item, "ptz_supported", 0) ||
            !cJSON_AddBoolToObject(item, "status_known", status_ret == 0) ||
            (status_ret == 0 && !cJSON_AddBoolToObject(item, "running", status.state == SMART_HOME_CAMERA_RUNNING))) {
            cJSON_Delete(item); goto nomem;
        }
        if ((ret = append_entry(root, devices, item)) != AGENT_OK) goto out;
    }
#endif
#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
    if (matches(source, "mijia")) {
        int reachable = app->miloco && smart_home_miloco_reachable(app->miloco);
        if (!cJSON_AddBoolToObject(root, "mijia_reachable", reachable)) goto nomem;
        if (reachable) {
            smart_home_miloco_device_t *list = smart_home_bulk_alloc(
                SMART_HOME_MILOCO_MAX_DEVICES * sizeof(*list));
            if (!list) goto nomem;
            size_t count = smart_home_miloco_list(app->miloco, list, SMART_HOME_MILOCO_MAX_DEVICES, NULL);
            ret = AGENT_OK;
            for (size_t i = 0; i < count; i++) {
                const char *type = list[i].category == SMART_HOME_MILOCO_CATEGORY_CAMERA ? "camera" :
                    list[i].category == SMART_HOME_MILOCO_CATEGORY_LIGHT ? "light" :
                    list[i].category == SMART_HOME_MILOCO_CATEGORY_AC ? "air-conditioner" :
                    list[i].category == SMART_HOME_MILOCO_CATEGORY_FAN ? "fan" : "other";
                char key[40];
                if (!matches(kind, type)) continue;
                snprintf(key, sizeof(key), "milo:%s", list[i].did);
                cJSON *item = entry("mijia", key, list[i].name, type, "miot_device_list");
                if (!item || !cJSON_AddStringToObject(item, "did", list[i].did) ||
                    !cJSON_AddStringToObject(item, "room", list[i].room) ||
                    !cJSON_AddBoolToObject(item, "online", list[i].online)) {
                    cJSON_Delete(item); ret = AGENT_ERROR_NOMEM; break;
                }
                ret = append_entry(root, devices, item);
                if (ret != AGENT_OK) break;
            }
            smart_home_bulk_free(list);
            if (ret != AGENT_OK) goto out;
        }
    }
#endif
    for (size_t i = 0; i < app->remote_route_count; i++) {
        const char *origin = app->remote_routes[i].group == SMART_HOME_TOOL_GROUP_NODE ? "node" : "mcp";
        if (!matches(source, origin)) continue;
        cJSON *item = cJSON_CreateObject();
        if (!item || !cJSON_AddStringToObject(item, "source", origin) ||
            !cJSON_AddStringToObject(item, "tool", app->remote_routes[i].name) ||
            !cJSON_AddBoolToObject(item, "read_only", app->remote_routes[i].read_only) ||
            !cJSON_AddStringToObject(item, "device_source", "unknown")) {
            cJSON_Delete(item); goto nomem;
        }
        if ((ret = append_entry(root, routes, item)) != AGENT_OK) goto out;
    }
    text = cJSON_PrintUnformatted(root);
    ret = text ? smart_home_tool_reply(sink, text, AGENT_OK) : AGENT_ERROR_NOMEM;
out:
    cJSON_free(text);
    cJSON_Delete(root);
    return (agent_error_t)ret;
nomem:
    ret = AGENT_ERROR_NOMEM;
    goto out;
invalid:
    return smart_home_tool_reply(sink, "{\"ok\":false,\"error\":\"invalid_inventory_filter\"}", AGENT_ERROR_TOOL_ARGUMENT);
}

int smart_home_device_inventory_register(smart_home_agent_app_t *app)
{
    const agent_tool_t tool = {
        .name = AGENT_SV_LITERAL("get_device_inventory"),
        .group = AGENT_SV_LITERAL("local"), .category = AGENT_SV_LITERAL("query"),
        .description = AGENT_SV_LITERAL("Read cached device origins and Node/MCP tool routes without starting hardware. "
            "Use kind=camera for ambiguous camera requests. Query details before control; ask if target/action is unclear."),
        .input_schema_json = {inventory_schema, sizeof(inventory_schema) - 1u},
        .validate = smart_home_tool_validate_object, .execute = inventory_tool,
        .user_data = app, .flags = AGENT_TOOL_READ_ONLY,
    };
    return agent_register_tool(app->agent, &tool);
}
