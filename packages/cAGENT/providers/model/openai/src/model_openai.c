/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* Provider lifecycle and one synchronous, non-streaming HTTP exchange. */

#include "openai_internal.h"

#include <agent.h>

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    char prefix;
    jsmntok_t token;
} agent_openai_token_alignment_t;

typedef struct {
    agent_openai_provider_t* provider;
    const agent_cancel_token_t* cancel;
    uint64_t deadline_ms;
    size_t response_size;
    unsigned int http_status;
    bool has_headers;
    agent_error_t callback_error;
} agent_openai_exchange_t;

static bool valid_view(agent_string_view_t value, bool required)
{
    return (value.size == 0u || value.data != NULL) && (!required || value.size != 0u);
}

static bool valid_header_value(agent_string_view_t value)
{
    size_t i;

    for (i = 0u; i < value.size; ++i) {
        unsigned char c = (unsigned char)value.data[i];
        if (c < 0x20u || c >= 0x7fu) {
            return false;
        }
    }
    return true;
}

static bool valid_endpoint(agent_string_view_t value)
{
    size_t i;

    for (i = 0u; i < value.size; ++i) {
        unsigned char c = (unsigned char)value.data[i];
        if (c <= 0x20u || c >= 0x7fu) {
            return false;
        }
    }
    return true;
}

static bool storage_valid(const void* buffer, size_t size)
{
    return buffer != NULL && size != 0u &&
           size - 1u <= UINTPTR_MAX - (uintptr_t)buffer;
}

static bool storage_overlaps(const void* first, size_t first_size,
                             const void* second, size_t second_size)
{
    uintptr_t a = (uintptr_t)first;
    uintptr_t b = (uintptr_t)second;

    return a <= b + second_size - 1u && b <= a + first_size - 1u;
}

static bool config_text_overlaps_storage(const agent_openai_config_t* config,
                                         agent_string_view_t text)
{
    if (text.size == 0u) return false;
    if (!storage_valid(text.data, text.size)) return true;
    return storage_overlaps(text.data, text.size,
                            config->request_buffer, config->request_capacity) ||
           storage_overlaps(text.data, text.size,
                            config->response_buffer, config->response_capacity) ||
           storage_overlaps(text.data, text.size,
                            config->decoded_buffer, config->decoded_capacity);
}

static size_t token_padding(const char* buffer)
{
    size_t alignment = offsetof(agent_openai_token_alignment_t, token);
    size_t remainder = (size_t)((uintptr_t)buffer % alignment);

    return remainder == 0u ? 0u : alignment - remainder;
}

jsmntok_t* agent_openai_token_buffer(agent_openai_provider_t* provider)
{
    return (jsmntok_t*)(void*)(provider->config.request_buffer +
                               token_padding(provider->config.request_buffer));
}

