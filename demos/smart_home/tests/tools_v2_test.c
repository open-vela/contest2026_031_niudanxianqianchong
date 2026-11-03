/* SPDX-License-Identifier: Apache-2.0 */
#include "agent/smart_home_agent.h"
#include "tools/smart_home_tools.h"
#include "tools/smart_home_tool_io.h"
#include "miloco/smart_home_miloco.h"
#include "tools/smart_home_camera_tools.h"
#include "camera/smart_home_camera_service.h"
#include "camera/smart_home_camera_control.h"
#include "camera/smart_home_snapshot_store.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static agent_tool_t tools[16];
static size_t count;
static int effects, fail_write;
#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
static size_t gateway_device_count = 1u;
static int rich_controls;
#endif
static char output[8192];
static smart_home_state_t state;

agent_error_t agent_register_tool(agent_t *agent, const agent_tool_t *tool)
{
    (void)agent;
    assert(count < 16);
    assert(tool->name.size && tool->execute && tool->validate);
    assert(tool->description.size <= AGENT_MAX_DESCRIPTION_BYTES);
    assert(tool->input_schema_json.size <= AGENT_MAX_SCHEMA_BYTES);
    tools[count++] = *tool;
    return AGENT_OK;
}
agent_error_t agent_tool_enumerate(const agent_t *agent, agent_tool_visit_fn visit, void *data)
{
    (void)agent;
    for (size_t i = 0; i < count; i++) {
        agent_error_t ret = visit(data, &tools[i]);
        if (ret != AGENT_OK) return ret;
    }
    return AGENT_OK;
}
int smart_home_device_service_copy_state(smart_home_device_service_t *s, smart_home_state_t *out)
{ (void)s; *out = state; return AGENT_OK; }
int smart_home_device_service_get_device(smart_home_device_service_t *s, int id, smart_home_device_t *out)
{
    (void)s;
    if (id != 1 && id != 2) return AGENT_ERROR_NOT_FOUND;
    memset(out, 0, sizeof(*out)); out->id = id; out->used = 1;
    out->type = id == 1 ? SMART_HOME_DEVICE_LIGHT : SMART_HOME_DEVICE_AC;
    strcpy(out->room, "bedroom"); out->temperature = 26;
    return AGENT_OK;
}
int smart_home_device_service_set_device_control(smart_home_device_service_t *s,
    int id, int on, int value, int mode, int fan)
{
    (void)s; (void)on; (void)mode; (void)fan;
    assert((id == 1 && value == 40) || (id == 2 && value == 26));
    ++effects;
    return AGENT_OK;
}
#ifdef CONFIG_SMART_HOME_CAMERA_TOOL
static int camera_requests;
int smart_home_camera_get_status(struct smart_home_camera_status_s *s)
{ memset(s, 0, sizeof(*s)); s->state = SMART_HOME_CAMERA_OFF; return 0; }
int smart_home_camera_request(bool on) { (void)on; camera_requests++; return 0; }
void smart_home_camera_control_status(smart_home_camera_control_status_t *s)
{ memset(s, 0, sizeof(*s)); }
int smart_home_camera_copy_latest(uint8_t *d, size_t bytes, uint32_t *sequence)
{ (void)d; (void)bytes; (void)sequence; assert(0); return -1; }
int smart_home_snapshot_store_write(const uint8_t *pixels, size_t bytes,
    unsigned int width, unsigned int height, uint64_t ts, unsigned int brightness,
    unsigned int motion, struct smart_home_snapshot_info_s *out)
{ (void)pixels; (void)bytes; (void)width; (void)height; (void)ts; (void)brightness; (void)motion; (void)out; assert(0); return -1; }
#endif
int smart_home_device_service_build_snapshot_json(smart_home_device_service_t *s, char *out, size_t n)
{ (void)s; snprintf(out, n, "{\"ok\":true}"); return AGENT_OK; }
int smart_home_device_service_set_light(smart_home_device_service_t *s, const char *room, int on, int brightness)
{ (void)s; assert(!strcmp(room, "bedroom")); assert(on == 1 && brightness == 40); ++effects; return AGENT_OK; }
int smart_home_device_service_find_first(smart_home_device_service_t *s, const char *room,
    smart_home_device_type_t type, smart_home_device_t *out)
{ (void)s; (void)room; (void)type; memset(out, 0, sizeof(*out)); out->temperature = 26; return AGENT_OK; }
int smart_home_ac_parse_mode(const char *s) { return !strcmp(s, "cool") ? 0 : -1; }
int smart_home_ac_parse_fan_speed(const char *s) { return !strcmp(s, "auto") ? 3 : -1; }
const char *smart_home_ac_mode_name(int n) { (void)n; return "cool"; }
const char *smart_home_ac_fan_speed_name(int n) { (void)n; return "auto"; }
int smart_home_device_service_set_ac(smart_home_device_service_t *s, const char *room,
    int on, int mode, int fan, int temperature)
{ (void)s; (void)room; (void)on; (void)mode; (void)fan; assert(temperature == 26); ++effects; return AGENT_OK; }
int smart_home_device_service_run_scene(smart_home_device_service_t *s, const char *scene)
{ (void)s; (void)scene; ++effects; return AGENT_OK; }
#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
bool smart_home_miloco_reachable(const smart_home_miloco_t *s) { return s != NULL; }
bool smart_home_miloco_copy_device(const smart_home_miloco_t *s, const char *did, smart_home_miloco_device_t *out)
{
    (void)s;
    if (strcmp(did, "123")) return false;
    memset(out, 0, sizeof(*out)); strcpy(out->did, did); out->online = true;
    out->control_count = 1; strcpy(out->controls[0].iid, "prop.2.1");
    out->controls[0].type = SMART_HOME_MILOCO_CTRL_BOOL;
    return true;
}
size_t smart_home_miloco_list(const smart_home_miloco_t *s, smart_home_miloco_device_t *d,
    size_t capacity, uint32_t *revision)
{
    (void)s; (void)revision;
    assert(capacity >= gateway_device_count);
    memset(d, 0, gateway_device_count * sizeof(*d));
    for (size_t i = 0; i < gateway_device_count; ++i) {
        snprintf(d[i].did, sizeof(d[i].did), "%zu", i);
        strcpy(d[i].name, "test");
        d[i].control_count = SMART_HOME_MILOCO_MAX_CONTROLS;
        for (size_t k = 0; k < SMART_HOME_MILOCO_MAX_CONTROLS; ++k) {
            snprintf(d[i].controls[k].iid, sizeof(d[i].controls[k].iid), "prop.2.%zu", k);
            strcpy(d[i].controls[k].desc, "test control");
            if (rich_controls) {
                d[i].controls[k].type = SMART_HOME_MILOCO_CTRL_ENUM;
                d[i].controls[k].option_count = SMART_HOME_MILOCO_MAX_OPTIONS;
                for (unsigned option = 0; option < SMART_HOME_MILOCO_MAX_OPTIONS; option++) {
                    strcpy(d[i].controls[k].options[option].name, "long option description");
                    d[i].controls[k].options[option].value = option;
                }
            }
        }
    }
    return gateway_device_count;
}
bool smart_home_miloco_get_weather(const smart_home_miloco_t *s, smart_home_miloco_weather_t *out)
{ (void)s; (void)out; return false; }
int smart_home_miloco_run_scene(smart_home_miloco_t *s, const char *scene, char *report, size_t n)
{ (void)s; (void)scene; snprintf(report, n, "submitted"); ++effects; return AGENT_OK; }
int smart_home_miloco_submit_control(smart_home_miloco_t *s, const char *did,
    const char *iid, const char *operation, int32_t value)
{ (void)s; assert(!strcmp(did, "123") && !strcmp(iid, "prop.2.1")); assert(!strcmp(operation, "set") && value == 1); ++effects; return AGENT_OK; }
#endif
static agent_error_t write_output(void *data, agent_string_view_t text)
{
    (void)data;
    if (fail_write) return AGENT_ERROR_CAPACITY;
    assert(text.size < sizeof(output));
    memcpy(output, text.data, text.size);
    output[text.size] = '\0';
    return AGENT_OK;
}
static agent_error_t invoke(const char *name, const char *args)
{
    const agent_text_sink_t sink = {write_output, NULL};
    agent_tool_context_t context = {0};
    char bounded[512];
    size_t size = strlen(args);
    assert(size <= sizeof(bounded));
    memcpy(bounded, args, size); /* Intentionally no NUL inside the supplied view. */
    context.call.arguments_json = agent_string_view(bounded, size);
    output[0] = '\0';
    for (size_t i = 0; i < count; ++i) {
        if (tools[i].name.size == strlen(name) && !memcmp(tools[i].name.data, name, strlen(name))) {
            agent_error_t ret = tools[i].validate(tools[i].user_data, &context);
            if (!ret) ret = tools[i].execute(tools[i].user_data, &context, &sink);
            if (output[0]) {
                cJSON *json = cJSON_Parse(output);
                assert(json);
                cJSON_Delete(json);
            }
            return ret;
        }
    }
    assert(0);
    return AGENT_ERROR_NOT_FOUND;
}
int main(void)
{
    smart_home_device_service_t service = {0};
    smart_home_agent_app_t app = {0};
    app.agent = (agent_t *)&service;
    app.device_service.state = &state;
    assert(!smart_home_tools_register((agent_t *)&service, &service));
    assert(count == 8);
    assert(!invoke("set_light", "{\"device_id\":1,\"room\":\"bedroom\",\"on\":true,\"brightness\":40}"));
    assert(effects == 1);
    assert(invoke("set_light", "{\"device_id\":1,\"room\":\"bedroom\",\"on\":true,\"brightness\":1.5}"));
    assert(invoke("set_light", "{\"device_id\":1,\"room\":\"bedroom\",\"on\":true,\"brightness\":2147483648}"));
    assert(invoke("set_light", "{\"device_id\":1,\"room\":\"bedroom\",\"on\":true,\"brightness\":40,\"brightness\":0}"));
    assert(invoke("set_light", "{\"device_id\":1,\"room\":\"bedroom\",\"on\":true,\"brightness\":40}x"));
    assert(invoke("set_light", "{\"room\":\"bedroom\\u0000suffix\",\"on\":true,\"brightness\":40}"));
    assert(invoke("set_ac", "{\"device_id\":2,\"room\":\"bedroom\",\"on\":true,\"temperature\":\"oops\"}"));
    assert(invoke("set_ac", "{\"device_id\":2,\"room\":\"bedroom\",\"on\":true,\"mode\":\"oops\"}"));
    assert(effects == 1);
    assert(!invoke("set_ac", "{\"device_id\":2,\"room\":\"bedroom\",\"on\":true}"));
    assert(effects == 2);
    assert(!invoke("get_weather", "{\"location\":\"Shenzhen\"}"));
    assert(!invoke("get_weather", "{\"location\":\"safe\\\\u0000\"}"));
    for (int i = 0; i < 8; ++i) assert(!invoke("set_timer", "{\"delay_seconds\":60,\"message\":\"test\"}"));
    assert(invoke("set_timer", "{\"delay_seconds\":60,\"message\":\"full\"}") == AGENT_ERROR_CAPACITY);
    assert(!invoke("list_timers", "{}"));
    assert(!invoke("cancel_timer", "{\"id\":1}"));
    assert(invoke("cancel_timer", "{\"id\":1}") == AGENT_ERROR_NOT_FOUND);
    fail_write = 1;
    assert(invoke("get_home_status", "{}") == AGENT_ERROR_CAPACITY);
    fail_write = 0;
#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
    app.miloco = (smart_home_miloco_t *)&service;
    assert(!smart_home_miloco_tools_register((agent_t *)&service, &app));
    assert(tools[count - 1].flags & AGENT_TOOL_SIDE_EFFECT);
    assert(!invoke("miot_device_list", "{}"));
    assert(invoke("miot_device_control", "{\"device_key\":\"milo:123\",\"did\":\"123\",\"iid\":\"prop.2.1\"}"));
    assert(invoke("miot_device_control", "{\"device_key\":\"milo:123\",\"did\":\"123\",\"iid\":\"prop.2.1\",\"operation\":\"unknown\",\"value\":1}"));
    assert(effects == 2);
    assert(!invoke("miot_device_control", "{\"device_key\":\"milo:123\",\"did\":\"123\",\"iid\":\"prop.2.1\",\"value\":true}"));
    assert(effects == 3 && strstr(output, "submitted"));
    assert(invoke("miot_device_control", "{\"device_key\":\"board:camera:0\",\"did\":\"123\",\"iid\":\"prop.2.1\",\"value\":true}") == AGENT_ERROR_TOOL_ARGUMENT);
    assert(invoke("miot_device_control", "{\"device_key\":\"milo:123\",\"did\":\"123\",\"iid\":\"action.2.1\",\"operation\":\"action\"}") == AGENT_ERROR_TOOL_ARGUMENT);
    assert(effects == 3);
    gateway_device_count = SMART_HOME_MILOCO_MAX_DEVICES;
    assert(!invoke("miot_device_list", "{}"));
    cJSON *list = cJSON_Parse(output);
    assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(list, "truncated")));
    assert(cJSON_GetObjectItemCaseSensitive(list, "total_devices")->valueint == SMART_HOME_MILOCO_MAX_DEVICES);
    assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(list, "devices")) < SMART_HOME_MILOCO_MAX_DEVICES);
    cJSON_Delete(list);
    gateway_device_count = 1;
    rich_controls = 1;
    int offset = 0, seen = 0;
    do {
        char args[80];
        snprintf(args, sizeof(args), "{\"did\":\"0\",\"control_offset\":%d}", offset);
        assert(!invoke("miot_device_list", args) && strlen(output) < 4096);
        list = cJSON_Parse(output);
        cJSON *device = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(list, "devices"), 0);
        cJSON *controls = cJSON_GetObjectItemCaseSensitive(device, "controls");
        assert(cJSON_GetArraySize(controls) > 0);
        assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(controls, 0), "options")) == SMART_HOME_MILOCO_MAX_OPTIONS);
        seen += cJSON_GetArraySize(controls);
        int next = cJSON_GetObjectItemCaseSensitive(device, "next_control_offset")->valueint;
        assert(next == -1 || next > offset);
        offset = next;
        cJSON_Delete(list);
    } while (offset >= 0);
    assert(seen == SMART_HOME_MILOCO_MAX_CONTROLS);
    rich_controls = 0;
