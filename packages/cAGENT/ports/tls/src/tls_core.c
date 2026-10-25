/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: 2026 openvela ai_agent contributors
 * SPDX-FileCopyrightText: 2026 contest2026 cAGENT contributors
 *
 * TLS session core derived from openvela packages/ai_agent
 * src/infra/vela_tls.c (Apache-2.0) via contest cAGENT
 * packages/cAGENT src/runtime/runtime_openvela.c. The TCP/TLS flow keeps
 * the device-tested fixes of that line: non-blocking connect with a
 * select() timeout, /dev/random entropy seeding on NuttX, ALPN disabled,
 * SNI always sent, WANT_READ/WANT_WRITE poll loops with deadlines, and
 * small bounded writes for ESP-Hosted (C6) Wi-Fi forwarding.
 */
#include "tls_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#if defined(__NuttX__) && defined(CONFIG_DEV_RANDOM)
#include <sys/random.h>
#endif

#define AGENT_OV_TLS_TAG "agent_ov_tls"
#define AGENT_OV_TLS_LOG_MAX 160

#ifndef AGENT_OV_TLS_DEFAULT_IO_TIMEOUT_MS
#define AGENT_OV_TLS_DEFAULT_IO_TIMEOUT_MS 60000u
#endif

#ifndef AGENT_OV_TLS_DEFAULT_CHUNK_BYTES
#define AGENT_OV_TLS_DEFAULT_CHUNK_BYTES 1024u
#endif

/* mbedTLS 4 on ESP targets does not wire the generic entropy context to the
 * hardware RNG even though the chip exposes it as /dev/random. Seed the
 * CTR-DRBG directly from that trusted device instead of relying on the
 * mbedTLS platform fallback.
 */
#if defined(__NuttX__) && defined(CONFIG_DEV_RANDOM)
static int agent_ov_tls_entropy_from_random(void* context,
                                            unsigned char* output,
                                            size_t length)
{
    size_t offset = 0;

    (void)context;
    while (offset < length)
    {
        ssize_t ret = getrandom(output + offset, length - offset,
                                GRND_RANDOM);

        if (ret > 0)
        {
            offset += (size_t)ret;
            continue;
        }
        if (ret < 0 && errno == EINTR)
        {
            continue;
        }
        return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
    }
    return 0;
}
#endif

static void agent_ov_tls_log(const agent_openvela_tls_conn_t* conn,
                             agent_log_level_t level, const char* format, ...)
{
    char message[AGENT_OV_TLS_LOG_MAX];
    va_list args;
    int length;

    if (conn == NULL || conn->log == NULL)
    {
        return;
    }
    va_start(args, format);
    length = vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    if (length <= 0)
    {
        return;
    }
    conn->log(conn->log_context, level,
              (agent_string_view_t){ message, (size_t)length });
}

agent_openvela_tls_conn_t* agent_openvela_tls_conn_create(void)
{
    agent_openvela_tls_conn_t* conn =
        (agent_openvela_tls_conn_t*)calloc(1, sizeof(*conn));

    if (conn == NULL)
    {
        return NULL;
    }
    conn->net.fd = -1;
    conn->io_timeout_ms = AGENT_OV_TLS_DEFAULT_IO_TIMEOUT_MS;
    conn->write_chunk_bytes = AGENT_OV_TLS_DEFAULT_CHUNK_BYTES;
    return conn;
}

static uint64_t agent_ov_tls_monotonic_ms(void)
{
    struct timespec value;

    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
    {
        return 0u;
    }
    return (uint64_t)value.tv_sec * 1000u + (uint64_t)value.tv_nsec / 1000000u;
}

static uint32_t agent_ov_tls_remaining_ms(uint64_t deadline_ms)
{
    uint64_t now = agent_ov_tls_monotonic_ms();
    uint64_t remaining;

    if (now == 0u || deadline_ms == 0u || now >= deadline_ms)
    {
        return 0u;
    }
    remaining = deadline_ms - now;
    return remaining > UINT32_MAX ? UINT32_MAX : (uint32_t)remaining;
}