agent_error_t agent_openai_provider_init(agent_openai_provider_t* provider,
                                          const agent_openai_config_t* config)
{
    bool https;
    size_t padding;

    if (provider == NULL || config == NULL) {
        return AGENT_ERROR_INVALID;
    }
    memset(provider, 0, sizeof(*provider));
    if (config->transport.ops == NULL || config->transport.ops->request == NULL ||
        config->runtime.now_ms == NULL || !valid_view(config->endpoint, true) ||
        !valid_view(config->model, true) || !valid_view(config->authorization, false) ||
        !valid_header_value(config->authorization) ||
        !storage_valid(config->request_buffer, config->request_capacity) ||
        !storage_valid(config->response_buffer, config->response_capacity) ||
        !storage_valid(config->decoded_buffer, config->decoded_capacity) ||
        storage_overlaps(config->request_buffer, config->request_capacity,
                         config->response_buffer, config->response_capacity) ||
        storage_overlaps(config->request_buffer, config->request_capacity,
                         config->decoded_buffer, config->decoded_capacity) ||
        storage_overlaps(config->response_buffer, config->response_capacity,
                         config->decoded_buffer, config->decoded_capacity) ||
        config->response_capacity > INT_MAX ||
        config->response_token_capacity == 0u ||
        config->response_token_capacity > UINT_MAX ||
        config->max_response_header_bytes == 0u || config->max_json_depth == 0u ||
        config->max_json_depth > 32u) {
        return AGENT_ERROR_INVALID;
    }
    if (config_text_overlaps_storage(config, config->endpoint) ||
        config_text_overlaps_storage(config, config->model) ||
        config_text_overlaps_storage(config, config->authorization)) {
        return AGENT_ERROR_INVALID;
    }
    https = config->endpoint.size > 8u &&
            memcmp(config->endpoint.data, "https://", 8u) == 0;
    if (!https && (config->endpoint.size <= 7u ||
                   memcmp(config->endpoint.data, "http://", 7u) != 0)) {
        return AGENT_ERROR_INVALID;
    }
    if (!valid_endpoint(config->endpoint)) {
        return AGENT_ERROR_INVALID;
    }
    if (config->authorization.size != 0u &&
        (!https || config->authorization.size <= 7u ||
         memcmp(config->authorization.data, "Bearer ", 7u) != 0)) {
        return AGENT_ERROR_INVALID;
    }
    padding = token_padding(config->request_buffer);
    if (padding >= config->request_capacity ||
        config->response_token_capacity >
            (config->request_capacity - padding) / sizeof(jsmntok_t)) {
        return AGENT_ERROR_CAPACITY;
    }
    provider->config = *config;
    return AGENT_OK;
}

static agent_error_t poll_exchange(const agent_openai_exchange_t* exchange)
{
    if (exchange->cancel != NULL && agent_cancel_token_is_set(exchange->cancel)) {
        return AGENT_ERROR_CANCELLED;
    }
    if (exchange->deadline_ms != 0u &&
        exchange->provider->config.runtime.now_ms(
            exchange->provider->config.runtime.clock_context) >= exchange->deadline_ms) {
        return AGENT_ERROR_TIMEOUT;
    }
    return AGENT_OK;
}

static agent_error_t http_status_error(unsigned int status)
{
    if (status == 401u || status == 403u) {
        return AGENT_ERROR_AUTH;
    }
    if (status == 429u) {
        return AGENT_ERROR_MODEL_RATE_LIMIT;
    }
    if (status == 408u || status == 504u) {
        return AGENT_ERROR_TIMEOUT;
    }
    if (status >= 500u && status < 600u) {
        return AGENT_ERROR_MODEL_UNAVAILABLE;
    }
    return AGENT_ERROR_MODEL_FAILED;
}

static agent_error_t receive_headers(void* context, unsigned int status,
                                     const agent_http_header_t* headers, size_t count)
{
    agent_openai_exchange_t* exchange = context;
    agent_error_t result;

    (void)headers;
    (void)count;
    if (exchange->callback_error != AGENT_OK) return exchange->callback_error;
    result = poll_exchange(exchange);
    if (result != AGENT_OK) {
        exchange->callback_error = result;
        return result;
    }
    if (exchange->has_headers || status < 100u || status > 599u) {
        exchange->callback_error = AGENT_ERROR_IO;
        return exchange->callback_error;
    }
    exchange->has_headers = true;
    exchange->http_status = status;
    result = status >= 200u && status < 300u ? AGENT_OK : http_status_error(status);
    exchange->callback_error = result;
    return result;
}

static agent_error_t receive_body(void* context, const void* data, size_t size)
{
    agent_openai_exchange_t* exchange = context;
    agent_openai_config_t* config = &exchange->provider->config;
    agent_error_t poll_status;

    if (exchange->callback_error != AGENT_OK) return exchange->callback_error;
    poll_status = poll_exchange(exchange);
    if (poll_status != AGENT_OK) {
        exchange->callback_error = poll_status;
        return poll_status;
    }
    if (!exchange->has_headers || exchange->http_status < 200u ||
        exchange->http_status >= 300u || (size != 0u && data == NULL)) {
        exchange->callback_error = AGENT_ERROR_IO;
        return exchange->callback_error;
    }
    if (size > config->response_capacity - exchange->response_size) {
        exchange->callback_error = AGENT_ERROR_CAPACITY;
        return exchange->callback_error;
    }
    if (size != 0u) {
        memcpy(config->response_buffer + exchange->response_size, data, size);
        exchange->response_size += size;
    }
    return AGENT_OK;
}

