/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* NuttX webclient adapter with borrowed buffers and synchronous sink delivery. */

#include <agent_openvela_transport.h>
#include <agent.h>

#include <netutils/webclient.h>

#include <errno.h>
#include <limits.h>
#include <string.h>

typedef struct {
    agent_port_openvela_transport_t* transport;
    const agent_http_request_t* request;
    const agent_http_sink_t* sink;
    struct webclient_context* client;
    size_t header_bytes;
    size_t header_text_used;
    size_t header_count;
    size_t body_bytes;
    bool headers_sent;
    agent_error_t status;
} agent_port_openvela_exchange_t;

static agent_error_t agent_port_openvela_poll(const agent_port_openvela_exchange_t* exchange)
{
    if (exchange->request->cancel != NULL &&
        agent_cancel_token_is_set(exchange->request->cancel))
    {
        return AGENT_ERROR_CANCELLED;
    }
    if (exchange->request->deadline_ms != 0u &&
        exchange->transport->config.runtime.now_ms(
            exchange->transport->config.runtime.clock_context) >= exchange->request->deadline_ms)
    {
        return AGENT_ERROR_TIMEOUT;
    }
    return AGENT_OK;
}

static agent_error_t agent_port_openvela_map_error(int error)
{
    if (error == 0)
    {
        return AGENT_OK;
    }
    if (error == -ETIMEDOUT)
    {
        return AGENT_ERROR_TIMEOUT;
    }
    if (error == -ENOMEM)
    {
        return AGENT_ERROR_NOMEM;
    }
    if (error == -E2BIG || error == -ENOBUFS)
    {
        return AGENT_ERROR_CAPACITY;
    }
    if (error == -ENOTSUP)
    {
        return AGENT_ERROR_NOT_SUPPORTED;
    }
    return AGENT_ERROR_IO;
}

static agent_error_t agent_port_openvela_copy_url(char* out, size_t capacity,
                                                   agent_string_view_t url, bool* https)
{
    size_t index;

    if (out == NULL || url.data == NULL || https == NULL)
    {
        return AGENT_ERROR_INVALID;
    }
    if (url.size > 8u && memcmp(url.data, "https://", 8u) == 0)
    {
        *https = true;
    }
    else if (url.size > 7u && memcmp(url.data, "http://", 7u) == 0)
    {
        *https = false;
    }
    else
    {
        return AGENT_ERROR_NOT_SUPPORTED;
    }
    if (url.size >= capacity)
    {
        return AGENT_ERROR_CAPACITY;
    }
    for (index = 0u; index < url.size; ++index)
    {
        if ((unsigned char)url.data[index] <= 0x20u || (unsigned char)url.data[index] == 0x7fu)
        {
            return AGENT_ERROR_INVALID;
        }
    }
    memcpy(out, url.data, url.size);
    out[url.size] = '\0';
    return AGENT_OK;
}

static agent_error_t agent_port_openvela_headers(agent_port_openvela_exchange_t* exchange)
{
    if (exchange->headers_sent)
    {
        return AGENT_OK;
    }
    if (exchange->client->http_status == 0u)
    {
        return AGENT_ERROR_IO;
    }
    exchange->status = exchange->sink->headers(exchange->sink->context,
                                               exchange->client->http_status,
                                               exchange->transport->config.response_headers,
                                               exchange->header_count);
    if (exchange->status == AGENT_OK)
    {
        exchange->headers_sent = true;
    }
    return exchange->status;
}

static int agent_port_openvela_header(const char* line, bool truncated, void* context)
{
    agent_port_openvela_exchange_t* exchange = (agent_port_openvela_exchange_t*)context;
    agent_port_openvela_transport_config_t* config = &exchange->transport->config;
    const char* colon;
    const char* value;
    size_t name_size;
    size_t value_size;
    size_t required;
    char* text;

    exchange->status = agent_port_openvela_poll(exchange);
    if (exchange->status != AGENT_OK)
    {
        return -ECANCELED;
    }
    /* webclient otherwise follows redirects, possibly forwarding credentials. */
    if (exchange->client->http_status / 100u == 3u)
    {
        exchange->status = AGENT_ERROR_NOT_SUPPORTED;
        return -EOPNOTSUPP;
    }
    if (truncated || line == NULL || exchange->header_count >= config->response_header_capacity)
    {
        exchange->status = AGENT_ERROR_CAPACITY;
        return -E2BIG;
    }
    colon = strchr(line, ':');
    if (colon == NULL || colon == line)
    {
        exchange->status = AGENT_ERROR_PARSE;
        return -EINVAL;
    }
    value = colon + 1;
    while (*value == ' ' || *value == '\t')
    {
        ++value;
    }
    name_size = (size_t)(colon - line);
    value_size = strlen(value);
    if (exchange->header_bytes > exchange->request->max_response_header_bytes ||
        name_size > exchange->request->max_response_header_bytes - exchange->header_bytes ||
        value_size > exchange->request->max_response_header_bytes - exchange->header_bytes - name_size ||
        exchange->header_text_used > config->response_header_buffer_size ||
        name_size >= config->response_header_buffer_size - exchange->header_text_used ||
        config->response_header_buffer_size - exchange->header_text_used - name_size < 2u ||
        value_size > config->response_header_buffer_size - exchange->header_text_used - name_size - 2u)
    {
        exchange->status = AGENT_ERROR_CAPACITY;
        return -E2BIG;
    }
    required = name_size + value_size + 2u;
    if (required > config->response_header_buffer_size - exchange->header_text_used)
    {
        exchange->status = AGENT_ERROR_CAPACITY;
        return -E2BIG;
    }
    text = config->response_header_buffer + exchange->header_text_used;
    memcpy(text, line, name_size);
    text[name_size] = '\0';
    config->response_headers[exchange->header_count].name = agent_string_view(text, name_size);
    text += name_size + 1u;
    memcpy(text, value, value_size);
    text[value_size] = '\0';
    config->response_headers[exchange->header_count].value = agent_string_view(text, value_size);
    exchange->header_count += 1u;
    exchange->header_bytes += name_size + value_size;
    exchange->header_text_used += required;
    return 0;
}

