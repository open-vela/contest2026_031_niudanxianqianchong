/* SPDX-License-Identifier: Apache-2.0 */
/* Direct standard MCP Streamable HTTP adapter for the smart_home MVP. */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "smart_home_mcp_bridge.h"
#include "../smart_home_memory.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>

#include "cagent_addons/cjson_compat.h"
#include "cagent_addons/errors.h"
#include "cagent_addons/limits.h"
#include "cagent_addons/mcp_bridge.h"
#include "cagent_addons/remote_tool.h"

/* 复用 cAGENT 的 mbedTLS TLS 连接封装（ov_tls_connect/read/write/free） */
#include <agent.h>
#include <agent_openvela_tls.h>
#include <netutils/webclient.h>

#ifndef CONFIG_SMART_HOME_MCP_BRIDGE_CONFIG_PATH
#ifdef CONFIG_SMART_HOME_MCP_PROBE_CONFIG_PATH
#define CONFIG_SMART_HOME_MCP_BRIDGE_CONFIG_PATH \
    CONFIG_SMART_HOME_MCP_PROBE_CONFIG_PATH
#else
#define CONFIG_SMART_HOME_MCP_BRIDGE_CONFIG_PATH "/data/smart_home/mcp_bridge.json"
#endif
#endif

#ifndef CONFIG_SMART_HOME_MCP_BRIDGE_SECRETS_PATH
#ifdef CONFIG_SMART_HOME_MCP_PROBE_SECRETS_PATH
#define CONFIG_SMART_HOME_MCP_BRIDGE_SECRETS_PATH \
    CONFIG_SMART_HOME_MCP_PROBE_SECRETS_PATH
#else
#define CONFIG_SMART_HOME_MCP_BRIDGE_SECRETS_PATH "/data/smart_home/secrets.json"
#endif
#endif

#ifndef CONFIG_SMART_HOME_MCP_BRIDGE_HTTP_HEADER_SIZE
#ifdef CONFIG_SMART_HOME_MCP_PROBE_HTTP_HEADER_SIZE
#define CONFIG_SMART_HOME_MCP_BRIDGE_HTTP_HEADER_SIZE \
    CONFIG_SMART_HOME_MCP_PROBE_HTTP_HEADER_SIZE
#else
#define CONFIG_SMART_HOME_MCP_BRIDGE_HTTP_HEADER_SIZE 2048
#endif
#endif

#ifndef CONFIG_SMART_HOME_MCP_BRIDGE_DISCOVERY_STACKSIZE
#ifdef CONFIG_SMART_HOME_MCP_PROBE_STACKSIZE
#define CONFIG_SMART_HOME_MCP_BRIDGE_DISCOVERY_STACKSIZE \
    CONFIG_SMART_HOME_MCP_PROBE_STACKSIZE
#else
#define CONFIG_SMART_HOME_MCP_BRIDGE_DISCOVERY_STACKSIZE 65536
#endif
#endif

/* One discovery operation shares this deadline across initialize,
 * notifications/initialized, and tools/list. The standalone probe overrides
 * it with its probe timeout setting. */
#ifndef CONFIG_SMART_HOME_MCP_BRIDGE_REQUEST_TIMEOUT_MS
#ifdef CONFIG_SMART_HOME_MCP_PROBE_TIMEOUT_MS
#define CONFIG_SMART_HOME_MCP_BRIDGE_REQUEST_TIMEOUT_MS \
    CONFIG_SMART_HOME_MCP_PROBE_TIMEOUT_MS
#else
#define CONFIG_SMART_HOME_MCP_BRIDGE_REQUEST_TIMEOUT_MS 30000u
#endif
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define SMART_HOME_MCP_SECRETS_VERSION 1
#define SMART_HOME_MCP_SECRETS_MAX_SIZE 4096u

#if defined(SMART_HOME_MCP_PROBE_DIAG) \
    || defined(CONFIG_SMART_HOME_MCP_PROBE) \
    || defined(CONFIG_SMART_HOME_DEMO_DEBUG_LOG)
static void probe_diag(const char *format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    vfprintf(stdout, format, arguments);
    va_end(arguments);
    fputc('\n', stdout);
    fflush(stdout);
}
#else
#define probe_diag(...) ((void)0)
#endif

/* append_format 定义在下方（header 拼装用），前向声明供 load_settings 使用 */
static int append_format(char *buffer, size_t buffer_size, size_t *used,
                         const char *format, ...);

typedef struct {
    char host[CAGENT_ADDONS_MCP_HOST_SIZE + 1u];
    char path[CAGENT_ADDONS_MCP_PATH_SIZE + 1u];
    char server_id[CAGENT_ADDONS_MCP_SERVER_ID_SIZE + 1u];
    char authorization[CAGENT_ADDONS_MCP_AUTHORIZATION_SIZE + 1u];
    char extra_headers[CAGENT_ADDONS_MCP_EXTRA_HEADERS_SIZE + 1u];
    uint16_t port;
    bool use_tls; /* true = https，false = 明文 http */
    caddons_mcp_tool_policy_t policies[CAGENT_ADDONS_MAX_MCP_TOOLS];
    char policy_names[CAGENT_ADDONS_MAX_MCP_TOOLS]
                     [CAGENT_ADDONS_COMMAND_NAME_SIZE + 1u];
    size_t policy_count;
} smart_home_mcp_settings_t;

struct smart_home_mcp_bridge {
    agent_t *agent;
    pthread_mutex_t *agent_mutex;
    caddons_remote_catalog_t *catalog;
    caddons_mcp_bridge_t *bridge;
    sem_t mutation_sem;
    pthread_mutex_t state_mutex;
    pthread_t mutation_worker;
    bool sem_ready;
    bool state_mutex_ready;
    bool worker_started;
    bool discover_requested;
    bool stopping;
    /* 以下两个字段由 state_mutex 保护 */
    smart_home_mcp_state_t state;
    int discovery_error; /* CADDONS_*；READY 时为 0 */
    char session_id[CAGENT_ADDONS_MCP_SESSION_ID_SIZE + 1u];
    char content_type[96];
    smart_home_mcp_settings_t settings;
};

static uint8_t
g_mcp_worker_stack[CONFIG_SMART_HOME_MCP_BRIDGE_DISCOVERY_STACKSIZE]
    SMART_HOME_SRAM_DATA;

static uint64_t monotonic_ms(void)
{
    struct timespec value;

    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
        return 0;
    }
    return (uint64_t)value.tv_sec * 1000u + (uint64_t)value.tv_nsec / 1000000u;
}

static void secure_clear(void *buffer, size_t size)
{
    volatile unsigned char *cursor = buffer;

    while (size-- > 0u) {
        *cursor++ = 0u;
    }
}

static int copy_string(char *destination, size_t capacity, const char *source)
{
    size_t length;

    if (!destination || !source || capacity < 2u) {
        return CADDONS_ERR_INVALID;
    }
    length = strlen(source);
    if (length == 0u || length >= capacity) {
        return CADDONS_ERR_LIMIT;
    }
    memcpy(destination, source, length + 1u);
    return CADDONS_OK;
}

static bool only_whitespace(const char *cursor, const char *end)
{
    while (cursor < end && isspace((unsigned char)*cursor)) {
        cursor++;
    }
    return cursor == end;
}

