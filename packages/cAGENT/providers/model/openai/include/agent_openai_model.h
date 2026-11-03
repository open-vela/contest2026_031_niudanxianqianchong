/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* Bounded, synchronous Chat Completions provider; all storage is caller-owned. */
#pragma once

#include <agent/model.h>
#include <agent/transport.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    agent_transport_t transport;          /* Borrowed synchronous HTTP backend. */
    agent_runtime_t runtime;              /* Monotonic clock; same time base as Core. */
    agent_string_view_t endpoint;         /* Complete chat/completions URL. */
    agent_string_view_t model;            /* Model identifier. */
    agent_string_view_t authorization;    /* Optional complete Bearer header value. */
    bool use_legacy_max_tokens;           /* Emit "max_tokens" instead of
                                             "max_completion_tokens"; some
                                             OpenAI-compatible gateways accept
                                             only the legacy field. */
    char* request_buffer;                 /* JSON body, then response token storage. */
    size_t request_capacity;
    char* response_buffer;                /* Non-streaming response bytes. */
    size_t response_capacity;
    char* decoded_buffer;                 /* Decoded text or one complete Tool call. */
    size_t decoded_capacity;
    size_t response_token_capacity;       /* Maximum outer JSON tokens. */
    size_t max_response_header_bytes;     /* HTTP response header budget. */
    size_t max_json_depth;                /* 1..32; applies to every JSON document. */
} agent_openai_config_t;

/* One active completion per instance; borrowed Model input and sink state must not alias buffers. */
typedef struct {
    agent_openai_config_t config;
    bool active;
} agent_openai_provider_t;

/* Validates borrowed configuration and buffers; performs no I/O or allocation. */
agent_error_t agent_openai_provider_init(agent_openai_provider_t* provider,
                                          const agent_openai_config_t* config);

/* Pass this ops table and the provider as context to agent_model_init(). */
const agent_model_ops_t* agent_openai_model_ops(void);

#ifdef __cplusplus
}
#endif
