#include <nuttx/config.h>

#include "smart_home_agent_backend.h"
#include "../smart_home_memory.h"

#include <agent/model.h>
#include <agent_session_ram.h>
#include <agent_openai_model.h>
#include <agent_openvela_runtime.h>
#include <agent_openvela_tls.h>
#include <agent_openvela_transport.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    agent_openai_provider_t provider;
    char endpoint[288];
    char model[64];
    char authorization[264];
    char *request;
    char *response;
    char *decoded;
    size_t request_size;
    size_t response_size;
} smart_home_model_state_t;

struct smart_home_agent_backend {
    agent_workspace_t core_storage;
    agent_model_workspace_t model_storage;
    agent_t *agent;
    agent_model_t *model;
    agent_runtime_t runtime;
    agent_transport_t transport;
    agent_port_openvela_transport_t transport_state;
    agent_port_openvela_tls_t tls;
    agent_limits_t limits;
    smart_home_model_state_t *active;
    agent_session_ram_t session;
    agent_session_ram_turn_t turns[CONFIG_SMART_HOME_SESSION_HISTORY_TURNS + 1u];
    agent_session_ram_message_t messages[(CONFIG_SMART_HOME_SESSION_HISTORY_TURNS + 1u) * AGENT_MAX_PROJECTED_MESSAGES];
    agent_session_ram_call_t calls[(CONFIG_SMART_HOME_SESSION_HISTORY_TURNS + 1u) * AGENT_MAX_PROJECTED_MESSAGES];
    agent_message_view_t read_messages[AGENT_MAX_PROJECTED_MESSAGES];
    agent_tool_call_view_t read_calls[AGENT_MAX_PROJECTED_MESSAGES];
    char session_payload[CONFIG_SMART_HOME_SESSION_PAYLOAD_BYTES];
    char url[512];
    char request_header_text[2048];
    const char *request_headers[16];
    char io[2048];
    agent_http_header_t response_headers[16];
    char response_header_text[2048];
};

static void secure_clear(void *memory, size_t size)
{
    volatile unsigned char *p = memory;
    while (p && size-- > 0u) *p++ = 0u;
}

static void model_state_destroy(smart_home_model_state_t *state)
{
    if (!state) return;
    secure_clear(state->request, state->request_size);
    secure_clear(state->response, state->response_size);
    secure_clear(state->decoded, CONFIG_SMART_HOME_MODEL_DECODED_BUFFER_SIZE);
    smart_home_bulk_free(state->request);
    smart_home_bulk_free(state->response);
    smart_home_bulk_free(state->decoded);
    secure_clear(state, sizeof(*state));
    smart_home_bulk_free(state);
}

static int validate_config(const smart_home_model_config_t *config)
{
    const char *p;
    unsigned long port;

    if (!config || !memchr(config->host, '\0', sizeof(config->host)) ||
        !memchr(config->path, '\0', sizeof(config->path)) ||
        !memchr(config->port, '\0', sizeof(config->port)) ||
        !memchr(config->model, '\0', sizeof(config->model)) ||
        !memchr(config->api_key, '\0', sizeof(config->api_key)) ||
        !config->host[0] || config->path[0] != '/' || !config->port[0] ||
        !config->model[0] || config->timeout_ms < 5000 ||
        config->timeout_ms > 60000) return AGENT_ERROR_INVALID;

    for (p = config->host; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != '.' && *p != '-')
            return AGENT_ERROR_INVALID;
    }
    for (p = config->port; *p; p++) {
        if (!isdigit((unsigned char)*p)) return AGENT_ERROR_INVALID;
    }
    port = strtoul(config->port, NULL, 10);
    if (port == 0u || port > 65535u) return AGENT_ERROR_INVALID;
    if (config->request_buffer_size == 0u || config->response_buffer_size == 0u ||
        config->request_buffer_size > CONFIG_SMART_HOME_MODEL_REQUEST_BUFFER_SIZE ||
        config->response_buffer_size > CONFIG_SMART_HOME_MODEL_RESPONSE_BUFFER_SIZE)
        return AGENT_ERROR_CAPACITY;
    return AGENT_OK;
}

