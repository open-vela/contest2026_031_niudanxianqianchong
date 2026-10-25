#include "agent/smart_home_agent.h"
#include "config/smart_home_backends.h"
#include "config/smart_home_config_store.h"
#include "config/smart_home_secrets.h"
#include "skills/smart_home_skills.h"
#include "tools/smart_home_tools.h"
#include "ui/smart_home_ui.h"
#include "miloco/smart_home_miloco.h"
#include <agent_openvela_tls.h>
#include <agent_openvela_transport.h>

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t live_allocations;
static size_t allocations;
static size_t fail_allocation;
static int missing_key;
static int fail_tools;
static int fail_context;
static int fail_transport_init;
static int transport_error;
static int tool_flow;
static int effects;
static int events;
static unsigned int http_status = 200u;
static char last_url[288], last_model[64], last_auth[264];
static pthread_mutex_t gate_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cond = PTHREAD_COND_INITIALIZER;
static int block_http, entered_http;
static atomic_int applying, applied;

void *smart_home_bulk_alloc(size_t size)
{
    void *p;
    allocations++;
    if (fail_allocation && allocations == fail_allocation) return NULL;
    p = malloc(size);
    if (p) live_allocations++;
    return p;
}

void smart_home_bulk_free(void *p)
{
    if (p) { assert(live_allocations); live_allocations--; free(p); }
}

static void copy_view(char *out, size_t size, agent_string_view_t view)
{
    assert(view.size < size);
    if (view.size) memcpy(out, view.data, view.size);
    out[view.size] = '\0';
}

static agent_error_t http_request(void *context, const agent_http_request_t *request,
                                  const agent_http_sink_t *sink)
{
    const char *final = "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"hello from V2\"},\"finish_reason\":\"stop\"}]}";
    const char *call = "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[{\"id\":\"call1\",\"type\":\"function\",\"function\":{\"name\":\"set_light\",\"arguments\":\"{}\"}}]},\"finish_reason\":\"tool_calls\"}]}";
    agent_error_t ret;
    cJSON *json, *messages, *message, *role;
    int has_result = 0;
    (void)context;
    copy_view(last_url, sizeof(last_url), request->url);
    last_auth[0] = '\0';
    for (size_t i = 0; i < request->header_count; i++) {
        if (smart_home_tool_text_equal(request->headers[i].name, "Authorization"))
            copy_view(last_auth, sizeof(last_auth), request->headers[i].value);
    }
    json = cJSON_ParseWithLength(request->body, request->body_size);
    assert(json);
    message = cJSON_GetObjectItemCaseSensitive(json, "model");
    assert(cJSON_IsString(message));
    snprintf(last_model, sizeof(last_model), "%s", message->valuestring);
    messages = cJSON_GetObjectItemCaseSensitive(json, "messages");
    cJSON_ArrayForEach(message, messages) {
        role = cJSON_GetObjectItemCaseSensitive(message, "role");
        if (cJSON_IsString(role) && strcmp(role->valuestring, "tool") == 0) has_result = 1;
    }
    cJSON_Delete(json);
    pthread_mutex_lock(&gate_mutex);
    if (block_http) {
        entered_http = 1;
        pthread_cond_broadcast(&gate_cond);
        while (block_http) pthread_cond_wait(&gate_cond, &gate_mutex);
    }
    pthread_mutex_unlock(&gate_mutex);
    if (transport_error) return (agent_error_t)transport_error;
    ret = sink->headers(sink->context, http_status, NULL, 0u);
    if (ret != AGENT_OK) return ret;
    const char *body = tool_flow && !has_result ? call : final;
    return sink->body(sink->context, body, strlen(body));
}

struct webclient_tls_ops { int unused; };

agent_error_t agent_port_openvela_tls_init(agent_port_openvela_tls_t *state,
                                           const agent_port_openvela_tls_config_t *config)
{
    static const struct webclient_tls_ops ops = {0};
    state->config = *config;
    state->ops = &ops;
    return AGENT_OK;
}