/* Wait until the connection fd reports the requested event; 0 on ready,
 * -1 with errno set on timeout or error.
 */
static int agent_ov_tls_wait_fd(const agent_openvela_tls_conn_t* conn,
                                short events, uint32_t timeout_ms)
{
    struct pollfd descriptor;
    int result;

    if (conn == NULL || conn->net.fd < 0)
    {
        errno = EINVAL;
        return -1;
    }
    descriptor.fd = conn->net.fd;
    descriptor.events = events;
    descriptor.revents = 0;
    result = poll(&descriptor, 1, (int)timeout_ms);
    if (result == 0)
    {
        errno = ETIMEDOUT;
        return -1;
    }
    if (result < 0)
    {
        return -1;
    }
    if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0 ||
        (descriptor.revents & events) == 0)
    {
        errno = EIO;
        return -1;
    }
    return 0;
}

static int agent_ov_tls_seed_rng(agent_openvela_tls_conn_t* conn)
{
    static const char personalization[] = "agent_openvela_tls";
    int ret;

#if defined(__NuttX__) && defined(CONFIG_DEV_RANDOM)
    ret = mbedtls_ctr_drbg_seed(&conn->ctr_drbg,
                                agent_ov_tls_entropy_from_random, NULL,
                                (const unsigned char*)personalization,
                                sizeof(personalization) - 1u);
#else
    ret = mbedtls_ctr_drbg_seed(&conn->ctr_drbg, mbedtls_entropy_func,
                                &conn->entropy,
                                (const unsigned char*)personalization,
                                sizeof(personalization) - 1u);
#endif
    if (ret != 0)
    {
        agent_ov_tls_log(conn, AGENT_LOG_WARNING,
                         "ctr_drbg_seed ret=-0x%04x", -ret);
        return -EIO;
    }
    return 0;
}

/* Non-blocking TCP connect under the deadline. mbedtls_net_connect() is not
 * used because on NuttX it blocks indefinitely when the remote port is
 * filtered (no SYN-ACK and no RST).
 */
static int agent_ov_tls_tcp_connect(agent_openvela_tls_conn_t* conn,
                                    const char* host, const char* port,
                                    uint64_t deadline_ms,
                                    uint32_t fallback_timeout_ms)
{
    struct addrinfo hints;
    struct addrinfo* resolved = NULL;
    struct timeval connect_timeout;
    uint32_t remaining_ms;
    int fd;
    int ret;
    int flags;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    ret = getaddrinfo(host, port, &hints, &resolved);
    if (ret != 0 || resolved == NULL)
    {
        agent_ov_tls_log(conn, AGENT_LOG_WARNING,
                         "dns failed host=%s port=%s rc=%d", host, port, ret);
        return -EIO;
    }
    if (resolved->ai_family != AF_INET || resolved->ai_addr == NULL)
    {
        freeaddrinfo(resolved);
        return -EIO;
    }

    fd = socket(resolved->ai_family, resolved->ai_socktype,
                resolved->ai_protocol);
    if (fd < 0)
    {
        freeaddrinfo(resolved);
        return -errno;
    }

    flags = fcntl(fd, F_GETFL, 0);
    remaining_ms = deadline_ms != 0u
        ? agent_ov_tls_remaining_ms(deadline_ms)
        : fallback_timeout_ms;
    connect_timeout.tv_sec = (time_t)(remaining_ms / 1000u);
    connect_timeout.tv_usec = (suseconds_t)(remaining_ms % 1000u) * 1000u;
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &connect_timeout,
                   sizeof(connect_timeout)) < 0)
    {
        ret = -errno;
        close(fd);
        freeaddrinfo(resolved);
        return ret;
    }

    ret = connect(fd, resolved->ai_addr, resolved->ai_addrlen);
    freeaddrinfo(resolved);
    if (ret < 0 && errno == EINPROGRESS)
    {
        fd_set write_set;
        int error = 0;
        socklen_t error_length = sizeof(error);

        FD_ZERO(&write_set);
        FD_SET(fd, &write_set);
        ret = select(fd + 1, NULL, &write_set, NULL, &connect_timeout);
        if (ret == 0)
        {
            close(fd);
            return -ETIMEDOUT;
        }
        if (ret < 0)
        {
            ret = -errno;
            close(fd);
            return ret;
        }
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_length) < 0 ||
            error != 0)
        {
            close(fd);
            return -EIO;
        }
    }
    else if (ret < 0)
    {
        ret = -errno;
        close(fd);
        return ret;
    }

    /* Stay non-blocking: mbedTLS reports WANT_READ/WANT_WRITE and the read
     * and write loops below drive poll() with the configured timeout. Do
     * not restore blocking mode, or a stalled peer blocks the caller.
     */
    conn->net.fd = fd;
    return 0;
}