static int model_state_create(smart_home_agent_backend_t *backend,
                              const smart_home_model_config_t *config,
                              smart_home_model_state_t **out)
{
    smart_home_model_state_t *state;
    agent_openai_config_t provider = {0};
    int n;
    int ret = validate_config(config);

    *out = NULL;
    if (ret != AGENT_OK) return ret;
    state = smart_home_bulk_alloc(sizeof(*state));
    if (!state) return AGENT_ERROR_NOMEM;
    memset(state, 0, sizeof(*state));
    state->request_size = config->request_buffer_size;
    state->response_size = config->response_buffer_size;
    state->request = smart_home_bulk_alloc(state->request_size);
    state->response = smart_home_bulk_alloc(state->response_size);
    state->decoded = smart_home_bulk_alloc(CONFIG_SMART_HOME_MODEL_DECODED_BUFFER_SIZE);
    if (!state->request || !state->response || !state->decoded) {
        ret = AGENT_ERROR_NOMEM;
        goto fail;
    }
    n = snprintf(state->endpoint, sizeof(state->endpoint), "https://%s:%s%s",
                 config->host, config->port, config->path);
    if (n < 0 || (size_t)n >= sizeof(state->endpoint)) {
        ret = AGENT_ERROR_LIMIT;
        goto fail;
    }
    memcpy(state->model, config->model, strlen(config->model) + 1u);
    if (config->api_key[0]) {
        n = snprintf(state->authorization, sizeof(state->authorization),
                     "Bearer %s", config->api_key);
        if (n < 0 || (size_t)n >= sizeof(state->authorization)) {
            ret = AGENT_ERROR_LIMIT;
            goto fail;
        }
    }

    provider.transport = backend->transport;
    provider.runtime = backend->runtime;
    provider.endpoint = agent_string_view(state->endpoint, strlen(state->endpoint));
    provider.model = agent_string_view(state->model, strlen(state->model));
    provider.authorization = agent_string_view(state->authorization,
                                                strlen(state->authorization));
    provider.use_legacy_max_tokens = true;
    provider.request_buffer = state->request;
    provider.request_capacity = state->request_size;
    provider.response_buffer = state->response;
    provider.response_capacity = state->response_size;
    provider.decoded_buffer = state->decoded;
    provider.decoded_capacity = CONFIG_SMART_HOME_MODEL_DECODED_BUFFER_SIZE;
    provider.response_token_capacity = CONFIG_SMART_HOME_MODEL_JSON_TOKENS;
    provider.max_response_header_bytes = sizeof(backend->response_header_text);
    provider.max_json_depth = AGENT_MAX_JSON_DEPTH;
    ret = agent_openai_provider_init(&state->provider, &provider);
    if (ret != AGENT_OK) goto fail;
    *out = state;
    return AGENT_OK;

fail:
    model_state_destroy(state);
    return ret;
}

static agent_error_t model_complete(void *context,
                                    const agent_model_request_t *request,
                                    const agent_model_sink_t *sink)
{
    smart_home_agent_backend_t *backend = context;
    if (!backend->active || !backend->active->authorization[0])
        return AGENT_ERROR_AUTH;
    return agent_openai_model_ops()->complete(&backend->active->provider,
                                               request, sink);
}

int smart_home_agent_backend_apply(smart_home_agent_backend_t *backend,
                                   const smart_home_model_config_t *config)
{
    smart_home_model_state_t *replacement;
    smart_home_model_state_t *previous;
    agent_limits_t limits;
    int ret;

    if (!backend || !backend->agent) return AGENT_ERROR_INVALID;
    ret = model_state_create(backend, config, &replacement);
    if (ret != AGENT_OK) return ret;
    limits = backend->limits;
    limits.timeout_ms = (uint32_t)config->timeout_ms;
    limits.per_model_timeout_ms = (uint32_t)config->timeout_ms;
    if (config->max_output_tokens) limits.max_output_tokens = config->max_output_tokens;
    ret = agent_set_limits(backend->agent, &limits);
    if (ret != AGENT_OK) {
        model_state_destroy(replacement);
        return ret;
    }
    previous = backend->active;
    backend->active = replacement;
    backend->limits = limits;
    backend->transport_state.config.request_timeout_ms = (uint32_t)config->timeout_ms;
    backend->tls.config.io_timeout_ms = (uint32_t)config->timeout_ms;
    model_state_destroy(previous);
    return AGENT_OK;
}

