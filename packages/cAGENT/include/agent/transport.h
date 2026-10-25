/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* Provider-facing synchronous HTTP transport, independent of Core lifecycle. */
#pragma once

#include <agent/error.h>
#include <agent/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* HTTP header; name/value are length-delimited, without CR/LF or NUL.  */
typedef struct {
    agent_string_view_t name;  /* Nonempty header name. */
    agent_string_view_t value; /* Header value. */
} agent_http_header_t;

/* One bounded HTTP exchange, borrowed until request returns. */
typedef struct {
    agent_string_view_t method;         /* Explicit HTTP method token. */
    agent_string_view_t url;            /* Absolute http/https URL; implementation validates. */
    const agent_http_header_t* headers; /* Borrowed array, NULL only for count=0. */
    size_t header_count;                /* Header count. */
    const void* body;                   /* Borrowed bytes; NULL only if body_size=0. */
    size_t body_size;                   /* Byte count, not necessarily UTF-8. */
    size_t max_response_bytes;          /* Required nonzero body byte ceiling. */
    size_t max_response_header_bytes;   /* Required nonzero aggregate header ceiling. */
    uint64_t deadline_ms;               /* Absolute runtime deadline; 0=none. */
    const agent_cancel_token_t* cancel; /* Optional borrowed token. */
} agent_http_request_t;

/* Response consumer; callbacks borrow bytes only for their duration. */
typedef struct {
    agent_error_t (*headers)(void* context, unsigned int status,
                                  const agent_http_header_t* headers,
                                  size_t count);                /* Final HTTP headers. */
    agent_error_t (*body)(void* context, const void* data,
                               size_t size);                    /* Decoded transfer body chunk. */
    void* context;                                             /* Borrowed receiver state. */
} agent_http_sink_t;

/* Synchronous transport operations; no implicit ownership transfer. */
typedef struct {
    agent_error_t (*request)(void* context, const agent_http_request_t* request,
                                  const agent_http_sink_t* sink); /* Required blocking exchange. */
} agent_transport_ops_t;

/* Allocation-free transport binding, passed through model configuration. */
typedef struct {
    const agent_transport_ops_t* ops; /* Immutable, non-NULL operations table. */
    void* context;                    /* Platform-specific state; no OS types exposed here. */
} agent_transport_t;

#ifdef __cplusplus
}
#endif