static bool contains_header_control(const char *value)
{
    const unsigned char *cursor = (const unsigned char *)value;

    while (cursor && *cursor) {
        if (*cursor == '\r' || *cursor == '\n') {
            return true;
        }
        cursor++;
    }
    return false;
}

static int read_file(const char *path, char *buffer, size_t buffer_size,
                     size_t *length_out)
{
    FILE *stream;
    size_t length;

    if (!path || !buffer || buffer_size < 2u || !length_out) {
        return CADDONS_ERR_INVALID;
    }
    stream = fopen(path, "rb");
    if (!stream) {
        return CADDONS_ERR_NOT_FOUND;
    }
    length = fread(buffer, 1u, buffer_size - 1u, stream);
    if (ferror(stream)) {
        fclose(stream);
        return CADDONS_ERR_INTERNAL;
    }
    if (!feof(stream)) {
        fclose(stream);
        return CADDONS_ERR_LIMIT;
    }
    fclose(stream);
    if (length == 0u) {
        return CADDONS_ERR_LIMIT;
    }
    buffer[length] = '\0';
    *length_out = length;
    return CADDONS_OK;
}

static caddons_tool_risk_t parse_risk(const char *risk)
{
    if (!risk) {
        return CADDONS_TOOL_RISK_UNKNOWN;
    }
    if (strcmp(risk, "read_only") == 0) {
        return CADDONS_TOOL_RISK_READ_ONLY;
    }
    if (strcmp(risk, "side_effect") == 0) {
        return CADDONS_TOOL_RISK_SIDE_EFFECT;
    }
    if (strcmp(risk, "dangerous") == 0) {
        return CADDONS_TOOL_RISK_DANGEROUS;
    }
    return CADDONS_TOOL_RISK_UNKNOWN;
}

static int load_settings(smart_home_mcp_settings_t *settings)
{
    char buffer[SMART_HOME_MCP_SECRETS_MAX_SIZE + 1u];
    cJSON *root = NULL;
    cJSON *bridge;
    cJSON *version;
    cJSON *tools;
    cJSON *tool;
    cJSON *secrets_root = NULL;
    const char *end = NULL;
    size_t length = 0u;
    size_t index = 0u;
    int rc;

    if (!settings) {
        return CADDONS_ERR_INVALID;
    }
    memset(settings, 0, sizeof(*settings));
    memset(buffer, 0, sizeof(buffer));

    /* ① 先读 secrets.json 的 mcp_bridge.header_values（敏感值表，用于 ${VAR} 替换） */
    rc = read_file(CONFIG_SMART_HOME_MCP_BRIDGE_SECRETS_PATH,
                   buffer, sizeof(buffer), &length);
    if (rc == CADDONS_OK) {
        const char *secrets_end = NULL;

        secrets_root = cJSON_ParseWithLengthOpts(buffer, length, &secrets_end, 0);
        if (!secrets_root || !cJSON_IsObject(secrets_root) || !secrets_end
            || !only_whitespace(secrets_end, buffer + length)) {
            cJSON_Delete(secrets_root);
            secrets_root = NULL;
        }
    }
    memset(buffer, 0, sizeof(buffer));

    /* ② 读 mcp_bridge.json（非敏感 server 配置） */
    rc = read_file(CONFIG_SMART_HOME_MCP_BRIDGE_CONFIG_PATH,
                   buffer, sizeof(buffer), &length);
    if (rc != CADDONS_OK) {
        goto out;
    }
    root = cJSON_ParseWithLengthOpts(buffer, length, &end, 0);
    if (!root || !cJSON_IsObject(root) || !end
        || !only_whitespace(end, buffer + length)) {
        rc = CADDONS_ERR_PARSE;
        goto out;
    }
    version = cJSON_GetObjectItemCaseSensitive(root, "version");
    bridge = cJSON_GetObjectItemCaseSensitive(root, "mcp_bridge");
    if (!cJSON_IsNumber(version) || version->valueint != SMART_HOME_MCP_SECRETS_VERSION
        || !cJSON_IsObject(bridge)) {
        rc = CADDONS_ERR_INVALID;
        goto out;
    }
    {
        cJSON *host = cJSON_GetObjectItemCaseSensitive(bridge, "host");
        cJSON *port = cJSON_GetObjectItemCaseSensitive(bridge, "port");
        cJSON *path = cJSON_GetObjectItemCaseSensitive(bridge, "path");
        cJSON *server_id = cJSON_GetObjectItemCaseSensitive(bridge, "server_id");
        cJSON *authorization = cJSON_GetObjectItemCaseSensitive(bridge, "authorization");
        cJSON *scheme = cJSON_GetObjectItemCaseSensitive(bridge, "scheme");
        const char *authorization_value;
        const char *scheme_value;

        if (!cJSON_IsString(host) || !cJSON_IsNumber(port)
            || !cJSON_IsString(path) || !cJSON_IsString(server_id)
            || port->valueint < 1 || port->valueint > 65535
            || copy_string(settings->host, sizeof(settings->host), host->valuestring) != CADDONS_OK
            || copy_string(settings->path, sizeof(settings->path), path->valuestring) != CADDONS_OK
            || copy_string(settings->server_id, sizeof(settings->server_id),
                           server_id->valuestring) != CADDONS_OK
            || settings->path[0] != '/' || contains_header_control(settings->host)
            || contains_header_control(settings->path)
            || contains_header_control(settings->server_id)) {
            rc = CADDONS_ERR_INVALID;
            goto out;
        }
        /* scheme: "https" → TLS，"http"（默认）→ 明文 */
        scheme_value = cJSON_GetStringValue(scheme);
        if (scheme && !scheme_value) {
            rc = CADDONS_ERR_INVALID;
            goto out;
        }
        if (scheme_value) {
            if (strcmp(scheme_value, "https") == 0) {
                settings->use_tls = true;
            } else if (strcmp(scheme_value, "http") != 0) {
                rc = CADDONS_ERR_INVALID;
                goto out;
            }
        }

        /* headers: 附加自定义 header（如 X-Caiyun-API-Key），拼成 "Name: value\r\n"。
         * 值若以 ${ 开头（如 ${CAIYUN_KEY}）→ 从 secrets 的
         * mcp_bridge.header_values.<名> 解析真实值。 */
        {
            cJSON *headers = cJSON_GetObjectItemCaseSensitive(bridge, "headers");
            cJSON *header_item;
            size_t used = 0u;

            if (headers && !cJSON_IsObject(headers)) {
                rc = CADDONS_ERR_INVALID;
                goto out;
            }
            if (headers) {
                cJSON_ArrayForEach(header_item, headers) {
                    const char *name = header_item->string;
                    const char *value;
                    char resolved[128];

                    if (!name || !name[0] || contains_header_control(name)) {
                        rc = CADDONS_ERR_INVALID;
                        goto out;
                    }
                    value = cJSON_GetStringValue(header_item);
                    if (!value || contains_header_control(value)) {
                        rc = CADDONS_ERR_INVALID;
                        goto out;
                    }
                    /* ${VAR} → secrets.mcp_bridge.header_values.VAR */
                    if (value[0] == '$' && value[1] == '{') {
                        const char *var_name = value + 2;
                        const char *var_end = strchr(var_name, '}');
                        cJSON *header_values = NULL;
                        cJSON *resolved_item = NULL;

                        if (!var_end || var_end == var_name) {
                            rc = CADDONS_ERR_INVALID;
                            goto out;
                        }
                        if (secrets_root) {
                            cJSON *secrets_bridge = cJSON_GetObjectItemCaseSensitive(
                                secrets_root, "mcp_bridge");
                            if (secrets_bridge) {
                                header_values = cJSON_GetObjectItemCaseSensitive(
                                    secrets_bridge, "header_values");
                            }
                        }
                        if (header_values) {
                            char var_key[96];
                            size_t var_len = (size_t)(var_end - var_name);

                            if (var_len >= sizeof(var_key)) {
                                rc = CADDONS_ERR_LIMIT;
                                goto out;
                            }
                            memcpy(var_key, var_name, var_len);
                            var_key[var_len] = '\0';
                            resolved_item = cJSON_GetObjectItemCaseSensitive(
                                header_values, var_key);
                        }
                        if (!resolved_item || !cJSON_IsString(resolved_item)
                            || !resolved_item->valuestring[0]
                            || contains_header_control(resolved_item->valuestring)
                            || strlen(resolved_item->valuestring) >= sizeof(resolved)) {
                            rc = CADDONS_ERR_INVALID;
                            goto out;
                        }
                        strcpy(resolved, resolved_item->valuestring);
                        value = resolved;
                    }
                    if (append_format(settings->extra_headers,
                                      sizeof(settings->extra_headers), &used,
                                      "%s: %s\r\n", name, value) != CADDONS_OK) {
                        rc = CADDONS_ERR_LIMIT;
                        goto out;
                    }
                }
            }
        }
        authorization_value = cJSON_GetStringValue(authorization);
        if (authorization && (!authorization_value
            || contains_header_control(authorization_value)
            || (authorization_value[0]
                && copy_string(settings->authorization,
                               sizeof(settings->authorization),
                               authorization_value) != CADDONS_OK))) {
            rc = CADDONS_ERR_INVALID;
            goto out;
        }
        settings->port = (uint16_t)port->valueint;
    }
    tools = cJSON_GetObjectItemCaseSensitive(bridge, "tools");
    if (!cJSON_IsArray(tools) || cJSON_GetArraySize(tools) <= 0
        || (size_t)cJSON_GetArraySize(tools) > CAGENT_ADDONS_MAX_MCP_TOOLS) {
        rc = CADDONS_ERR_LIMIT;
        goto out;
    }
    cJSON_ArrayForEach(tool, tools) {
        cJSON *name = cJSON_GetObjectItemCaseSensitive(tool, "name");
        cJSON *risk = cJSON_GetObjectItemCaseSensitive(tool, "risk");
        cJSON *timeout_ms = cJSON_GetObjectItemCaseSensitive(tool, "timeout_ms");
        const char *risk_value;
        caddons_tool_risk_t parsed_risk;

        if (!cJSON_IsObject(tool) || !cJSON_IsString(name) || !cJSON_IsString(risk)
            || !cJSON_IsNumber(timeout_ms) || timeout_ms->valueint < 100
            || timeout_ms->valueint > 60000 || index >= CAGENT_ADDONS_MAX_MCP_TOOLS) {
            rc = CADDONS_ERR_INVALID;
            goto out;
        }
        risk_value = cJSON_GetStringValue(risk);
        parsed_risk = parse_risk(risk_value);
        if (parsed_risk == CADDONS_TOOL_RISK_UNKNOWN
            || copy_string(settings->policy_names[index],
                           sizeof(settings->policy_names[index]),
                           name->valuestring) != CADDONS_OK) {
            rc = CADDONS_ERR_INVALID;
            goto out;
        }
        settings->policies[index].name = settings->policy_names[index];
        settings->policies[index].risk = parsed_risk;
        settings->policies[index].timeout_ms = (uint32_t)timeout_ms->valueint;
        index++;
    }
    settings->policy_count = index;
    rc = CADDONS_OK;

out:
    cJSON_Delete(root);
    cJSON_Delete(secrets_root);
    secure_clear(buffer, sizeof(buffer));
    if (rc != CADDONS_OK) {
        secure_clear(settings, sizeof(*settings));
    }
    return rc;
}