agent_error_t agent_port_openvela_transport_init(agent_transport_t *out,
    agent_port_openvela_transport_t *state, const agent_port_openvela_transport_config_t *config)
{
    static const agent_transport_ops_t ops = {.request = http_request};
    if (fail_transport_init) return AGENT_ERROR_IO;
    assert(config->runtime.now_ms && config->tls_ops && config->tls_context);
    assert(config->url_buffer && config->io_buffer && config->response_header_buffer);
    state->config = *config;
    out->ops = &ops;
    out->context = state;
    return AGENT_OK;
}

void smart_home_cpu_debug_log(const char *role) { (void)role; }
void smart_home_network_status_init(smart_home_network_status_t *s) { memset(s, 0, sizeof(*s)); }
void smart_home_device_init(smart_home_state_t *s) { memset(s, 0, sizeof(*s)); }
int smart_home_device_service_init(smart_home_device_service_t *s, smart_home_state_t *state)
{ memset(s, 0, sizeof(*s)); s->state = state; return AGENT_OK; }
void smart_home_device_service_deinit(smart_home_device_service_t *s) { (void)s; }
int smart_home_device_service_register_context(agent_t *agent, smart_home_device_service_t *s)
{ (void)agent; (void)s; return fail_context ? AGENT_ERROR_CONTEXT_OVERFLOW : AGENT_OK; }
void smart_home_skill_store_init(smart_home_skill_store_t *s) { memset(s, 0, sizeof(*s)); }
void smart_home_skill_store_deinit(smart_home_skill_store_t *s) { memset(s, 0, sizeof(*s)); }
int smart_home_skills_register(agent_t *a, smart_home_skill_store_t *s)
{ (void)a; (void)s; return AGENT_ERROR_NOT_FOUND; }
const smart_home_loaded_skill_t *smart_home_skill_store_find(const smart_home_skill_store_t *s,
                                                            const char *name)
{ (void)s; (void)name; return NULL; }
int smart_home_scene_catalog_load(const char *text, smart_home_scene_catalog_t *c)
{ (void)text; memset(c, 0, sizeof(*c)); return AGENT_OK; }
int smart_home_device_service_set_scene_catalog(smart_home_device_service_t *s,
                                                const smart_home_scene_catalog_t *c)
{ (void)s; (void)c; return AGENT_OK; }

static agent_error_t execute_tool(void *user_data, const agent_tool_context_t *context,
                                  const agent_text_sink_t *output)
{
    static const char result[] = "{\"ok\":true}";
    (void)user_data; (void)context;
    effects++;
    return output->write(output->context, agent_string_view(result, sizeof(result) - 1u));
}

int smart_home_tools_register(agent_t *agent, smart_home_device_service_t *s)
{
    const agent_tool_t tool = {
        .name = AGENT_SV_LITERAL("set_light"),
        .description = AGENT_SV_LITERAL("Test light control"),
        .input_schema_json = AGENT_SV_LITERAL("{\"type\":\"object\",\"properties\":{}}"),
        .group = AGENT_SV_LITERAL(SMART_HOME_TOOL_GROUP_LOCAL_NAME),
        .category = AGENT_SV_LITERAL(SMART_HOME_TOOL_CATEGORY_CONTROL_NAME),
        .flags = AGENT_TOOL_SIDE_EFFECT, .execute = execute_tool
    };
    (void)s;
    return fail_tools ? AGENT_ERROR_CAPACITY : agent_register_tool(agent, &tool);
}

#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
int smart_home_miloco_tools_register(agent_t *a, smart_home_agent_app_t *app)
{ (void)a; (void)app; return AGENT_OK; }
void smart_home_miloco_stop(smart_home_miloco_t **s) { *s = NULL; }
#endif

int smart_home_config_settings_load(cJSON **out, char *error, size_t size)
{
    *out = cJSON_CreateObject();
    assert(*out && cJSON_AddStringToObject(*out, "active_backend_id", "deepseek"));
    if (size) error[0] = '\0';
    return AGENT_OK;
}
int smart_home_config_settings_save(const cJSON *root) { (void)root; return AGENT_OK; }
int smart_home_secrets_get_model_api_key(const char *id, char *out, size_t size)
{
    (void)id;
    if (missing_key) { out[0] = '\0'; return AGENT_ERROR_NOT_FOUND; }
    snprintf(out, size, "test-key");
    return AGENT_OK;
}
void smart_home_ui_event_cb(const agent_event_t *event, void *user_data)
{
    uint64_t *start = user_data;
    if (event->type == AGENT_EVENT_TURN_BEGIN) *start = event->timestamp_ms;
    events++;
}