static agent_error_t effective_deadline(const agent_openai_provider_t* provider,
                                        const agent_model_request_t* request,
                                        uint64_t* deadline)
{
    uint64_t now = provider->config.runtime.now_ms(provider->config.runtime.clock_context);

    *deadline = request->deadline_ms;
    if (*deadline != 0u && now >= *deadline) {
        return AGENT_ERROR_TIMEOUT;
    }
    if (request->timeout_ms != 0u) {
        uint64_t call_deadline = UINT64_MAX - now < request->timeout_ms ?
                                 UINT64_MAX : now + request->timeout_ms;
        if (*deadline == 0u || call_deadline < *deadline) {
            *deadline = call_deadline;
        }
    }
    return *deadline != 0u && now >= *deadline ? AGENT_ERROR_TIMEOUT : AGENT_OK;
}

static agent_error_t openai_complete(void* context, const agent_model_request_t* request,
                                     const agent_model_sink_t* sink)
{
    agent_openai_provider_t* provider = context;
    agent_openai_exchange_t exchange = {0};
    agent_http_header_t headers[2];
    agent_http_request_t http_request = {0};
    agent_http_sink_t http_sink = {receive_headers, receive_body, &exchange};
    agent_string_view_t body;
    agent_error_t status;

    if (provider == NULL || request == NULL || sink == NULL || sink->text == NULL ||
        sink->tool_call == NULL || provider->config.transport.ops == NULL) {
        return AGENT_ERROR_INVALID;
    }
    if (provider->active) {
        return AGENT_ERROR_BUSY;
    }
    provider->active = true;
    if (request->cancel != NULL && agent_cancel_token_is_set(request->cancel)) {
        status = AGENT_ERROR_CANCELLED;
        goto done;
    }
    status = effective_deadline(provider, request, &http_request.deadline_ms);
    if (status != AGENT_OK) {
        goto done;
    }
    status = agent_openai_write_request(provider, request, &body);
    if (status != AGENT_OK) {
        goto done;
    }

    headers[0].name = OPENAI_LITERAL("Content-Type");
    headers[0].value = OPENAI_LITERAL("application/json");
    headers[1].name = OPENAI_LITERAL("Authorization");
    headers[1].value = provider->config.authorization;
    http_request.method = OPENAI_LITERAL("POST");
    http_request.url = provider->config.endpoint;
    http_request.headers = headers;
    http_request.header_count = provider->config.authorization.size == 0u ? 1u : 2u;
    http_request.body = body.data;
    http_request.body_size = body.size;
    http_request.max_response_bytes = provider->config.response_capacity;
    http_request.max_response_header_bytes = provider->config.max_response_header_bytes;
    http_request.cancel = request->cancel;
    exchange.provider = provider;
    exchange.cancel = request->cancel;
    exchange.deadline_ms = http_request.deadline_ms;
    status = poll_exchange(&exchange);
    if (status != AGENT_OK) goto done;

    status = provider->config.transport.ops->request(provider->config.transport.context,
                                                      &http_request, &http_sink);
    if (exchange.callback_error != AGENT_OK) {
        status = exchange.callback_error;
    }
    if (status != AGENT_OK) {
        goto done;
    }
    if (!exchange.has_headers) {
        status = AGENT_ERROR_IO;
        goto done;
    }
    if (exchange.http_status < 200u || exchange.http_status >= 300u) {
        status = http_status_error(exchange.http_status);
        goto done;
    }
    status = poll_exchange(&exchange);
    if (status != AGENT_OK) {
        goto done;
    }
    status = agent_openai_read_response(provider, exchange.response_size, sink);

done:
    provider->active = false;
    return status;
}

const agent_model_ops_t* agent_openai_model_ops(void)
{
    static const agent_model_ops_t ops = {openai_complete, NULL};

    return &ops;
}