static int wait_fd(int fd, short events, uint64_t deadline_ms)
{
    struct pollfd descriptor;

    for (;;) {
        uint64_t now = monotonic_ms();
        uint64_t remaining;
        int timeout;
        int rc;

        if (deadline_ms && now >= deadline_ms) {
            return CADDONS_ERR_TIMEOUT;
        }
        remaining = deadline_ms ? deadline_ms - now : (uint64_t)INT_MAX;
        timeout = remaining > (uint64_t)INT_MAX ? INT_MAX : (int)remaining;
        descriptor.fd = fd;
        descriptor.events = events;
        descriptor.revents = 0;
        rc = poll(&descriptor, 1, timeout);
        if (rc > 0) {
            return (descriptor.revents & events) ? CADDONS_OK : CADDONS_ERR_NETWORK;
        }
        if (rc == 0) {
            return CADDONS_ERR_TIMEOUT;
        }
        if (errno != EINTR) {
            return CADDONS_ERR_NETWORK;
        }
    }
}

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);

    return flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0
        ? CADDONS_ERR_NETWORK : CADDONS_OK;
}

/* ── 连接封装（明文 fd 或 TLS ctx） ── */

/* connect_socket 定义在下方，前向声明 */
static int connect_socket(const char *host, uint16_t port, uint64_t deadline_ms);

typedef struct {
    int fd;                                /* 明文 socket；TLS 时无 fd */
    struct webclient_tls_connection *tls;  /* NULL = 明文 */
} smh_conn_t;

/* cAGENT V2 不再导出 ov_tls_*，改用它的 webclient TLS ops 适配层
 * （agent_port_openvela_tls_*）。ops 表是静态的，状态由 ctx 承载。
 */
static agent_port_openvela_tls_t g_mcp_tls;
static bool g_mcp_tls_ready;

static int smh_tls_ensure(void)
{
    agent_port_openvela_tls_config_t config;

    if (g_mcp_tls_ready) {
        return CADDONS_OK;
    }
    memset(&config, 0, sizeof(config));
    /* 与 V1 行为一致：校验证书链但只记录结果（AGENT_OPENVELA_TLS_VERIFY_OPTIONAL）。 */
    config.verify = AGENT_OPENVELA_TLS_VERIFY_OPTIONAL;
    config.io_timeout_ms = 30000u;
    config.write_chunk_bytes = 1024u;
    if (agent_port_openvela_tls_init(&g_mcp_tls, &config) != AGENT_OK) {
        return CADDONS_ERR_INTERNAL;
    }
    g_mcp_tls_ready = true;
    return CADDONS_OK;
}

/* 把绝对 deadline 转为相对超时（毫秒），0 表示未设 deadline */
static uint32_t smh_timeout_ms(uint64_t deadline_ms)
{
    uint64_t now = monotonic_ms();

    if (!deadline_ms) {
        return 30000u;
    }
    if (deadline_ms <= now) {
        return 100u;
    }
    if (deadline_ms - now > UINT32_MAX) {
        return UINT32_MAX;
    }
    return (uint32_t)(deadline_ms - now);
}