#endif
    const agent_tool_t node = {
        .name = AGENT_SV_LITERAL("node_bedroom_status"), .group = AGENT_SV_LITERAL("node"),
        .description = AGENT_SV_LITERAL("Node status"),
        .input_schema_json = AGENT_SV_LITERAL("{\"type\":\"object\"}"),
        .execute = tools[0].execute, .validate = smart_home_tool_validate_object,
        .flags = AGENT_TOOL_READ_ONLY
    };
    assert(!agent_register_tool(app.agent, &node));
    assert(!smart_home_device_inventory_register(&app));
    assert(!smart_home_device_inventory_prepare(&app));
    assert(!invoke("get_device_inventory", "{\"source\":\"node\"}"));
    cJSON *inventory = cJSON_Parse(output);
    assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(inventory, "remote_tools")) == 1);
    assert(strstr(output, "node_bedroom_status") && strstr(output, "unknown"));
    cJSON_Delete(inventory);
    for (unsigned i = 0; i < 2; i++) {
        state.devices[i].used = 1; state.devices[i].id = i + 1;
        strcpy(state.devices[i].name, "同名灯"); state.devices[i].type = SMART_HOME_DEVICE_LIGHT;
    }
    assert(!invoke("get_device_inventory", "{\"source\":\"demo\",\"kind\":\"light\"}"));
    inventory = cJSON_Parse(output);
    assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(inventory, "devices")) == 2);
    assert(!strstr(output, "milo:") && !strstr(output, "board:camera"));
    cJSON_Delete(inventory);
    int before = effects;
    assert(invoke("set_light", "{\"room\":\"bedroom\",\"on\":true,\"brightness\":40}"));
    assert(invoke("set_light", "{\"device_id\":2,\"room\":\"bedroom\",\"on\":true,\"brightness\":40}"));
    assert(effects == before);
#ifdef CONFIG_SMART_HOME_CAMERA_TOOL
    assert(!smart_home_camera_tools_register(app.agent));
    assert(!invoke("get_device_inventory", "{\"source\":\"board\",\"kind\":\"camera\"}"));
    assert(strstr(output, "board:camera:0") && strstr(output, "\"ptz_supported\":false"));
    assert(!camera_requests);
    assert(invoke("board_camera", "{\"device_key\":\"milo:123\",\"action\":\"on\"}") == AGENT_ERROR_TOOL_ARGUMENT);
    assert(invoke("board_camera", "{\"device_key\":\"board:camera:0\",\"action\":\"ptz\"}") == AGENT_ERROR_TOOL_ARGUMENT);
    assert(invoke("board_camera", "{\"device_key\":\"board:camera:0\",\"action\":\"shot\"}") == AGENT_ERROR_STATE);
    assert(!camera_requests);
    assert(!invoke("board_camera", "{\"device_key\":\"board:camera:0\",\"action\":\"on\"}"));
    assert(camera_requests == 1);
#endif
    puts("PASS: native tools, bounded arguments, rejection without effects, timer capacity and sink failures");
    return 0;
}
