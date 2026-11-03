/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* Internal TLS session core used by tls_ops.c; not a public interface. */
#pragma once

#include <agent_openvela_tls.h>

#include <stddef.h>
#include <sys/types.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

/* One TLS connection. Instances are heap-allocated inside the webclient
 * connect callback and destroyed by the close callback; the caller-owned
 * agent_port_openvela_tls_t only carries the borrowed configuration.
 */
typedef struct agent_openvela_tls_conn {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config cfg;
    mbedtls_net_context net;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_entropy_context entropy;
    mbedtls_x509_crt ca;
    agent_openvela_tls_verify_t verify;
    /* Borrowed PEM bundle incl. terminating NUL; used when verify=REQUIRED. */
    const unsigned char* ca_pem_bytes;
    size_t ca_pem_length;
    uint32_t io_timeout_ms;   /* Per-I/O inactivity ceiling. */
    size_t write_chunk_bytes; /* Max bytes per mbedTLS write (C6 limit). */
    void (*log)(void* context, agent_log_level_t level,
                agent_string_view_t message);
    void* log_context;
} agent_openvela_tls_conn_t;

/* errno-style returns: 0 on success, negative errno on failure. */

agent_openvela_tls_conn_t* agent_openvela_tls_conn_create(void);

/* Covers DNS, TCP connect, and the TLS handshake under one timeout. */
int agent_openvela_tls_conn_connect(agent_openvela_tls_conn_t* conn,
                                    const char* host, const char* port,
                                    uint32_t timeout_ms);

/* Blocking bounded read; 0 denotes an orderly close. */
ssize_t agent_openvela_tls_conn_read(agent_openvela_tls_conn_t* conn,
                                     void* buffer, size_t length);

/* Blocking bounded write of the whole buffer in conn->write_chunk_bytes
 * slices. */
ssize_t agent_openvela_tls_conn_write(agent_openvela_tls_conn_t* conn,
                                      const void* buffer, size_t length);

int agent_openvela_tls_conn_fd(const agent_openvela_tls_conn_t* conn);

/* Closes, frees, and tolerates NULL. */
void agent_openvela_tls_conn_close(agent_openvela_tls_conn_t* conn);