/* 建立连接：is_tls 走 cAGENT ov_tls_connect，否则走 connect_socket */
static int smh_conn_open(smh_conn_t *conn,
                         const caddons_http_request_t *request,
                         uint64_t deadline_ms)
{
    char port_text[8];
    int rc;

    memset(conn, 0, sizeof(*conn));
    conn->fd = -1;

    if (deadline_ms && monotonic_ms() >= deadline_ms) {
        return CADDONS_ERR_TIMEOUT;
    }

    if (!request->is_tls) {
        conn->fd = connect_socket(request->host, request->port, deadline_ms);
        return conn->fd >= 0 ? CADDONS_OK : CADDONS_ERR_NETWORK;
    }

    rc = smh_tls_ensure();
    if (rc != CADDONS_OK) {
        return rc;
    }
    snprintf(port_text, sizeof(port_text), "%u", (unsigned int)request->port);
    {
        uint32_t timeout = smh_timeout_ms(deadline_ms);
        unsigned int timeout_sec = (unsigned int)((timeout + 999u) / 1000u);

        rc = g_mcp_tls.ops->connect(&g_mcp_tls, request->host, port_text,
                                    timeout_sec ? timeout_sec : 1u,
                                    &conn->tls);
    }
    if (rc != 0 || !conn->tls) {
        conn->tls = NULL;
        return rc == -ETIMEDOUT ? CADDONS_ERR_TIMEOUT : CADDONS_ERR_NETWORK;
    }
    conn->fd = -1;
    return CADDONS_OK;
}

/* 单次读：TLS 走 cAGENT ov_tls_read，明文走 recv */
static ssize_t smh_conn_read_once(smh_conn_t *conn, void *buffer, size_t length,
                                  uint64_t deadline_ms)
{
    (void)deadline_ms;

    if (conn->tls) {
        return g_mcp_tls.ops->recv(&g_mcp_tls, conn->tls, buffer, length);
    }
    return recv(conn->fd, buffer, length, 0);
}

/* 单次写：TLS 走 cAGENT ov_tls_write，明文走 send */
static ssize_t smh_conn_write_once(smh_conn_t *conn, const void *buffer,
                                   size_t length, uint64_t deadline_ms)
{
    (void)deadline_ms;

    if (conn->tls) {
        return g_mcp_tls.ops->send(&g_mcp_tls, conn->tls, buffer, length);
    }
    return send(conn->fd, buffer, length, MSG_NOSIGNAL);
}

static void smh_conn_close(smh_conn_t *conn)
{
    if (conn->tls) {
        if (g_mcp_tls_ready && g_mcp_tls.ops) {
            g_mcp_tls.ops->close(&g_mcp_tls, conn->tls);
        }
        conn->tls = NULL;
        /* TLS ops 拥有并关闭底层 socket，不要再关一次描述符。 */
        conn->fd = -1;
    }
    if (conn->fd >= 0) {
        close(conn->fd);
        conn->fd = -1;
    }
}

static int connect_socket(const char *host, uint16_t port, uint64_t deadline_ms)
{
    struct addrinfo hints;
    struct addrinfo *addresses = NULL;
    struct addrinfo *address;
    char port_text[8];
    int fd = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(port_text, sizeof(port_text), "%u", (unsigned int)port);
    if (getaddrinfo(host, port_text, &hints, &addresses) != 0 || !addresses) {
        return -1;
    }
    for (address = addresses; address; address = address->ai_next) {
        int error = 0;
        socklen_t error_size = sizeof(error);

        fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd < 0 || set_nonblocking(fd) != CADDONS_OK) {
            if (fd >= 0) {
                close(fd);
            }
            fd = -1;
            continue;
        }
        if (connect(fd, address->ai_addr, address->ai_addrlen) == 0) {
            break;
        }
        if (errno != EINPROGRESS || wait_fd(fd, POLLOUT, deadline_ms) != CADDONS_OK
            || getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_size) < 0 || error != 0) {
            close(fd);
            fd = -1;
            continue;
        }
        break;
    }
    freeaddrinfo(addresses);
    return fd;
}

static int send_all(smh_conn_t *conn, const void *payload, size_t length,
                    uint64_t deadline_ms)
{
    const unsigned char *bytes = payload;
    size_t offset = 0u;

    while (offset < length) {
        ssize_t count = smh_conn_write_once(conn, bytes + offset,
                                            length - offset, deadline_ms);

        if (count > 0) {
            offset += (size_t)count;
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            int rc = wait_fd(conn->fd, POLLOUT, deadline_ms);

            if (rc != CADDONS_OK) {
                return rc;
            }
        } else {
            if (errno == ETIMEDOUT) {
                return CADDONS_ERR_TIMEOUT;
            }
            return CADDONS_ERR_NETWORK;
        }
    }
    return CADDONS_OK;
}

static int receive_exact(smh_conn_t *conn, char *output, size_t length,
                         uint64_t deadline_ms)
{
    size_t offset = 0u;

    while (offset < length) {
        ssize_t count = smh_conn_read_once(conn, output + offset,
                                           length - offset, deadline_ms);

        if (count > 0) {
            offset += (size_t)count;
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            int rc = wait_fd(conn->fd, POLLIN, deadline_ms);

            if (rc != CADDONS_OK) {
                return rc;
            }
        } else {
            if (errno == ETIMEDOUT) {
                return CADDONS_ERR_TIMEOUT;
            }
            return CADDONS_ERR_NETWORK;
        }
    }
    return CADDONS_OK;
}

static int read_chunk_line(smh_conn_t *conn, char *line, size_t line_size,
                           size_t *line_length, uint64_t deadline_ms)
{
    size_t used = 0u;

    if (!line || line_size < 3u || !line_length) {
        return CADDONS_ERR_INVALID;
    }
    while (used + 1u < line_size) {
        int rc = receive_exact(conn, line + used, 1u, deadline_ms);

        if (rc != CADDONS_OK) {
            return rc;
        }
        used++;
        if (used >= 2u && line[used - 1u] == '\n') {
            if (line[used - 2u] != '\r') {
                return CADDONS_ERR_PROTOCOL;
            }
            line[used - 2u] = '\0';
            *line_length = used - 2u;
            return CADDONS_OK;
        }
    }
    return CADDONS_ERR_LIMIT;
}

static int parse_chunk_size(const char *line, size_t line_length,
                            size_t *chunk_size)
{
    size_t index = 0u;
    size_t value = 0u;
    bool saw_digit = false;

    if (!line || !chunk_size) {
        return CADDONS_ERR_INVALID;
    }
    while (index < line_length
           && (line[index] == ' ' || line[index] == '\t')) {
        index++;
    }
    while (index < line_length) {
        unsigned int digit;
        char current = line[index];

        if (current >= '0' && current <= '9') {
            digit = (unsigned int)(current - '0');
        } else if (current >= 'a' && current <= 'f') {
            digit = (unsigned int)(current - 'a' + 10);
        } else if (current >= 'A' && current <= 'F') {
            digit = (unsigned int)(current - 'A' + 10);
        } else {
            break;
        }
        if (value > (SIZE_MAX - digit) / 16u) {
            return CADDONS_ERR_LIMIT;
        }
        value = value * 16u + digit;
        saw_digit = true;
        index++;
    }
    if (!saw_digit) {
        return CADDONS_ERR_PROTOCOL;
    }
    if (index < line_length && line[index] != ';') {
        while (index < line_length
               && (line[index] == ' ' || line[index] == '\t')) {
            index++;
        }
        if (index < line_length && line[index] != ';') {
            return CADDONS_ERR_PROTOCOL;
        }
    }
    *chunk_size = value;
    return CADDONS_OK;
}

