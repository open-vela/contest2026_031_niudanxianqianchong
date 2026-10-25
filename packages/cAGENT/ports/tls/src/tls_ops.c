/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* webclient_tls_ops adapter over the TLS session core (tls_core.c). */
#include <agent_openvela_tls.h>

#include <errno.h>
#include <string.h>

#include <netutils/webclient.h>

#include "tls_internal.h"

static agent_port_openvela_tls_t* agent_ov_tls_state(void* context)
{
    return (agent_port_openvela_tls_t*)context;
}

static int agent_ov_tls_ops_connect(
    void* context, const char* hostname, const char* port,
    unsigned int timeout_second, struct webclient_tls_connection** connp)
{
    agent_port_openvela_tls_t* state = agent_ov_tls_state(context);
    agent_openvela_tls_conn_t* conn;
    uint32_t timeout_ms;
    int ret;

    if (state == NULL || hostname == NULL || port == NULL || connp == NULL)
    {
        return -EINVAL;
    }
    conn = agent_openvela_tls_conn_create();
    if (conn == NULL)
    {
        return -ENOMEM;
    }
    conn->verify = state->config.verify;
    conn->log = state->config.log;
    conn->log_context = state->config.log_context;
    if (state->config.io_timeout_ms != 0u)
    {
        conn->io_timeout_ms = state->config.io_timeout_ms;
    }
    if (state->config.write_chunk_bytes != 0u)
    {
        conn->write_chunk_bytes = state->config.write_chunk_bytes;
    }
    if (state->config.verify == AGENT_OPENVELA_TLS_VERIFY_REQUIRED)
    {
        conn->ca_pem_bytes = (const unsigned char*)state->config.ca_pem;
        conn->ca_pem_length = strlen(state->config.ca_pem) + 1u;
    }

    timeout_ms = timeout_second != 0u
        ? (uint32_t)timeout_second * 1000u
        : conn->io_timeout_ms;
    ret = agent_openvela_tls_conn_connect(conn, hostname, port, timeout_ms);
    if (ret != 0)
    {
        agent_openvela_tls_conn_close(conn);
        return ret;
    }
    *connp = (struct webclient_tls_connection*)conn;
    return 0;
}

static ssize_t agent_ov_tls_ops_send(void* context,
                                     struct webclient_tls_connection* conn,
                                     const void* buffer, size_t length)
{
    (void)context;
    return agent_openvela_tls_conn_write(
        (agent_openvela_tls_conn_t*)conn, buffer, length);
}

static ssize_t agent_ov_tls_ops_recv(void* context,
                                     struct webclient_tls_connection* conn,
                                     void* buffer, size_t length)
{
    (void)context;
    return agent_openvela_tls_conn_read(
        (agent_openvela_tls_conn_t*)conn, buffer, length);
}

static int agent_ov_tls_ops_close(void* context,
                                  struct webclient_tls_connection* conn)
{
    (void)context;
    agent_openvela_tls_conn_close((agent_openvela_tls_conn_t*)conn);
    return 0;
}

/* Blocking adapter: the session core drives poll() internally, so
 * non-blocking webclient mode (the only get_poll_info caller) is not
 * supported.
 */
static int agent_ov_tls_ops_get_poll_info(
    void* context, struct webclient_tls_connection* conn,
    struct webclient_poll_info* info)
{
    (void)context;
    (void)conn;
    (void)info;
    return -ENOTSUP;
}

/* The callbacks are stateless; per-instance behaviour arrives through the
 * webclient TLS context (ctx). Proxy tunneling (init_connection) stays
 * NULL.
 */
static const struct webclient_tls_ops agent_ov_tls_ops_table = {
    agent_ov_tls_ops_connect,
    agent_ov_tls_ops_send,
    agent_ov_tls_ops_recv,
    agent_ov_tls_ops_close,
    agent_ov_tls_ops_get_poll_info,
    NULL
};

agent_error_t agent_port_openvela_tls_init(
    agent_port_openvela_tls_t* state,
    const agent_port_openvela_tls_config_t* config)
{
    if (state == NULL || config == NULL)
    {
        return AGENT_ERROR_INVALID;
    }
    if (config->verify == AGENT_OPENVELA_TLS_VERIFY_REQUIRED &&
        (config->ca_pem == NULL || config->ca_pem[0] == '\0'))
    {
        return AGENT_ERROR_INVALID;
    }
    state->config = *config;
    state->ops = &agent_ov_tls_ops_table;
    return AGENT_OK;
}
