/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* Provider-neutral HTTP transport validation and synchronous dispatch. */

#include "transport/transport_internal.h"

static bool agent_transport_view_valid(agent_string_view_t value, bool required)
{
    if (value.size != 0u && value.data == NULL)
    {
        return false;
    }

    return !required || value.size != 0u;
}

static bool agent_transport_headers_valid(const agent_http_header_t* headers, size_t count)
{
    size_t index;

    if (count != 0u && headers == NULL)
    {
        return false;
    }

    for (index = 0u; index < count; ++index)
    {
        if (!agent_transport_view_valid(headers[index].name, true) ||
            !agent_transport_view_valid(headers[index].value, false))
        {
            return false;
        }
    }

    return true;
}

agent_error_t agent_transport_validate(const agent_transport_t* transport,
                                       const agent_http_request_t* request,
                                       const agent_http_sink_t* sink)
{
    if (transport == NULL || transport->ops == NULL || transport->ops->request == NULL ||
        request == NULL || sink == NULL || sink->headers == NULL || sink->body == NULL)
    {
        return AGENT_ERROR_INVALID;
    }

    if (!agent_transport_view_valid(request->method, true) ||
        !agent_transport_view_valid(request->url, true) ||
        !agent_transport_headers_valid(request->headers, request->header_count) ||
        (request->body_size != 0u && request->body == NULL) ||
        request->max_response_bytes == 0u || request->max_response_header_bytes == 0u)
    {
        return AGENT_ERROR_INVALID;
    }

    return AGENT_OK;
}

agent_error_t agent_transport_request(const agent_transport_t* transport,
                                      const agent_http_request_t* request,
                                      const agent_http_sink_t* sink)
{
    agent_error_t status = agent_transport_validate(transport, request, sink);

    if (status != AGENT_OK)
    {
        return status;
    }

    return transport->ops->request(transport->context, request, sink);
}