static int receive_chunked(smh_conn_t *conn, char *output, size_t output_size,
                           size_t *length_out, uint64_t deadline_ms)
{
    char line[256];
    char trailer[2];
    size_t used = 0u;
    size_t line_length;
    size_t chunk_size;
    int rc;

    if (!output || output_size < 2u || !length_out) {
        return CADDONS_ERR_INVALID;
    }
    for (;;) {
        rc = read_chunk_line(conn, line, sizeof(line), &line_length,
                             deadline_ms);
        if (rc != CADDONS_OK) {
            return rc;
        }
        rc = parse_chunk_size(line, line_length, &chunk_size);
        if (rc != CADDONS_OK) {
            return rc;
        }
        if (chunk_size == 0u) {
            do {
                rc = read_chunk_line(conn, line, sizeof(line), &line_length,
                                     deadline_ms);
                if (rc != CADDONS_OK) {
                    return rc;
                }
            } while (line_length != 0u);
            *length_out = used;
            return CADDONS_OK;
        }
        if (chunk_size > output_size - 1u - used) {
            return CADDONS_ERR_LIMIT;
        }
        rc = receive_exact(conn, output + used, chunk_size, deadline_ms);
        if (rc != CADDONS_OK) {
            return rc;
        }
        used += chunk_size;
        rc = receive_exact(conn, trailer, sizeof(trailer), deadline_ms);
        if (rc != CADDONS_OK) {
            return rc;
        }
        if (trailer[0] != '\r' || trailer[1] != '\n') {
            return CADDONS_ERR_PROTOCOL;
        }
    }
}

static int read_headers(smh_conn_t *conn, char *output, size_t output_size,
                        uint64_t deadline_ms)
{
    size_t used = 0u;

    if (!output || output_size < 5u) {
        return CADDONS_ERR_INVALID;
    }
    while (used + 1u < output_size) {
        ssize_t count = smh_conn_read_once(conn, output + used, 1u,
                                           deadline_ms);

        if (count == 1) {
            used++;
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            int rc = wait_fd(conn->fd, POLLIN, deadline_ms);

            if (rc != CADDONS_OK) {
                return rc;
            }
        } else {
            if (errno == ETIMEDOUT) {
                return CADDONS_ERR_TIMEOUT;
            }
            return CADDONS_ERR_NETWORK;
        }
        if (used >= 4u && memcmp(output + used - 4u, "\r\n\r\n", 4u) == 0) {
            output[used] = '\0';
            return CADDONS_OK;
        }
    }
    return CADDONS_ERR_LIMIT;
}

static bool header_name_equal(const char *value, size_t length, const char *name)
{
    size_t index;

    if (strlen(name) != length) {
        return false;
    }
    for (index = 0u; index < length; index++) {
        if (tolower((unsigned char)value[index]) != tolower((unsigned char)name[index])) {
            return false;
        }
    }
    return true;
}

static const char *header_value(const char *headers, const char *name,
                                char *output, size_t output_size)
{
    const char *line = strstr(headers, "\r\n");

    if (!line || !output || output_size == 0u) {
        return NULL;
    }
    line += 2;
    while (*line != '\0' && !(line[0] == '\r' && line[1] == '\n')) {
        const char *end = strstr(line, "\r\n");
        const char *colon;
        const char *value;
        size_t value_length;

        if (!end) {
            return NULL;
        }
        colon = memchr(line, ':', (size_t)(end - line));
        if (colon && header_name_equal(line, (size_t)(colon - line), name)) {
            value = colon + 1;
            while (value < end && (*value == ' ' || *value == '\t')) {
                value++;
            }
            value_length = (size_t)(end - value);
            while (value_length > 0u && (value[value_length - 1u] == ' '
                                          || value[value_length - 1u] == '\t')) {
                value_length--;
            }
            if (value_length == 0u || value_length >= output_size) {
                return NULL;
            }
            memcpy(output, value, value_length);
            output[value_length] = '\0';
            return output;
        }
        line = end + 2;
    }
    return NULL;
}

static int append_format(char *buffer, size_t buffer_size, size_t *used,
                         const char *format, ...)
{
    va_list arguments;
    int written;

    if (!buffer || !used || *used >= buffer_size) {
        return CADDONS_ERR_LIMIT;
    }
    va_start(arguments, format);
    written = vsnprintf(buffer + *used, buffer_size - *used, format, arguments);
    va_end(arguments);
    if (written < 0 || (size_t)written >= buffer_size - *used) {
        return CADDONS_ERR_LIMIT;
    }
    *used += (size_t)written;
    return CADDONS_OK;
}

