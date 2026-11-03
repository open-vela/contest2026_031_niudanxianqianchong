/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* mbedTLS backend for the NuttX webclient TLS ops (blocking adapter). */
#pragma once

#include <agent/runtime.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct webclient_tls_ops;

/* Certificate verification posture. Zero-initialization selects OPTIONAL,
 * the current documented debt: the chain is checked but the result is only
 * logged, so encryption and handshake integrity hold while peer
 * authentication does not. Supply ca_pem with VERIFY_REQUIRED to close it.
 */
typedef enum {
    AGENT_OPENVELA_TLS_VERIFY_OPTIONAL = 0, /* Check the chain, log, continue. */
    AGENT_OPENVELA_TLS_VERIFY_REQUIRED = 1, /* Fail the handshake on an untrusted chain. */
    AGENT_OPENVELA_TLS_VERIFY_NONE = 2      /* Skip chain checks entirely. */
} agent_openvela_tls_verify_t;

typedef struct {
    /* Verification posture; see agent_openvela_tls_verify_t. */
    agent_openvela_tls_verify_t verify;
    /* Borrowed NUL-terminated PEM trust bundle; required for VERIFY_REQUIRED. */
    const char* ca_pem;
    /* Optional diagnostics; mirrors agent_runtime_t.log. */
    void (*log)(void* context, agent_log_level_t level,
                agent_string_view_t message);
    void* log_context;
    /* Per-I/O inactivity ceiling in milliseconds; 0 selects 60000. */
    uint32_t io_timeout_ms;
    /* Maximum bytes handed to one mbedTLS write; 0 selects 1024. Small
     * chunks keep ESP-Hosted (C6) Wi-Fi forwarding from stalling on large
     * writes. */
    size_t write_chunk_bytes;
} agent_port_openvela_tls_config_t;

/* Caller-owned state borrowing a webclient_tls_ops vector. Hand
 * state.ops to the webclient context as tls_ops and &state as tls_context.
 * One request may use the connection at a time; connections are created
 * and destroyed by the ops themselves.
 */
typedef struct {
    const struct webclient_tls_ops* ops;   /* Produced interface. */
    agent_port_openvela_tls_config_t config; /* Copied at init. */
} agent_port_openvela_tls_t;

/* Validate the configuration and fill state->ops. Returns AGENT_ERROR_INVALID
 * when VERIFY_REQUIRED is selected without ca_pem.
 */
agent_error_t agent_port_openvela_tls_init(
    agent_port_openvela_tls_t* state,
    const agent_port_openvela_tls_config_t* config);

#ifdef __cplusplus
}
#endif