int agent_openvela_tls_conn_connect(agent_openvela_tls_conn_t* conn,
                                    const char* host, const char* port,
                                    uint32_t timeout_ms)
{
    uint64_t deadline_ms;
    int ret;

    if (conn == NULL || host == NULL || port == NULL)
    {
        return -EINVAL;
    }

    mbedtls_ssl_init(&conn->ssl);
    mbedtls_ssl_config_init(&conn->cfg);
    mbedtls_net_init(&conn->net);
    mbedtls_ctr_drbg_init(&conn->ctr_drbg);
    mbedtls_entropy_init(&conn->entropy);
    mbedtls_x509_crt_init(&conn->ca);

    ret = agent_ov_tls_seed_rng(conn);
    if (ret != 0)
    {
        return ret;
    }

    deadline_ms = agent_ov_tls_monotonic_ms();
    if (deadline_ms == 0u)
    {
        return -EIO;
    }
    deadline_ms += timeout_ms;

    agent_ov_tls_log(conn, AGENT_LOG_DEBUG, "connecting %s:%s timeout=%ums",
                     host, port, timeout_ms);

    ret = agent_ov_tls_tcp_connect(conn, host, port, deadline_ms, timeout_ms);
    if (ret != 0)
    {
        return ret;
    }

    if ((ret = mbedtls_ssl_config_defaults(&conn->cfg,
                                           MBEDTLS_SSL_IS_CLIENT,
                                           MBEDTLS_SSL_TRANSPORT_STREAM,
                                           MBEDTLS_SSL_PRESET_DEFAULT)) != 0)
    {
        agent_ov_tls_log(conn, AGENT_LOG_WARNING,
                         "ssl_config_defaults ret=-0x%04x", -ret);
        return -EIO;
    }

    mbedtls_ssl_conf_min_tls_version(&conn->cfg, MBEDTLS_SSL_VERSION_TLS1_2);
#if defined(MBEDTLS_SSL_PROTO_TLS1_3)
    mbedtls_ssl_conf_max_tls_version(&conn->cfg, MBEDTLS_SSL_VERSION_TLS1_3);
#else
    mbedtls_ssl_conf_max_tls_version(&conn->cfg, MBEDTLS_SSL_VERSION_TLS1_2);
#endif

#if 0 /* Disabled: some servers reject the connection when ALPN is present. */
#if defined(MBEDTLS_SSL_ALPN)
    {
        static const char* alpn_protos[] = { "http/1.1", NULL };
        mbedtls_ssl_conf_alpn_protocols(&conn->cfg, alpn_protos);
    }
#endif
#endif

    if (conn->verify == AGENT_OPENVELA_TLS_VERIFY_REQUIRED)
    {
        /* x509_crt_parse wants the length including the terminating NUL. */
        ret = mbedtls_x509_crt_parse(&conn->ca,
                                     (const unsigned char*)conn->ca_pem_bytes,
                                     conn->ca_pem_length);
        if (ret != 0)
        {
            agent_ov_tls_log(conn, AGENT_LOG_WARNING,
                             "x509_crt_parse ret=-0x%04x", -ret);
            return -EIO;
        }
        mbedtls_ssl_conf_ca_chain(&conn->cfg, &conn->ca, NULL);
        mbedtls_ssl_conf_authmode(&conn->cfg, MBEDTLS_SSL_VERIFY_REQUIRED);
    }
    else if (conn->verify == AGENT_OPENVELA_TLS_VERIFY_NONE)
    {
        mbedtls_ssl_conf_authmode(&conn->cfg, MBEDTLS_SSL_VERIFY_NONE);
    }
    else
    {
        mbedtls_ssl_conf_authmode(&conn->cfg, MBEDTLS_SSL_VERIFY_OPTIONAL);
    }
    mbedtls_ssl_conf_rng(&conn->cfg, mbedtls_ctr_drbg_random, &conn->ctr_drbg);

    if ((ret = mbedtls_ssl_setup(&conn->ssl, &conn->cfg)) != 0)
    {
        agent_ov_tls_log(conn, AGENT_LOG_WARNING, "ssl_setup ret=-0x%04x",
                         -ret);
        return -EIO;
    }

    /* Always send SNI: CDN virtual hosting routes on it, and it stays
     * meaningful even while chain verification is relaxed.
     */
    if ((ret = mbedtls_ssl_set_hostname(&conn->ssl, host)) != 0)
    {
        agent_ov_tls_log(conn, AGENT_LOG_WARNING,
                         "ssl_set_hostname ret=-0x%04x", -ret);
        return -EIO;
    }

    mbedtls_ssl_set_bio(&conn->ssl, &conn->net,
                        mbedtls_net_send, mbedtls_net_recv, NULL);

    while ((ret = mbedtls_ssl_handshake(&conn->ssl)) != 0)
    {
        uint32_t remaining_ms;
        short events;

        if (ret != MBEDTLS_ERR_SSL_WANT_READ &&
            ret != MBEDTLS_ERR_SSL_WANT_WRITE)
        {
            agent_ov_tls_log(conn, AGENT_LOG_WARNING,
                             "ssl_handshake ret=-0x%04x", -ret);
            return -EIO;
        }
        events = ret == MBEDTLS_ERR_SSL_WANT_READ ? POLLIN : POLLOUT;
        remaining_ms = agent_ov_tls_remaining_ms(deadline_ms);
        if (remaining_ms == 0u ||
            agent_ov_tls_wait_fd(conn, events, remaining_ms) != 0)
        {
            return -ETIMEDOUT;
        }
    }

    agent_ov_tls_log(conn, AGENT_LOG_INFO, "tls handshake ok %s %s",
                     mbedtls_ssl_get_version(&conn->ssl),
                     mbedtls_ssl_get_ciphersuite(&conn->ssl));

    if (conn->verify == AGENT_OPENVELA_TLS_VERIFY_OPTIONAL)
    {
        int verify_result = (int)mbedtls_ssl_get_verify_result(&conn->ssl);

        if (verify_result != 0)
        {
            /* Recorded debt: with VERIFY_OPTIONAL the chain is inspected but
             * not enforced. Ship ca_pem with VERIFY_REQUIRED to close this.
             */
            agent_ov_tls_log(conn, AGENT_LOG_WARNING,
                             "chain verify result=0x%x ignored (OPTIONAL)",
                             verify_result);
        }
    }

    return 0;
}