static int mcp_http_request(void *context, const caddons_http_request_t *request,
                            caddons_http_response_t *response)
{
    smart_home_mcp_bridge_t *bridge = context;
    char headers[CONFIG_SMART_HOME_MCP_BRIDGE_HTTP_HEADER_SIZE + 1u];
    char content_length[24];
    char transfer_encoding[64];
    char port_text[8];
    char *end = NULL;
    size_t used = 0u;
    unsigned long length = 0u;
    bool chunked = false;
    smh_conn_t conn;
    int rc = CADDONS_ERR_NETWORK;

    if (!bridge || !request || !response || !request->method || !request->host
        || !request->path || !request->request_body || !response->response_body
        || response->response_size < 2u || !request->deadline_ms
        || contains_header_control(request->host) || contains_header_control(request->path)
        || (request->authorization && contains_header_control(request->authorization))
        || (request->mcp_session_id && contains_header_control(request->mcp_session_id))) {
        return CADDONS_ERR_INVALID;
    }
    probe_diag("HTTP begin method=%s host=%s path=%s body_bytes=%u",
               request->method, request->host, request->path,
               (unsigned int)strlen(request->request_body));
    snprintf(port_text, sizeof(port_text), "%u", (unsigned int)request->port);
    if (append_format(headers, sizeof(headers), &used, "%s %s HTTP/1.1\r\n",
                      request->method, request->path) != CADDONS_OK
        || append_format(headers, sizeof(headers), &used, "Host: %s:%s\r\n",
                         request->host, port_text) != CADDONS_OK
        || append_format(headers, sizeof(headers), &used,
                         "Content-Type: application/json\r\n"
                         "Accept: application/json, text/event-stream\r\n"
                         "Connection: close\r\n") != CADDONS_OK
        || append_format(headers, sizeof(headers), &used, "Content-Length: %zu\r\n",
                         strlen(request->request_body)) != CADDONS_OK
        || (request->authorization && append_format(headers, sizeof(headers), &used,
                                                     "Authorization: %s\r\n",
                                                     request->authorization) != CADDONS_OK)
        || (request->extra_headers && append_format(headers, sizeof(headers), &used,
                                                     "%s",
                                                     request->extra_headers) != CADDONS_OK)
        || (request->mcp_protocol_version && append_format(headers, sizeof(headers), &used,
                                                            "MCP-Protocol-Version: %s\r\n",
                                                            request->mcp_protocol_version) != CADDONS_OK)
        || (request->mcp_session_id && append_format(headers, sizeof(headers), &used,
                                                      "Mcp-Session-Id: %s\r\n",
                                                      request->mcp_session_id) != CADDONS_OK)
        || append_format(headers, sizeof(headers), &used, "\r\n") != CADDONS_OK) {
        return CADDONS_ERR_LIMIT;
    }
    rc = smh_conn_open(&conn, request, request->deadline_ms);
    if (rc != CADDONS_OK) {
        return rc;
    }
    rc = send_all(&conn, headers, used, request->deadline_ms);
    if (rc == CADDONS_OK) {
        probe_diag("HTTP headers sent bytes=%u", (unsigned int)used);
    }
    if (rc == CADDONS_OK) {
        rc = send_all(&conn, request->request_body, strlen(request->request_body),
                      request->deadline_ms);
        if (rc == CADDONS_OK) {
            probe_diag("HTTP body sent");
        }
    }
    if (rc == CADDONS_OK) {
        rc = read_headers(&conn, headers, sizeof(headers), request->deadline_ms);
        if (rc == CADDONS_OK) {
            probe_diag("HTTP response headers received");
        }
    }
    if (rc != CADDONS_OK) {
        goto out;
    }
    {
        const char *space = strchr(headers, ' ');
        long status;

        if (strncmp(headers, "HTTP/", 5u) != 0 || !space) {
            rc = CADDONS_ERR_PROTOCOL;
            goto out;
        }
        errno = 0;
        status = strtol(space + 1, &end, 10);
        if (errno || end == space + 1 || status < 100 || status > 599) {
            rc = CADDONS_ERR_PROTOCOL;
            goto out;
        }
        response->http_status = (int)status;
        probe_diag("HTTP status=%d", response->http_status);
    }
    bridge->content_type[0] = '\0';
    bridge->session_id[0] = '\0';
    (void)header_value(headers, "Content-Type", bridge->content_type,
                       sizeof(bridge->content_type));
    (void)header_value(headers, "Mcp-Session-Id", bridge->session_id,
                       sizeof(bridge->session_id));
    response->content_type = bridge->content_type[0] ? bridge->content_type : NULL;
    response->mcp_session_id = bridge->session_id[0] ? bridge->session_id : NULL;
    chunked = header_value(headers, "Transfer-Encoding", transfer_encoding,
                           sizeof(transfer_encoding))
        && strstr(transfer_encoding, "chunked") != NULL;
    if (chunked) {
        probe_diag("HTTP response uses chunked transfer");
        rc = receive_chunked(&conn, response->response_body,
                             response->response_size, &response->response_len,
                             request->deadline_ms);
        if (rc == CADDONS_OK) {
            probe_diag("HTTP chunked body received bytes=%u",
                       (unsigned int)response->response_len);
        }
        goto out;
    }
    if (!header_value(headers, "Content-Length", content_length,
                     sizeof(content_length))) {
        response->response_len = 0u;
        probe_diag("HTTP response has no Content-Length");
        rc = CADDONS_OK;
        goto out;
    }
    errno = 0;
    length = strtoul(content_length, &end, 10);
    if (errno || !end || *end || length >= response->response_size) {
        rc = CADDONS_ERR_LIMIT;
        goto out;
    }
    rc = receive_exact(&conn, response->response_body, (size_t)length,
                       request->deadline_ms);
    if (rc == CADDONS_OK) {
        response->response_len = (size_t)length;
        probe_diag("HTTP response body received bytes=%u",
                   (unsigned int)response->response_len);
    }

out:
    probe_diag("HTTP complete rc=%d", rc);
    smh_conn_close(&conn);
    return rc;
}

/* catalog 变更通知：MCP 工具发现/移除后由 addons bridge 调用，
 * 唤醒 mutation worker 消费队列。 */
static void catalog_changed(void *user_data)
{
    smart_home_mcp_bridge_t *bridge = user_data;

    if (bridge && bridge->sem_ready) {
        sem_post(&bridge->mutation_sem);
    }
}

static int apply_pending_mutations(smart_home_mcp_bridge_t *bridge,
                                   size_t *registered_count,
                                   size_t *unregistered_count)
{
    caddons_remote_mutation_t mutation;
    int rc;

    for (;;) {
        rc = caddons_remote_catalog_next_mutation(bridge->catalog, &mutation);
        if (rc == CADDONS_ERR_NOT_FOUND) {
            return rc;
        }
        if (rc != CADDONS_OK) {
            return rc;
        }
        pthread_mutex_lock(bridge->agent_mutex);
        rc = caddons_remote_catalog_apply_mutation(bridge->catalog, bridge->agent,
                                                    &mutation);
        pthread_mutex_unlock(bridge->agent_mutex);
        if (rc != CADDONS_OK) {
            syslog(LOG_ERR, "smart_home: MCP catalog mutation failed: %d\n", rc);
            return rc;
        }
        if (mutation.type == CADDONS_MUTATION_REGISTER) {
            if (registered_count) {
                (*registered_count)++;
            }
        } else if (unregistered_count) {
            (*unregistered_count)++;
        }
        probe_diag("MCP tool %s: %s",
                   mutation.type == CADDONS_MUTATION_REGISTER
                       ? "registered" : "unregistered",
                   mutation.public_name);
        syslog(LOG_INFO, "smart_home: MCP tool %s: %s\n",
               mutation.type == CADDONS_MUTATION_REGISTER ? "registered" : "unregistered",
               mutation.public_name);
    }
}

static bool is_stopping(smart_home_mcp_bridge_t *bridge)
{
    bool stopping;

    pthread_mutex_lock(&bridge->state_mutex);
    stopping = bridge->stopping;
    pthread_mutex_unlock(&bridge->state_mutex);
    return stopping;
}

static void *mutation_worker(void *argument)
{
    char stack_marker = 0;
    smart_home_mcp_bridge_t *bridge = argument;

    ov_mem_region_log("mcp-worker-stack", &stack_marker);

    for (;;) {
        int rc;
        int discovery_rc = CADDONS_OK;
        size_t registered_count = 0u;
        size_t unregistered_count = 0u;
        bool discover = false;

        do {
            rc = sem_wait(&bridge->mutation_sem);
        } while (rc < 0 && errno == EINTR);
        if (rc < 0) {
            return NULL;
        }

        pthread_mutex_lock(&bridge->state_mutex);
        if (!bridge->stopping && bridge->discover_requested) {
            bridge->discover_requested = false;
            discover = true;
        }
        pthread_mutex_unlock(&bridge->state_mutex);

        /* The operation worker owns the synchronous network exchange. It is
         * deliberately separate from LVGL and uses the configured large
         * stack, but discovery only runs after an explicit request. */
        if (discover) {
            probe_diag("MCP discovery worker start");
            /* A manual retry starts a fresh Streamable HTTP session.  This
             * also queues removal of tools from a previous successful
             * discovery before the new catalog is imported. */
            (void)caddons_mcp_bridge_stop(bridge->bridge);
            probe_diag("calling cagent MCP initialize/tools/list");
            discovery_rc = caddons_mcp_bridge_start(bridge->bridge);

            if (discovery_rc == CADDONS_OK) {
                syslog(LOG_INFO, "smart_home: MCP discovery succeeded\n");
            } else {
                syslog(LOG_ERR, "smart_home: MCP discovery failed: %d\n",
                       discovery_rc);
            }

            pthread_mutex_lock(&bridge->state_mutex);
            bridge->discovery_error = discovery_rc == CADDONS_OK
                ? 0 : discovery_rc;
            pthread_mutex_unlock(&bridge->state_mutex);
            probe_diag("MCP discovery worker returned rc=%d", discovery_rc);
        }

        rc = apply_pending_mutations(bridge,
                                     discover ? &registered_count : NULL,
                                     discover ? &unregistered_count : NULL);
        if (discover) {
            if (rc == CADDONS_ERR_NOT_FOUND) {
                probe_diag("MCP catalog drain complete: registered=%u unregistered=%u",
                           (unsigned int)registered_count,
                           (unsigned int)unregistered_count);
            } else {
                probe_diag("MCP catalog drain failed rc=%d: registered=%u unregistered=%u",
                           rc, (unsigned int)registered_count,
                           (unsigned int)unregistered_count);
            }
        }
        if (rc != CADDONS_OK && rc != CADDONS_ERR_NOT_FOUND) {
            pthread_mutex_lock(&bridge->state_mutex);
            if (discover) {
                bridge->discovery_error = rc;
                bridge->state = SMART_HOME_MCP_STATE_FAILED;
            }
            pthread_mutex_unlock(&bridge->state_mutex);
            continue;
        }
        if (discover) {
            pthread_mutex_lock(&bridge->state_mutex);
            if (discovery_rc == CADDONS_OK && bridge->discovery_error == 0) {
                bridge->state = SMART_HOME_MCP_STATE_READY;
            } else {
                bridge->state = SMART_HOME_MCP_STATE_FAILED;
            }
            pthread_mutex_unlock(&bridge->state_mutex);
        }
        if (is_stopping(bridge) && rc == CADDONS_ERR_NOT_FOUND) {
            return NULL;
        }
    }
}