int smart_home_agent_backend_create(smart_home_agent_backend_t **out,
                                    agent_t **agent,
                                    const char *system_prompt,
                                    const smart_home_model_config_t *config)
{
    smart_home_agent_backend_t *backend;
    agent_port_openvela_tls_config_t tls = {0};
    agent_port_openvela_transport_config_t transport = {0};
    agent_config_t core = agent_config_default();
    const agent_model_ops_t model = {.complete = model_complete};
    int ret;

    if (!out || !agent || !system_prompt) return AGENT_ERROR_INVALID;
    *out = NULL;
    *agent = NULL;
    backend = smart_home_bulk_alloc(sizeof(*backend));
    if (!backend) return AGENT_ERROR_NOMEM;
    memset(backend, 0, sizeof(*backend));
    core.limits.max_history_turns = CONFIG_SMART_HOME_SESSION_HISTORY_TURNS;
    backend->limits = core.limits;
    ret = agent_port_openvela_runtime_init(&backend->runtime);
    if (ret != AGENT_OK) goto fail;
    tls.io_timeout_ms = config ? (uint32_t)config->timeout_ms : 0u;
    ret = agent_port_openvela_tls_init(&backend->tls, &tls);
    if (ret != AGENT_OK) goto fail;
    transport.runtime = backend->runtime;
    transport.tls_ops = backend->tls.ops;
    transport.tls_context = &backend->tls;
    transport.url_buffer = backend->url;
    transport.url_buffer_size = sizeof(backend->url);
    transport.request_header_buffer = backend->request_header_text;
    transport.request_header_buffer_size = sizeof(backend->request_header_text);
    transport.request_headers = backend->request_headers;
    transport.request_header_capacity = 16u;
    transport.io_buffer = backend->io;
    transport.io_buffer_size = sizeof(backend->io);
    transport.response_headers = backend->response_headers;
    transport.response_header_capacity = 16u;
    transport.response_header_buffer = backend->response_header_text;
    transport.response_header_buffer_size = sizeof(backend->response_header_text);
    ret = agent_port_openvela_transport_init(&backend->transport,
                                             &backend->transport_state, &transport);
    if (ret != AGENT_OK) goto fail;
    core.runtime = backend->runtime;
    core.system_prompt = agent_string_view(system_prompt, strlen(system_prompt));
    ret = agent_init(&backend->agent, &backend->core_storage, &core);
    if (ret != AGENT_OK) goto fail;
    {
        agent_session_storage_t storage;
        const agent_session_ram_config_t ram = {
            backend->turns, CONFIG_SMART_HOME_SESSION_HISTORY_TURNS + 1u,
            backend->messages, sizeof(backend->messages) / sizeof(backend->messages[0]),
            backend->calls, sizeof(backend->calls) / sizeof(backend->calls[0]),
            backend->session_payload, sizeof(backend->session_payload),
            backend->read_messages, AGENT_MAX_PROJECTED_MESSAGES,
            backend->read_calls, AGENT_MAX_PROJECTED_MESSAGES
        };
        ret = agent_session_ram_init(&backend->session, &ram);
        if (ret == AGENT_OK) ret = agent_session_ram_bind(&backend->session, &storage);
        if (ret == AGENT_OK) ret = agent_set_session_storage(backend->agent, &storage);
        if (ret != AGENT_OK) goto fail;
    }
    ret = agent_model_init(&backend->model, &backend->model_storage,
                           &model, backend);
    if (ret != AGENT_OK) goto fail;
    ret = agent_set_model(backend->agent, backend->model);
    if (ret != AGENT_OK) goto fail;
    ret = smart_home_agent_backend_apply(backend, config);
    if (ret != AGENT_OK) goto fail;
    *agent = backend->agent;
    *out = backend;
    return AGENT_OK;

fail:
    smart_home_agent_backend_destroy(&backend);
    return ret;
}

void smart_home_agent_backend_destroy(smart_home_agent_backend_t **ptr)
{
    smart_home_agent_backend_t *backend;
    if (!ptr || !(backend = *ptr)) return;
    agent_destroy(backend->agent);
    agent_model_destroy(backend->model);
    model_state_destroy(backend->active);
    secure_clear(backend, sizeof(*backend));
    smart_home_bulk_free(backend);
    *ptr = NULL;
}

int smart_home_agent_backend_prepare_turn(smart_home_agent_backend_t *backend)
{
    size_t keep = CONFIG_SMART_HOME_SESSION_HISTORY_TURNS;
    int ret;
    if (!backend) return AGENT_ERROR_INVALID;
    ret = agent_session_ram_trim(&backend->session, keep);
    while (ret == AGENT_OK && keep &&
           backend->session.config.payload_capacity - backend->session.payload_used <
           CONFIG_SMART_HOME_SESSION_PAYLOAD_BYTES / 4u) {
        ret = agent_session_ram_trim(&backend->session, --keep);
    }
    return ret;
}
