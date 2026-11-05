/* SPDX-License-Identifier: Apache-2.0 */
#include "agent/smart_home_agent.h"
#include "config/smart_home_config.h"
#include "miloco/smart_home_miloco.h"
#include <agent_openvela_tls.h>
#include <agent_openvela_transport.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t peak_request, peak_scratch;
static int latest_users, policy_denials, http_status = 200;
static char final_json[4608];
static const char calls_json[] =
    "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":null,\"tool_calls\":["
    "{\"id\":\"c1\",\"type\":\"function\",\"function\":{\"name\":\"read_skill\",\"arguments\":\"{\\\"name\\\":\\\"smart_home_device_control\\\"}\"}},"
    "{\"id\":\"c2\",\"type\":\"function\",\"function\":{\"name\":\"set_light\",\"arguments\":\"{\\\"room\\\":\\\"bedroom\\\",\\\"on\\\":true,\\\"brightness\\\":40}\"}},"
    "{\"id\":\"c3\",\"type\":\"function\",\"function\":{\"name\":\"miot_device_list\",\"arguments\":\"{}\"}},"
    "{\"id\":\"c4\",\"type\":\"function\",\"function\":{\"name\":\"get_weather\",\"arguments\":\"{\\\"location\\\":\\\"Shenzhen\\\"}\"}}"
    "]},\"finish_reason\":\"tool_calls\"}]}";

void *smart_home_bulk_alloc(size_t n) { return malloc(n); }
void smart_home_bulk_free(void *p) { free(p); }
void smart_home_cpu_debug_log(const char *stage) { (void)stage; }
void smart_home_network_status_init(smart_home_network_status_t *s) { memset(s, 0, sizeof(*s)); }
void smart_home_ui_event_cb(const agent_event_t *event, void *data)
{
    (void)data;
    if (event->summary.scratch_peak_bytes > peak_scratch) peak_scratch = event->summary.scratch_peak_bytes;
}
bool smart_home_miloco_reachable(const smart_home_miloco_t *s) { return s != NULL; }
size_t smart_home_miloco_list(const smart_home_miloco_t *s, smart_home_miloco_device_t *d,
                              size_t capacity, uint32_t *revision)
{
    (void)s; (void)revision;
    assert(capacity >= 2);
    memset(d, 0, 2 * sizeof(*d));
    for (int i = 0; i < 2; ++i) {
        snprintf(d[i].did, sizeof(d[i].did), "%d", i + 1);
        strcpy(d[i].name, "mock gateway device");
        strcpy(d[i].room, "bedroom");
        d[i].online = true;
        d[i].control_count = SMART_HOME_MILOCO_MAX_CONTROLS;
        for (int j = 0; j < SMART_HOME_MILOCO_MAX_CONTROLS; ++j) {
            snprintf(d[i].controls[j].iid, sizeof(d[i].controls[j].iid), "prop.2.%d", j + 1);
            strcpy(d[i].controls[j].desc, "mock control");
        }
    }
    return 2;
}
void smart_home_miloco_stop(smart_home_miloco_t **s) { *s = NULL; }
bool smart_home_miloco_get_weather(const smart_home_miloco_t *s, smart_home_miloco_weather_t *out)
{ (void)s; (void)out; return false; }
int smart_home_miloco_run_scene(smart_home_miloco_t *s, const char *scene, char *report, size_t size)
{ (void)s; (void)scene; (void)report; (void)size; assert(0); return AGENT_ERROR; }
int smart_home_miloco_submit_control(smart_home_miloco_t *s, const char *did,
                                   const char *iid, const char *operation, int32_t value)
{ (void)s; (void)did; (void)iid; (void)operation; (void)value; assert(0); return AGENT_ERROR; }