static int agent_port_openvela_body(char** buffer, int offset, int datend,
                                     int* buflen, void* context)
{
    agent_port_openvela_exchange_t* exchange = (agent_port_openvela_exchange_t*)context;
    size_t size;

    exchange->status = agent_port_openvela_poll(exchange);
    if (exchange->status == AGENT_OK)
    {
        exchange->status = agent_port_openvela_headers(exchange);
    }
    if (exchange->status != AGENT_OK)
    {
        return -ECANCELED;
    }
    if (buffer == NULL || buflen == NULL || *buffer == NULL || offset < 0 ||
        datend < offset || datend > *buflen)
    {
        exchange->status = AGENT_ERROR_IO;
        return -EIO;
    }
    size = (size_t)(datend - offset);
    if (exchange->body_bytes > exchange->request->max_response_bytes ||
        size > exchange->request->max_response_bytes - exchange->body_bytes)
    {
        exchange->status = AGENT_ERROR_CAPACITY;
        return -E2BIG;
    }
    if (size != 0u)
    {
        exchange->status = exchange->sink->body(exchange->sink->context, *buffer + offset, size);
        if (exchange->status != AGENT_OK)
        {
            return -ECANCELED;
        }
        exchange->body_bytes += size;
    }
    return 0;
}

static agent_error_t agent_port_openvela_request_headers(
    agent_port_openvela_transport_config_t* config, const agent_http_request_t* request)
{
    size_t index;
    size_t used = 0u;

    if (request->header_count > config->request_header_capacity)
    {
        return AGENT_ERROR_CAPACITY;
    }
    for (index = 0u; index < request->header_count; ++index)
    {
        agent_string_view_t name = request->headers[index].name;
        agent_string_view_t value = request->headers[index].value;
        size_t j;
        size_t required;
        char* text;

        if (name.data == NULL || name.size == 0u ||
            (value.size != 0u && value.data == NULL))
        {
            return AGENT_ERROR_INVALID;
        }
        for (j = 0u; j < name.size; ++j)
        {
            if ((unsigned char)name.data[j] <= 0x20u || name.data[j] == ':' ||
                (unsigned char)name.data[j] >= 0x7fu)
            {
                return AGENT_ERROR_INVALID;
            }
        }
        for (j = 0u; j < value.size; ++j)
        {
            if (((unsigned char)value.data[j] < 0x20u && value.data[j] != '\t') ||
                (unsigned char)value.data[j] == 0x7fu)
            {
                return AGENT_ERROR_INVALID;
            }
        }
        if (used > config->request_header_buffer_size ||
            name.size >= config->request_header_buffer_size - used ||
            config->request_header_buffer_size - used - name.size < 3u ||
            value.size > config->request_header_buffer_size - used - name.size - 3u)
        {
            return AGENT_ERROR_CAPACITY;
        }
        required = name.size + value.size + 3u;
        text = config->request_header_buffer + used;
        memcpy(text, name.data, name.size);
        text[name.size] = ':';
        text[name.size + 1u] = ' ';
        if (value.size != 0u)
        {
            memcpy(text + name.size + 2u, value.data, value.size);
        }
        text[name.size + 2u + value.size] = '\0';
        config->request_headers[index] = text;
        used += required;
    }
    return AGENT_OK;
}

