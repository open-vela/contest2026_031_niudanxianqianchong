/* SPDX-License-Identifier: Apache-2.0 */
#include "agent/smart_home_agent.h"
#include "tools/smart_home_tools.h"
#include "tools/smart_home_tool_io.h"
#include "miloco/smart_home_miloco.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static agent_tool_t tools[16];
static size_t count;
static int effects, fail_write;
static char output[8192];

agent_error_t agent_register_tool(agent_t *agent, const agent_tool_t *tool)
{
    (void)agent;
    assert(count < 16);
    assert(tool->name.size && tool->execute && tool->validate);
    assert(tool->description.size <= AGENT_MAX_DESCRIPTION_BYTES);
    assert(tool->input_schema_json.size <= AGENT_MAX_SCHEMA_BYTES);
    assert(tool->group.size == 5 && !memcmp(tool->group.data, "local", 5));
    tools[count++] = *tool;
    return AGENT_OK;
}
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
size_t smart_home_miloco_list(const smart_home_miloco_t *s, smart_home_miloco_device_t *d,
    size_t capacity, uint32_t *revision)
{ (void)s; (void)capacity; (void)revision; memset(d, 0, sizeof(*d)); strcpy(d->did, "123"); strcpy(d->name, "test"); return 1; }
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
    assert(!smart_home_tools_register((agent_t *)&service, &service));
    assert(count == 8);
    assert(!invoke("set_light", "{\"room\":\"bedroom\",\"on\":true,\"brightness\":40}"));
    assert(effects == 1);
    assert(invoke("set_light", "{\"room\":\"bedroom\",\"on\":true,\"brightness\":1.5}"));
    assert(invoke("set_light", "{\"room\":\"bedroom\",\"on\":true,\"brightness\":2147483648}"));
    assert(invoke("set_light", "{\"room\":\"bedroom\",\"on\":true,\"brightness\":40,\"brightness\":0}"));
    assert(invoke("set_light", "{\"room\":\"bedroom\",\"on\":true,\"brightness\":40}x"));
    assert(invoke("set_ac", "{\"room\":\"bedroom\",\"on\":true,\"temperature\":\"oops\"}"));
    assert(invoke("set_ac", "{\"room\":\"bedroom\",\"on\":true,\"mode\":\"oops\"}"));
    assert(effects == 1);
    assert(!invoke("set_ac", "{\"room\":\"bedroom\",\"on\":true}"));
    assert(effects == 2);
    assert(!invoke("get_weather", "{\"location\":\"Shenzhen\"}"));
    for (int i = 0; i < 8; ++i) assert(!invoke("set_timer", "{\"delay_seconds\":60,\"message\":\"test\"}"));
    assert(invoke("set_timer", "{\"delay_seconds\":60,\"message\":\"full\"}") == AGENT_ERROR_CAPACITY);
    assert(!invoke("list_timers", "{}"));
    assert(!invoke("cancel_timer", "{\"id\":1}"));
    assert(invoke("cancel_timer", "{\"id\":1}") == AGENT_ERROR_NOT_FOUND);
    fail_write = 1;
    assert(invoke("get_home_status", "{}") == AGENT_ERROR_CAPACITY);
    fail_write = 0;
#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
    smart_home_agent_app_t app = {0};
    app.miloco = (smart_home_miloco_t *)&service;
    assert(!smart_home_miloco_tools_register((agent_t *)&service, &app));
    assert(tools[count - 1].flags & AGENT_TOOL_SIDE_EFFECT);
    assert(!invoke("miot_device_list", "{}"));
    assert(invoke("miot_device_control", "{\"did\":\"123\",\"iid\":\"prop.2.1\"}"));
    assert(invoke("miot_device_control", "{\"did\":\"123\",\"iid\":\"prop.2.1\",\"operation\":\"unknown\",\"value\":1}"));
    assert(effects == 2);
    assert(!invoke("miot_device_control", "{\"did\":\"123\",\"iid\":\"prop.2.1\",\"value\":true}"));
    assert(effects == 3 && strstr(output, "submitted"));
#endif
    puts("PASS: native tools, bounded arguments, rejection without effects, timer capacity and sink failures");
    return 0;
}