static smart_home_model_config_t custom_config(smart_home_agent_app_t *app)
{
    smart_home_model_config_t config = app->model_config;
    strcpy(config.backend_id, "custom");
    strcpy(config.host, "mock.example");
    strcpy(config.port, "8443");
    strcpy(config.path, "/chat");
    strcpy(config.model, "mock-next");
    strcpy(config.api_key, "test-next");
    return config;
}

static void expect_run(smart_home_agent_app_t *app)
{
    char output[64];
    static int run;
    int ret = smart_home_agent_run(app, "hello", output, sizeof(output));
    run++;
    if (ret != AGENT_OK) fprintf(stderr, "run %d failed: %d (%s), model_status=%d\n",
                                 run, ret, agent_error_str((agent_error_t)ret),
                                 app->system_status.model_status);
    assert(ret == AGENT_OK);
    assert(strcmp(output, "hello from V2") == 0);
}

static int visit_tool(const smart_home_tool_info_t *tool, void *data)
{
    int *count = data;
    assert(strcmp(tool->name, "set_light") == 0);
    assert(tool->group_id == SMART_HOME_TOOL_GROUP_LOCAL);
    assert(tool->category_id == SMART_HOME_TOOL_CATEGORY_CONTROL);
    assert(tool->enabled && tool->llm_visible && tool->flags == AGENT_TOOL_SIDE_EFFECT);
    (*count)++;
    return AGENT_OK;
}

static void lifecycle_and_configuration(void)
{
    smart_home_agent_app_t app;
    smart_home_model_config_t config;
    char tiny[3];
    int count = 0;
    assert(smart_home_agent_app_init(&app) == AGENT_OK);
    expect_run(&app);
    assert(strcmp(last_auth, "Bearer test-key") == 0 && events >= 4);
    assert(smart_home_agent_app_enumerate_tools(&app, visit_tool, &count) == AGENT_OK && count == 1);
    config = custom_config(&app);
    assert(smart_home_agent_app_apply_model_config(&app, &config) == AGENT_OK);
    memset(&config, 0xa5, sizeof(config));
    expect_run(&app);
    assert(strcmp(last_url, "https://mock.example:8443/chat") == 0);
    assert(strcmp(last_model, "mock-next") == 0 && strcmp(last_auth, "Bearer test-next") == 0);
    for (size_t i = 1u; i <= 4u; i++) {
        size_t before = live_allocations;
        config = app.model_config;
        fail_allocation = allocations + i;
        assert(smart_home_agent_app_apply_model_config(&app, &config) == AGENT_ERROR_NOMEM);
        fail_allocation = 0;
        assert(live_allocations == before);
        expect_run(&app);
        assert(strcmp(last_auth, "Bearer test-next") == 0);
    }
    config = app.model_config;
    strcpy(config.path, "/bad path");
    assert(smart_home_agent_app_apply_model_config(&app, &config) == AGENT_ERROR_INVALID);
    expect_run(&app);
    assert(strcmp(last_url, "https://mock.example:8443/chat") == 0);
    config = app.model_config;
    config.request_buffer_size = 1u;
    assert(smart_home_agent_app_apply_model_config(&app, &config) == AGENT_ERROR_CAPACITY);
    expect_run(&app);
    config = app.model_config;
    memset(config.model, 'x', sizeof(config.model));
    assert(smart_home_agent_app_apply_model_config(&app, &config) == AGENT_ERROR_INVALID);
    expect_run(&app);
    assert(smart_home_agent_run(&app, "hello", tiny, sizeof(tiny)) == AGENT_ERROR_TRUNCATED);
    assert(tiny[sizeof(tiny) - 1u] == '\0');
    http_status = 401u;
    assert(smart_home_agent_run(&app, "hello", tiny, sizeof(tiny)) == AGENT_ERROR_AUTH);
    http_status = 200u;
    transport_error = AGENT_ERROR_TIMEOUT;
    assert(smart_home_agent_run(&app, "hello", tiny, sizeof(tiny)) == AGENT_ERROR_TIMEOUT);
    transport_error = 0;
    tool_flow = 1;
    expect_run(&app);
    assert(effects == 1);
    assert(smart_home_agent_app_set_local_tool_access(&app, "set_light", 0) == AGENT_OK);
    expect_run(&app);
    assert(effects == 1);
    assert(smart_home_agent_app_set_local_tool_access(&app, "set_light", 1) == AGENT_OK);
    expect_run(&app);
    assert(effects == 2);
    tool_flow = 0;
    smart_home_agent_app_deinit(&app);
    smart_home_agent_app_deinit(&app);
    assert(!live_allocations);
    puts("PASS: lifecycle, owned configuration, rollback, tool policy, HTTP failures and truncation");
}