static int start_worker(smart_home_mcp_bridge_t *bridge)
{
    pthread_attr_t attributes;
    int rc;

    if (sem_init(&bridge->mutation_sem, 0, 0) != 0) {
        return CADDONS_ERR_INTERNAL;
    }
    bridge->sem_ready = true;
    if (pthread_mutex_init(&bridge->state_mutex, NULL) != 0) {
        return CADDONS_ERR_INTERNAL;
    }
    bridge->state_mutex_ready = true;
    if (pthread_attr_init(&attributes) != 0) {
        return CADDONS_ERR_INTERNAL;
    }
#ifdef __NuttX__
    rc = pthread_attr_setstack(&attributes, g_mcp_worker_stack,
                               sizeof(g_mcp_worker_stack));
    if (rc != 0) {
        pthread_attr_destroy(&attributes);
        return CADDONS_ERR_INVALID;
    }
#endif
    rc = pthread_create(&bridge->mutation_worker, &attributes, mutation_worker, bridge);
    pthread_attr_destroy(&attributes);
    if (rc != 0) {
        return CADDONS_ERR_INTERNAL;
    }
    bridge->worker_started = true;
    return CADDONS_OK;
}

static void destroy_bridge(smart_home_mcp_bridge_t *bridge)
{
    if (!bridge) {
        return;
    }
    secure_clear(&bridge->settings, sizeof(bridge->settings));
    /* Mark the worker stopping before waking it. caddons_mcp_bridge_stop()
     * may wait for an in-flight HTTP callback to finish its deadline. */
    if (bridge->state_mutex_ready) {
        pthread_mutex_lock(&bridge->state_mutex);
        bridge->stopping = true;
        bridge->discover_requested = false;
        pthread_mutex_unlock(&bridge->state_mutex);
    }
    if (bridge->bridge) {
        (void)caddons_mcp_bridge_stop(bridge->bridge);
    }
    if (bridge->sem_ready) {
        sem_post(&bridge->mutation_sem);
    }
    if (bridge->worker_started) {
        pthread_join(bridge->mutation_worker, NULL);
    }
    if (bridge->bridge) {
        caddons_mcp_bridge_destroy(bridge->bridge);
    }
    if (bridge->catalog) {
        caddons_remote_catalog_destroy(bridge->catalog);
    }
    if (bridge->state_mutex_ready) {
        pthread_mutex_destroy(&bridge->state_mutex);
    }
    if (bridge->sem_ready) {
        sem_destroy(&bridge->mutation_sem);
    }
    secure_clear(bridge, sizeof(*bridge));
    free(bridge);
}

int smart_home_mcp_bridge_start(smart_home_mcp_bridge_t **bridge_out,
                                agent_t *agent, pthread_mutex_t *agent_mutex)
{
    smart_home_mcp_settings_t settings;
    caddons_mcp_bridge_config_t config;
    smart_home_mcp_bridge_t *bridge;
    int rc;

    if (!bridge_out || *bridge_out || !agent || !agent_mutex) {
        return AGENT_ERROR_INVALID;
    }
    rc = load_settings(&settings);
    if (rc != CADDONS_OK) {
        return caddons_error_to_agent(rc);
    }
    bridge = calloc(1u, sizeof(*bridge));
    if (!bridge) {
        secure_clear(&settings, sizeof(settings));
        return AGENT_ERROR_NOMEM;
    }
    ov_mem_region_log("mcp-bridge-context", bridge);
    bridge->agent = agent;
    bridge->agent_mutex = agent_mutex;
    bridge->catalog = caddons_remote_catalog_create();
    if (!bridge->catalog) {
        rc = CADDONS_ERR_NOMEM;
        goto fail;
    }
    rc = start_worker(bridge);
    if (rc != CADDONS_OK) {
        goto fail;
    }
    memset(&config, 0, sizeof(config));
    config.host = settings.host;
    config.port = settings.port;
    config.path = settings.path;
    config.use_tls = settings.use_tls;
    config.server_id = settings.server_id;
    config.authorization = settings.authorization[0] ? settings.authorization : NULL;
    config.extra_headers = settings.extra_headers[0] ? settings.extra_headers : NULL;
    config.tool_policies = settings.policies;
    config.tool_policy_count = settings.policy_count;
    config.catalog = bridge->catalog;
    config.http_request = mcp_http_request;
    config.http_context = bridge;
    config.catalog_changed_fn = catalog_changed;
    config.catalog_changed_user_data = bridge;
    config.request_timeout_ms = CONFIG_SMART_HOME_MCP_BRIDGE_REQUEST_TIMEOUT_MS;
    bridge->bridge = caddons_mcp_bridge_create(&config);
    if (!bridge->bridge) {
        rc = CADDONS_ERR_INVALID;
        goto fail;
    }
    pthread_mutex_lock(&bridge->state_mutex);
    bridge->state = SMART_HOME_MCP_STATE_IDLE;
    bridge->discovery_error = 0;
    pthread_mutex_unlock(&bridge->state_mutex);
    bridge->settings = settings;
    *bridge_out = bridge;
    syslog(LOG_INFO, "smart_home: MCP bridge ready; discovery is manual: %s:%u%s\n",
           config.host, (unsigned int)config.port, config.path);
    secure_clear(&settings, sizeof(settings));
    return AGENT_OK;

fail:
    secure_clear(&settings, sizeof(settings));
    destroy_bridge(bridge);
    return caddons_error_to_agent(rc);
}

