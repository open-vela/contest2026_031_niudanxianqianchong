/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* NuttX webclient HTTP adapter; TLS is provided by the application. */
#pragma once

#include <agent/runtime.h>
#include <agent/transport.h>

#ifdef __cplusplus
extern "C" {
#endif

struct webclient_tls_ops;

typedef struct {
    agent_runtime_t runtime;                    /* Copied; now_ms is required. */
    const struct webclient_tls_ops* tls_ops;    /* Borrowed; HTTPS requires authenticated TLS. */
    void* tls_context;                          /* Borrowed TLS state. */
    uint32_t request_timeout_ms;                /* Per-I/O inactivity ceiling. */
    char* url_buffer;                           /* Caller-owned NUL-termination buffer. */
    size_t url_buffer_size;
    char* request_header_buffer;                /* Caller-owned "Name: Value" text. */
    size_t request_header_buffer_size;
    const char** request_headers;               /* Caller-owned descriptor array. */
    size_t request_header_capacity;
    char* io_buffer;                            /* webclient request/response staging. */
    size_t io_buffer_size;
    agent_http_header_t* response_headers;      /* Caller-owned descriptor array. */
    size_t response_header_capacity;
    char* response_header_buffer;               /* Caller-owned header text. */
    size_t response_header_buffer_size;
} agent_port_openvela_transport_config_t;

typedef struct {
    agent_port_openvela_transport_config_t config;
    bool active;                                 /* One request at a time. */
} agent_port_openvela_transport_t;

agent_error_t agent_port_openvela_transport_init(
    agent_transport_t* out, agent_port_openvela_transport_t* state,
    const agent_port_openvela_transport_config_t* config);

#ifdef __cplusplus
}
#endif