static void initialization_failures(void)
{
    smart_home_agent_app_t app;
    char output[128];
    missing_key = 1;
    assert(smart_home_agent_app_init(&app) == AGENT_OK);
    assert(app.system_status.agent_status == AGENT_OK);
    assert(smart_home_agent_run(&app, "hello", output, sizeof(output)) == AGENT_ERROR_NOT_FOUND);
    missing_key = 0;
    assert(smart_home_agent_app_apply_model_config(&app, &app.model_config) == AGENT_OK);
    expect_run(&app);
    smart_home_agent_app_deinit(&app);
    fail_transport_init = 1;
    assert(smart_home_agent_app_init(&app) == AGENT_ERROR_IO);
    assert(!app.agent && !app.backend && !live_allocations);
    fail_transport_init = 0;
    fail_context = 1;
    assert(smart_home_agent_app_init(&app) == AGENT_ERROR_CONTEXT_OVERFLOW);
    assert(!live_allocations);
    fail_context = 0;
    fail_tools = 1;
    assert(smart_home_agent_app_init(&app) == AGENT_ERROR_CAPACITY);
    assert(!live_allocations);
    fail_tools = 0;
    for (size_t i = 1u; i <= 9u; i++) {
        allocations = 0;
        fail_allocation = i;
        int ret = smart_home_agent_app_init(&app);
        assert(ret == AGENT_OK || ret == AGENT_ERROR_NOMEM);
        smart_home_agent_app_deinit(&app);
        assert(!live_allocations);
    }
    fail_allocation = 0;
    puts("PASS: missing key recovery and cleanup after transport/context/tool/allocation failures");
}

static void *run_thread(void *data) { expect_run(data); return NULL; }
static void *apply_thread(void *data)
{
    smart_home_agent_app_t *app = data;
    smart_home_model_config_t config = custom_config(app);
    atomic_store(&applying, 1);
    assert(smart_home_agent_app_apply_model_config(app, &config) == AGENT_OK);
    atomic_store(&applied, 1);
    return NULL;
}

static void serialization(void)
{
    smart_home_agent_app_t app;
    pthread_t run, change;
    assert(smart_home_agent_app_init(&app) == AGENT_OK);
    pthread_mutex_lock(&gate_mutex);
    block_http = 1; entered_http = 0;
    pthread_mutex_unlock(&gate_mutex);
    assert(pthread_create(&run, NULL, run_thread, &app) == 0);
    pthread_mutex_lock(&gate_mutex);
    while (!entered_http) pthread_cond_wait(&gate_cond, &gate_mutex);
    pthread_mutex_unlock(&gate_mutex);
    assert(pthread_create(&change, NULL, apply_thread, &app) == 0);
    while (!atomic_load(&applying)) sched_yield();
    assert(pthread_mutex_trylock(&app.agent_mutex) == EBUSY);
    assert(!atomic_load(&applied));
    pthread_mutex_lock(&gate_mutex);
    block_http = 0;
    pthread_cond_broadcast(&gate_cond);
    pthread_mutex_unlock(&gate_mutex);
    pthread_join(run, NULL); pthread_join(change, NULL);
    assert(atomic_load(&applied));
    expect_run(&app);
    assert(strcmp(last_model, "mock-next") == 0);
    smart_home_agent_app_deinit(&app);
    assert(!live_allocations);
    puts("PASS: standard SmartHome serializes model changes with an active run");
}

int main(void)
{
#ifdef CONFIG_AGENT_MAX_TOOLS
    _Static_assert(AGENT_MAX_TOOLS == CONFIG_AGENT_MAX_TOOLS, "inconsistent build profile");
#endif
    lifecycle_and_configuration();
    initialization_failures();
    serialization();
    return 0;
}