static agent_error_t http_request(void *data, const agent_http_request_t *request,
                                 const agent_http_sink_t *sink)
{
    int has_result = 0, references = 0;
    cJSON *json, *message, *messages;
    const char *body;
    agent_error_t ret;
    (void)data;
    if (request->body_size > peak_request) peak_request = request->body_size;
    assert(request->body_size <= CONFIG_SMART_HOME_MODEL_REQUEST_BUFFER_SIZE);
    json = cJSON_ParseWithLength(request->body, request->body_size);
    assert(json);
    messages = cJSON_GetObjectItemCaseSensitive(json, "messages");
    latest_users = 0;
    cJSON_ArrayForEach(message, messages) {
        const cJSON *role = cJSON_GetObjectItemCaseSensitive(message, "role");
        const cJSON *content = cJSON_GetObjectItemCaseSensitive(message, "content");
        assert(cJSON_IsString(role));
        if (!strcmp(role->valuestring, "system")) {
            assert(cJSON_IsString(content) && strstr(content->valuestring, "Skill smart_home_device_control:"));
            assert(!strstr(content->valuestring, "Device inventory:"));
        } else if (!strcmp(role->valuestring, "user")) {
            assert(cJSON_IsString(content));
            if (strstr(content->valuestring, "\"environment\"")) ++references;
            else ++latest_users;
            has_result = 0;
        } else if (!strcmp(role->valuestring, "tool")) {
            has_result = 1;
            assert(cJSON_IsString(content));
            if (strstr(content->valuestring, agent_error_str(AGENT_ERROR_POLICY_DENIED))) ++policy_denials;
        }
    }
#ifdef CONFIG_SMART_HOME_USER_MEMORY
    assert(references == 2);
#else
    assert(references == 1);
#endif
    cJSON_Delete(json);
    ret = sink->headers(sink->context, http_status, NULL, 0);
    if (ret) return ret;
    body = has_result ? final_json : calls_json;
    return sink->body(sink->context, body, strlen(body));
}
struct webclient_tls_ops { int unused; };
agent_error_t agent_port_openvela_tls_init(agent_port_openvela_tls_t *s,
                                          const agent_port_openvela_tls_config_t *config)
{
    static const struct webclient_tls_ops ops = {0};
    s->config = *config; s->ops = &ops; return AGENT_OK;
}
agent_error_t agent_port_openvela_transport_init(agent_transport_t *out,
    agent_port_openvela_transport_t *s, const agent_port_openvela_transport_config_t *config)
{
    static const agent_transport_ops_t ops = {.request = http_request};
    if (!config->request_timeout_ms) return AGENT_ERROR_INVALID;
    assert(config->request_timeout_ms ==
           ((agent_port_openvela_tls_t *)config->tls_context)->config.io_timeout_ms);
    s->config = *config; out->context = s; out->ops = &ops; return AGENT_OK;
}
static void expect_run(smart_home_agent_app_t *app, const char *input)
{
    char output[SMART_HOME_OUTPUT_SIZE];
    int ret = smart_home_agent_run(app, input, output, sizeof(output));
    if (ret) fprintf(stderr, "standard run failed: %d (%s)\n", ret, agent_error_str(ret));
    assert(!ret && strlen(output) == 3000);
}
static agent_error_t check_history(void *context, const agent_session_group_view_t *group)
{
    size_t *turns = context;
    size_t calls = 0u;
    assert(group->message_count >= 2u);
    assert(group->messages[0].role == AGENT_MESSAGE_ROLE_USER);
    for (size_t i = 0; i < group->message_count; ++i) calls += group->messages[i].tool_call_count;
    assert(calls == 4u);
    assert(group->messages[group->message_count - 1u].role == AGENT_MESSAGE_ROLE_ASSISTANT);
    assert(group->messages[group->message_count - 1u].content.size == 3000u);
    ++*turns;
    return AGENT_OK;
}
int main(int argc, char **argv)
{
    smart_home_agent_app_t app;
    smart_home_device_t device;
    char answer[3001], input[SMART_HOME_INPUT_SIZE], output[SMART_HOME_OUTPUT_SIZE];
    size_t identities;
    memset(answer, 'a', sizeof(answer) - 1); answer[sizeof(answer) - 1] = '\0';
    snprintf(final_json, sizeof(final_json),
             "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"%s\"},\"finish_reason\":\"stop\"}]}", answer);
    memset(input, 'q', sizeof(input) - 1); input[sizeof(input) - 1] = '\0';
    assert(!smart_home_agent_app_init(&app));
    assert(app.skill_store.count == 7 && app.system_status.model_status == AGENT_OK);
    app.miloco = (smart_home_miloco_t *)&app;
    if (argc > 1 && !strcmp(argv[1], "--resume")) {
        size_t turns = 0u;
        assert(!smart_home_agent_app_visit_history(&app, check_history, &turns) && turns == 3u);
        assert(peak_request == 0u);
        expect_run(&app, "after process restart");
        assert(latest_users == 4);
        assert(!smart_home_agent_app_clear_session(&app));
        smart_home_agent_app_deinit(&app);
        assert(!smart_home_agent_app_init(&app));
        turns = 0u;
        assert(!smart_home_agent_app_visit_history(&app, check_history, &turns) && turns == 0u);
        smart_home_agent_app_deinit(&app);
        puts("PASS: separate process restores real conversation and tool results without replay; clear survives reinitialization");
        return 0;
    }
    for (int i = 0; i < 8; ++i) {
        expect_run(&app, input);
        assert(latest_users == (i < 3 ? i + 1 : 4));
    }
    assert(!smart_home_device_service_find_first(&app.device_service, "bedroom", SMART_HOME_DEVICE_LIGHT, &device));
    assert(device.on && device.brightness == 40);
    assert(!agent_session_count(app.agent, &identities) && identities == 1);
    assert(!smart_home_agent_app_set_local_tool_access(&app, "set_light", 0));
    assert(!smart_home_device_service_set_light(&app.device_service, "bedroom", 0, 0));
    expect_run(&app, input);
    assert(!smart_home_device_service_find_first(&app.device_service, "bedroom", SMART_HOME_DEVICE_LIGHT, &device));
    assert(!device.on && policy_denials > 0);
    smart_home_model_config_t config = app.model_config;
    strcpy(config.backend_id, "custom"); strcpy(config.api_key, "mock-custom-key");
    strcpy(config.host, "mock.example");
    assert(!smart_home_agent_app_apply_model_config(&app, &config));
    expect_run(&app, input);
    assert(latest_users == 4);
    assert(!smart_home_agent_app_clear_session(&app));
    expect_run(&app, input);
    assert(latest_users == 1);
    http_status = 401;
    assert(smart_home_agent_run(&app, input, output, sizeof(output)) == AGENT_ERROR_AUTH);
    http_status = 200;
    expect_run(&app, input);
    assert(latest_users == 2);
    config.request_buffer_size = 16384;
    assert(!smart_home_agent_app_apply_model_config(&app, &config));
    assert(smart_home_agent_run(&app, input, output, sizeof(output)) == AGENT_ERROR_CAPACITY);
    config.request_buffer_size = CONFIG_SMART_HOME_MODEL_REQUEST_BUFFER_SIZE;
    assert(!smart_home_agent_app_apply_model_config(&app, &config));
    expect_run(&app, input);
    smart_home_agent_app_deinit(&app);
    printf("PASS: standard firmware profile, real application/Skills/devices, 4-tool turns, history, policy, switch, HTTP recovery; request_peak=%zu scratch_peak=%zu\n",
           peak_request, peak_scratch);
    return 0;
}