static agent_error_t agent_port_openvela_request(void* context,
                                                  const agent_http_request_t* request,
                                                  const agent_http_sink_t* sink)
{
    agent_port_openvela_transport_t* transport = (agent_port_openvela_transport_t*)context;
    agent_port_openvela_exchange_t exchange;
    struct webclient_context client;
    agent_error_t status;
    bool https;
    uint64_t timeout_ms;
    int result;

    if (transport == NULL || request == NULL || sink == NULL || sink->headers == NULL ||
        sink->body == NULL || request->method.data == NULL || request->method.size == 0u ||
        (request->header_count != 0u && request->headers == NULL) ||
        (request->body_size != 0u && request->body == NULL) ||
        request->max_response_bytes == 0u || request->max_response_header_bytes == 0u)
    {
        return AGENT_ERROR_INVALID;
    }
    if (transport->active)
    {
        return AGENT_ERROR_BUSY;
    }
    if (request->header_count > UINT_MAX)
    {
        return AGENT_ERROR_LIMIT;
    }
    status = agent_port_openvela_copy_url(transport->config.url_buffer,
                                          transport->config.url_buffer_size,
                                          request->url, &https);
    if (status != AGENT_OK)
    {
        return status;
    }
    if (https && transport->config.tls_ops == NULL)
    {
        return AGENT_ERROR_NOT_SUPPORTED;
    }
    status = agent_port_openvela_request_headers(&transport->config, request);
    if (status != AGENT_OK)
    {
        return status;
    }

    exchange = (agent_port_openvela_exchange_t){0};
    exchange.transport = transport;
    exchange.request = request;
    exchange.sink = sink;
    status = agent_port_openvela_poll(&exchange);
    if (status != AGENT_OK)
    {
        return status;
    }
    timeout_ms = transport->config.request_timeout_ms;
    if (request->deadline_ms != 0u)
    {
        uint64_t now = transport->config.runtime.now_ms(
            transport->config.runtime.clock_context);
        uint64_t remaining;

        if (now >= request->deadline_ms)
        {
            return AGENT_ERROR_TIMEOUT;
        }
        remaining = request->deadline_ms - now;
        if (remaining < timeout_ms)
        {
            timeout_ms = remaining;
        }
    }
    webclient_set_defaults(&client);
    client.method = NULL;
    if (request->method.size == 3u && memcmp(request->method.data, "GET", 3u) == 0)
    {
        client.method = "GET";
    }
    else if (request->method.size == 4u && memcmp(request->method.data, "POST", 4u) == 0)
    {
        client.method = "POST";
    }
    else
    {
        return AGENT_ERROR_NOT_SUPPORTED;
    }
    client.url = transport->config.url_buffer;
    client.protocol_version = WEBCLIENT_PROTOCOL_VERSION_HTTP_1_1;
    client.headers = transport->config.request_headers;
    client.nheaders = (unsigned int)request->header_count;
    client.timeout_sec = (unsigned int)((timeout_ms + 999u) / 1000u);
    client.buffer = transport->config.io_buffer;
    client.buflen = (int)transport->config.io_buffer_size;
    client.tls_ops = transport->config.tls_ops;
    client.tls_ctx = transport->config.tls_context;
    client.header_callback = agent_port_openvela_header;
    client.header_callback_arg = &exchange;
    client.sink_callback = agent_port_openvela_body;
    client.sink_callback_arg = &exchange;
    if (request->body_size != 0u)
    {
        webclient_set_static_body(&client, request->body, request->body_size);
    }
    exchange.client = &client;
    exchange.status = AGENT_OK;
    transport->active = true;
    result = webclient_perform(&client);
    transport->active = false;
    if (exchange.status != AGENT_OK)
    {
        return exchange.status;
    }
    status = agent_port_openvela_map_error(result);
    if (status != AGENT_OK)
    {
        return status;
    }
    status = agent_port_openvela_poll(&exchange);
    return status == AGENT_OK ? agent_port_openvela_headers(&exchange) : status;
}

static const agent_transport_ops_t agent_port_openvela_transport_ops = {
    agent_port_openvela_request,
};

agent_error_t agent_port_openvela_transport_init(
    agent_transport_t* out, agent_port_openvela_transport_t* state,
    const agent_port_openvela_transport_config_t* config)
{
    if (out == NULL || state == NULL || config == NULL || config->runtime.now_ms == NULL ||
        config->request_timeout_ms == 0u || config->url_buffer == NULL ||
        config->url_buffer_size == 0u || config->request_header_buffer == NULL ||
        config->request_header_buffer_size == 0u || config->request_headers == NULL ||
        config->request_header_capacity == 0u || config->io_buffer == NULL ||
        config->io_buffer_size == 0u || config->io_buffer_size > (size_t)INT_MAX ||
        config->response_headers == NULL || config->response_header_capacity == 0u ||
        config->response_header_buffer == NULL || config->response_header_buffer_size == 0u)
    {
        return AGENT_ERROR_INVALID;
    }
    state->config = *config;
    state->active = false;
    out->ops = &agent_port_openvela_transport_ops;
    out->context = state;
    return AGENT_OK;
}