ssize_t agent_openvela_tls_conn_read(agent_openvela_tls_conn_t* conn,
                                     void* buffer, size_t length)
{
    if (conn == NULL || buffer == NULL)
    {
        return -EINVAL;
    }
    for (;;)
    {
        int count = mbedtls_ssl_read(&conn->ssl, buffer, length);

        if (count == MBEDTLS_ERR_SSL_WANT_READ)
        {
            if (agent_ov_tls_wait_fd(conn, POLLIN, conn->io_timeout_ms) != 0)
            {
                return errno == ETIMEDOUT ? -ETIMEDOUT : -EIO;
            }
            continue;
        }
        if (count == MBEDTLS_ERR_SSL_WANT_WRITE)
        {
            if (agent_ov_tls_wait_fd(conn, POLLOUT, conn->io_timeout_ms) != 0)
            {
                return errno == ETIMEDOUT ? -ETIMEDOUT : -EIO;
            }
            continue;
        }
        if (count == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
        {
            return 0;
        }
        if (count < 0)
        {
            agent_ov_tls_log(conn, AGENT_LOG_WARNING,
                             "ssl_read ret=-0x%04x", -count);
            return -EIO;
        }
        return (ssize_t)count;
    }
}

static int agent_ov_tls_write_slice(agent_openvela_tls_conn_t* conn,
                                    const unsigned char* buffer, size_t length)
{
    size_t written = 0;

    while (written < length)
    {
        struct timeval timeout;
        uint32_t remaining_ms = conn->io_timeout_ms;
        int ret;

        /* Bound the underlying send as well as the subsequent poll; a
         * kernel-level stall must not lock the caller for good.
         */
        timeout.tv_sec = (time_t)(remaining_ms / 1000u);
        timeout.tv_usec = (suseconds_t)(remaining_ms % 1000u) * 1000u;
        if (setsockopt(conn->net.fd, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                       sizeof(timeout)) < 0)
        {
            return -errno;
        }

        ret = mbedtls_ssl_write(&conn->ssl, buffer + written,
                                length - written);
        if (ret > 0)
        {
            written += (size_t)ret;
            continue;
        }
        if (ret == MBEDTLS_ERR_SSL_WANT_READ ||
            ret == MBEDTLS_ERR_SSL_WANT_WRITE)
        {
            short events = ret == MBEDTLS_ERR_SSL_WANT_READ ? POLLIN : POLLOUT;

            if (agent_ov_tls_wait_fd(conn, events, remaining_ms) != 0)
            {
                return errno == ETIMEDOUT ? -ETIMEDOUT : -EIO;
            }
            continue;
        }
        agent_ov_tls_log(conn, AGENT_LOG_WARNING, "ssl_write ret=-0x%04x",
                         -ret);
        return -EIO;
    }
    return 0;
}

ssize_t agent_openvela_tls_conn_write(agent_openvela_tls_conn_t* conn,
                                      const void* buffer, size_t length)
{
    const unsigned char* cursor = (const unsigned char*)buffer;
    size_t remaining = length;

    if (conn == NULL || (buffer == NULL && length != 0u))
    {
        return -EINVAL;
    }
    /* ESP-Hosted (C6) Wi-Fi forwarding stalls on large single writes, so the
     * body is advanced in bounded slices with per-slice timeout control.
     */
    while (remaining != 0u)
    {
        size_t slice = remaining > conn->write_chunk_bytes
            ? conn->write_chunk_bytes
            : remaining;
        int ret = agent_ov_tls_write_slice(conn, cursor, slice);

        if (ret != 0)
        {
            return (ssize_t)ret;
        }
        cursor += slice;
        remaining -= slice;
    }
    return (ssize_t)length;
}

int agent_openvela_tls_conn_fd(const agent_openvela_tls_conn_t* conn)
{
    if (conn == NULL)
    {
        return -1;
    }
    return conn->net.fd;
}

void agent_openvela_tls_conn_close(agent_openvela_tls_conn_t* conn)
{
    if (conn == NULL)
    {
        return;
    }
    mbedtls_ssl_close_notify(&conn->ssl);
    mbedtls_net_free(&conn->net);
    mbedtls_ssl_free(&conn->ssl);
    mbedtls_ssl_config_free(&conn->cfg);
    mbedtls_x509_crt_free(&conn->ca);
    mbedtls_ctr_drbg_free(&conn->ctr_drbg);
    mbedtls_entropy_free(&conn->entropy);
    free(conn);
}