int smart_home_mcp_bridge_reconfigure(
    smart_home_mcp_bridge_t *bridge,
    const smart_home_mcp_runtime_config_t *config)
{
    smart_home_mcp_settings_t settings;
    caddons_mcp_bridge_config_t bridge_config;
    caddons_mcp_bridge_t *replacement;

    if (!bridge || !config || !bridge->bridge || !bridge->catalog ||
        !bridge->state_mutex_ready || !bridge->agent_mutex) {
        return AGENT_ERROR_INVALID;
    }
    if (config->port == 0u || config->host[0] == '\0' ||
        config->path[0] != '/' || config->server_id[0] == '\0' ||
        contains_header_control(config->host) ||
        contains_header_control(config->path) ||
        contains_header_control(config->server_id) ||
        contains_header_control(config->authorization)) {
        return AGENT_ERROR_INVALID;
    }

    pthread_mutex_lock(bridge->agent_mutex);
    pthread_mutex_lock(&bridge->state_mutex);
    if (bridge->stopping ||
        bridge->state == SMART_HOME_MCP_STATE_CONNECTING ||
        bridge->discover_requested) {
        pthread_mutex_unlock(&bridge->state_mutex);
        pthread_mutex_unlock(bridge->agent_mutex);
        return AGENT_ERROR_BUSY;
    }
    settings = bridge->settings;
    for (size_t i = 0; i < settings.policy_count; i++)
        settings.policies[i].name = settings.policy_names[i];
    if (copy_string(settings.host, sizeof(settings.host), config->host) != CADDONS_OK ||
        copy_string(settings.path, sizeof(settings.path), config->path) != CADDONS_OK ||
        copy_string(settings.server_id, sizeof(settings.server_id), config->server_id) != CADDONS_OK ||
        (config->authorization[0] && copy_string(settings.authorization, sizeof(settings.authorization), config->authorization) != CADDONS_OK)) {
        pthread_mutex_unlock(&bridge->state_mutex);
        pthread_mutex_unlock(bridge->agent_mutex);
        secure_clear(&settings, sizeof(settings));
        return AGENT_ERROR_LIMIT;
    }
    if (!config->authorization[0]) settings.authorization[0] = '\0';
    settings.port = config->port;
    settings.use_tls = config->use_tls;

    memset(&bridge_config, 0, sizeof(bridge_config));
    bridge_config.host = settings.host;
    bridge_config.port = settings.port;
    bridge_config.path = settings.path;
    bridge_config.use_tls = settings.use_tls;
    bridge_config.server_id = settings.server_id;
    bridge_config.authorization = settings.authorization[0] ?
                                  settings.authorization : NULL;
    bridge_config.extra_headers = settings.extra_headers[0] ? settings.extra_headers : NULL;
    bridge_config.tool_policies = settings.policies;
    bridge_config.tool_policy_count = settings.policy_count;
    bridge_config.catalog = bridge->catalog;
    bridge_config.http_request = mcp_http_request;
    bridge_config.http_context = bridge;
    bridge_config.catalog_changed_fn = catalog_changed;
    bridge_config.catalog_changed_user_data = bridge;
    bridge_config.request_timeout_ms =
        CONFIG_SMART_HOME_MCP_BRIDGE_REQUEST_TIMEOUT_MS;
    replacement = caddons_mcp_bridge_create(&bridge_config);
    if (replacement) {
        caddons_mcp_bridge_destroy(bridge->bridge);
        bridge->bridge = replacement;
        bridge->settings = settings;
        for (size_t i = 0; i < settings.policy_count; i++)
            bridge->settings.policies[i].name = bridge->settings.policy_names[i];
        bridge->state = SMART_HOME_MCP_STATE_IDLE;
        bridge->discovery_error = 0;
    }
    pthread_mutex_unlock(&bridge->state_mutex);
    pthread_mutex_unlock(bridge->agent_mutex);
    secure_clear(&settings, sizeof(settings));
    return replacement ? AGENT_OK : AGENT_ERROR_NOMEM;
}

int smart_home_mcp_bridge_get_config(smart_home_mcp_bridge_t *bridge,
                                      smart_home_mcp_runtime_config_t *config)
{
    if (!bridge || !config || !bridge->state_mutex_ready) return AGENT_ERROR_INVALID;
    if (pthread_mutex_trylock(&bridge->state_mutex) != 0) return AGENT_ERROR_BUSY;
    memset(config, 0, sizeof(*config));
    snprintf(config->host, sizeof(config->host), "%s", bridge->settings.host);
    snprintf(config->path, sizeof(config->path), "%s", bridge->settings.path);
    snprintf(config->server_id, sizeof(config->server_id), "%s", bridge->settings.server_id);
    snprintf(config->authorization, sizeof(config->authorization), "%s", bridge->settings.authorization);
    config->port = bridge->settings.port;
    config->use_tls = bridge->settings.use_tls;
    pthread_mutex_unlock(&bridge->state_mutex);
    return AGENT_OK;
}

int smart_home_mcp_bridge_request_discover(smart_home_mcp_bridge_t *bridge)
{
    if (!bridge || !bridge->bridge || !bridge->state_mutex_ready || !bridge->sem_ready) {
        return AGENT_ERROR_INVALID;
    }

    pthread_mutex_lock(&bridge->state_mutex);
    if (bridge->stopping) {
        pthread_mutex_unlock(&bridge->state_mutex);
        return AGENT_ERROR_BUSY;
    }
    if (bridge->state == SMART_HOME_MCP_STATE_CONNECTING
        || bridge->discover_requested) {
        pthread_mutex_unlock(&bridge->state_mutex);
        return AGENT_ERROR_BUSY;
    }
    bridge->discover_requested = true;
    bridge->state = SMART_HOME_MCP_STATE_CONNECTING;
    bridge->discovery_error = 0;
    pthread_mutex_unlock(&bridge->state_mutex);
    sem_post(&bridge->mutation_sem);
    syslog(LOG_INFO, "smart_home: MCP discovery requested\n");
    return AGENT_OK;
}

int smart_home_mcp_bridge_get_state(smart_home_mcp_bridge_t *bridge,
                                    smart_home_mcp_state_t *state,
                                    int *last_error)
{
    if (!bridge || !state) {
        return AGENT_ERROR_INVALID;
    }

    pthread_mutex_lock(&bridge->state_mutex);
    *state = bridge->state;
    if (last_error) {
        *last_error = bridge->discovery_error;
    }
    pthread_mutex_unlock(&bridge->state_mutex);
    return AGENT_OK;
}

int smart_home_mcp_bridge_wait_ready(smart_home_mcp_bridge_t *bridge,
                                     uint32_t timeout_ms)
{
    uint64_t deadline;
    smart_home_mcp_state_t state;
    int last_error = 0;

    if (!bridge) {
        return AGENT_ERROR_INVALID;
    }

    deadline = monotonic_ms() + timeout_ms;
    for (;;) {
        (void)smart_home_mcp_bridge_get_state(bridge, &state, &last_error);
        if (state == SMART_HOME_MCP_STATE_READY) {
            return AGENT_OK;
        }
        if (state == SMART_HOME_MCP_STATE_FAILED) {
            return caddons_error_to_agent(last_error);
        }
        if (monotonic_ms() >= deadline) {
            return AGENT_ERROR_TIMEOUT;
        }
        usleep(20000);
    }
}

void smart_home_mcp_bridge_stop(smart_home_mcp_bridge_t **bridge_ptr)
{
    if (!bridge_ptr || !*bridge_ptr) {
        return;
    }
    destroy_bridge(*bridge_ptr);
    *bridge_ptr = NULL;
}
