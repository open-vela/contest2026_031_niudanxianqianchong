/* SPDX-License-Identifier: Apache-2.0 */

#include "smart_home_app_bridge.h"
#include "smart_home_gateway_api.h"
#include "../agent/smart_home_agent_run_service.h"
#include "../smart_home_cpu_debug.h"
#include "../smart_home_memory.h"

#include <cagent_addons/ws_frame.h>

#include "../config/cjson_compat.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <malloc.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/socket.h>
#include <unistd.h>

#define BRIDGE_HEADER_MAX 1024u
#define BRIDGE_BODY_MAX 768u
#define BRIDGE_REQUEST_MAX (BRIDGE_HEADER_MAX + BRIDGE_BODY_MAX + 1u)
#define BRIDGE_TOKEN_MAX 128u

#ifdef CONFIG_SMART_HOME_APP_BRIDGE_WORKER
static uint8_t g_bridge_worker_stack[CONFIG_SMART_HOME_APP_BRIDGE_STACKSIZE]
    SMART_HOME_SRAM_DATA;
#endif

static void bridge_mem_diag(const char *point)
{
    struct mallinfo info = mallinfo();

    fprintf(stderr,
            "[smart_home_mem] %s fordblks=%u mxordblk=%u uordblks=%u\n",
            point, info.fordblks, info.mxordblk, info.uordblks);
    smart_home_bulk_diag(point);
}

struct smart_home_app_bridge {
    smart_home_agent_app_t *app;
    int listen_fd;
    int websocket_fd;
    int stopping;
    pthread_t worker;
    int worker_started;
#if defined(CONFIG_SMART_HOME_APP_BRIDGE_WORKER) && \
    !defined(CONFIG_SMART_HOME_APP_BRIDGE_WORKER_ACCEPT)
    sem_t worker_wait;
    int worker_wait_ready;
#endif
    pthread_mutex_t mutex;
    int mutex_ready;
    char token[BRIDGE_TOKEN_MAX];
};

static int send_all(int fd, const void *data, size_t length)
{
    const char *cursor = data;
    while (length > 0u) {
        ssize_t written = send(fd, cursor, length, 0);
        if (written <= 0) return AGENT_ERROR_NETWORK;
        cursor += written;
        length -= (size_t)written;
    }
    return AGENT_OK;
}

static int load_token(char *token, size_t token_size)
{
    char json[1025];
    FILE *file;
    cJSON *root;
    cJSON *bridge;
    cJSON *value;
    size_t length;

    file = fopen(CONFIG_SMART_HOME_APP_BRIDGE_SECRETS_PATH, "rb");
    if (!file) return AGENT_ERROR_NOTFOUND;
    length = fread(json, 1u, sizeof(json) - 1u, file);
    fclose(file);
    if (length == 0u || length == sizeof(json) - 1u) return AGENT_ERROR_PARSE;
    json[length] = '\0';
    root = cJSON_Parse(json);
    bridge = root ? cJSON_GetObjectItemCaseSensitive(root, "app_bridge") : NULL;
    value = bridge ? cJSON_GetObjectItemCaseSensitive(bridge, "shared_token") : NULL;
    if (!cJSON_IsString(value) || !value->valuestring ||
        strlen(value->valuestring) >= token_size) {
        cJSON_Delete(root);
        return AGENT_ERROR_INVALID;
    }
    strcpy(token, value->valuestring);
    cJSON_Delete(root);
    return AGENT_OK;
}

#ifdef CONFIG_SMART_HOME_APP_BRIDGE_WORKER_ACCEPT
static int authorized(const char *request, const char *token)
{
    char expected[BRIDGE_TOKEN_MAX + 32];
    snprintf(expected, sizeof(expected), "Authorization: Bearer %s", token);
    return strstr(request, expected) != NULL;
}
#endif

static void bridge_event(const smart_home_device_event_t *event, void *user_data)
{
    smart_home_app_bridge_t *bridge = user_data;
    size_t payload_size = SMART_HOME_DEVICE_EVENT_DATA_SIZE + 160u;
    char *payload;
    uint8_t *frame;
    size_t written;
    int fd;
    int length;

    if (!bridge || !event) {
        return;
    }

    payload = smart_home_bulk_alloc(payload_size);
    frame = smart_home_bulk_alloc(payload_size + 16u);
    if (!payload || !frame) {
        smart_home_bulk_free(payload);
        smart_home_bulk_free(frame);
        return;
    }
    ov_mem_region_log("bridge-event-payload", payload);
    ov_mem_region_log("bridge-event-frame", frame);

    length = snprintf(payload, payload_size,
                      "{\"type\":\"%s\",\"eventId\":\"%s\",\"revision\":%lu,"
                      "\"occurredAt\":%llu,\"data\":%s}", event->type,
                      event->event_id, (unsigned long)event->revision,
                      (unsigned long long)event->occurred_at_ms,
                      event->data_json);
    if (length < 0 || (size_t)length >= payload_size) {
        smart_home_bulk_free(frame);
        smart_home_bulk_free(payload);
        return;
    }

    pthread_mutex_lock(&bridge->mutex);
    fd = bridge->websocket_fd;
    if (fd >= 0 && caddons_ws_write_frame(CADDONS_WS_SERVER, CADDONS_WS_TEXT,
                                           payload, (size_t)length, NULL, frame,
                                           payload_size + 16u, &written) == CADDONS_OK &&
        send_all(fd, frame, written) != AGENT_OK) {
        close(fd);
        bridge->websocket_fd = -1;
    }
    pthread_mutex_unlock(&bridge->mutex);
    smart_home_bulk_free(frame);
    smart_home_bulk_free(payload);
}

static void bridge_run_event(const char *type, const char *run_id,
                             const char *data_json, void *user_data)
{
    smart_home_app_bridge_t *bridge = user_data;
    size_t payload_size = 640u;
    char *payload;
    uint8_t *frame;
    size_t written;
    int length;

    if (!bridge || !type || !run_id || !data_json) {
        return;
    }

    payload = smart_home_bulk_alloc(payload_size);
    frame = smart_home_bulk_alloc(payload_size + 16u);
    if (!payload || !frame) {
        smart_home_bulk_free(payload);
        smart_home_bulk_free(frame);
        return;
    }
    ov_mem_region_log("bridge-run-payload", payload);
    ov_mem_region_log("bridge-run-frame", frame);

    length = snprintf(payload, payload_size,
                      "{\"type\":\"%s\",\"runId\":\"%s\",\"data\":%s}",
                      type, run_id, data_json);
    if (length < 0 || (size_t)length >= payload_size) {
        smart_home_bulk_free(frame);
        smart_home_bulk_free(payload);
        return;
    }

    pthread_mutex_lock(&bridge->mutex);
    if (bridge->websocket_fd >= 0 &&
        caddons_ws_write_frame(CADDONS_WS_SERVER, CADDONS_WS_TEXT, payload,
                               (size_t)length, NULL, frame, payload_size + 16u,
                               &written) == CADDONS_OK &&
        send_all(bridge->websocket_fd, frame, written) != AGENT_OK) {
        close(bridge->websocket_fd);
        bridge->websocket_fd = -1;
    }
    pthread_mutex_unlock(&bridge->mutex);
    smart_home_bulk_free(frame);
    smart_home_bulk_free(payload);
}

#ifdef CONFIG_SMART_HOME_APP_BRIDGE_WORKER_ACCEPT
static void send_http(int fd, int status, const char *body)
{
    char header[192];
    const char *reason = status == 200 ? "OK" : status == 202 ? "Accepted" :
                         status == 400 ? "Bad Request" : status == 401 ? "Unauthorized" :
                         status == 409 ? "Conflict" :
                         status == 404 ? "Not Found" :
                         status == 503 ? "Service Unavailable" : "Error";
    int length = (int)strlen(body);
    snprintf(header, sizeof(header),
             "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n"
             "Content-Length: %d\r\nConnection: close\r\n\r\n", status, reason, length);
    (void)send_all(fd, header, strlen(header));
    (void)send_all(fd, body, (size_t)length);
}

static ssize_t recv_request(int fd, char *request, size_t request_size)
{
    size_t used = 0u;
    size_t expected = 0u;
    char *body;

    while (used + 1u < request_size) {
        ssize_t received = recv(fd, request + used, request_size - used - 1u, 0);
        if (received <= 0) return received;
        used += (size_t)received;
        request[used] = '\0';
        body = strstr(request, "\r\n\r\n");
        if (body && expected == 0u) {
            const char *length = strstr(request, "Content-Length:");
            int parsed = 0;
            if (length) (void)sscanf(length, "Content-Length: %d", &parsed);
            if (parsed < 0 || (size_t)parsed > BRIDGE_BODY_MAX) return -1;
            expected = (size_t)(body + 4 - request) + (size_t)parsed;
        }
        if (body && used >= expected) return (ssize_t)used;
    }
    return -1;
}

static int handle_websocket(smart_home_app_bridge_t *bridge, int fd,
                            char **request, size_t request_size)
{
    char upgrade[512];
    char key[64];
    char *snapshot;
    uint8_t *frame;
    size_t upgrade_len;
    size_t frame_len;

    if (caddons_ws_parse_client_upgrade(*request, request_size, "/v1/events",
                                        key, sizeof(key)) != CADDONS_OK ||
        caddons_ws_build_server_upgrade(key, upgrade, sizeof(upgrade),
                                        &upgrade_len) != CADDONS_OK) {
        send_http(fd, 400, "{\"error\":{\"code\":\"invalid_websocket\"}}");
        return 0;
    }

    smart_home_bulk_free(*request);
    *request = NULL;
    snapshot = smart_home_bulk_alloc(SMART_HOME_GATEWAY_RESPONSE_SIZE);
    frame = smart_home_bulk_alloc(SMART_HOME_GATEWAY_RESPONSE_SIZE + 16u);
    if (!snapshot || !frame) {
        smart_home_bulk_free(frame);
        smart_home_bulk_free(snapshot);
        send_http(fd, 503, "{\"error\":{\"code\":\"resource_unavailable\"}}");
        return 0;
    }
    ov_mem_region_log("bridge-ws-snapshot", snapshot);
    ov_mem_region_log("bridge-ws-frame", frame);

    if (smart_home_device_service_build_snapshot_json(&bridge->app->device_service,
                                                       snapshot,
                                                       SMART_HOME_GATEWAY_RESPONSE_SIZE) != AGENT_OK ||
        caddons_ws_write_frame(CADDONS_WS_SERVER, CADDONS_WS_TEXT, snapshot,
                               strlen(snapshot), NULL, frame,
                               SMART_HOME_GATEWAY_RESPONSE_SIZE + 16u,
                               &frame_len) != CADDONS_OK ||
        send_all(fd, upgrade, upgrade_len) != AGENT_OK) {
        smart_home_bulk_free(frame);
        smart_home_bulk_free(snapshot);
        return 0;
    }

    pthread_mutex_lock(&bridge->mutex);
    if (bridge->websocket_fd >= 0) {
        close(bridge->websocket_fd);
    }
    bridge->websocket_fd = fd;
    if (send_all(fd, frame, frame_len) != AGENT_OK) {
        bridge->websocket_fd = -1;
        pthread_mutex_unlock(&bridge->mutex);
        smart_home_bulk_free(frame);
        smart_home_bulk_free(snapshot);
        return 0;
    }
    pthread_mutex_unlock(&bridge->mutex);
    smart_home_bulk_free(frame);
    smart_home_bulk_free(snapshot);
    return 1;
}

static void handle_client(smart_home_app_bridge_t *bridge, int fd)
{
    char *request = NULL;
    char method[8];
    char path[128];
    char *body;
    ssize_t received;
    smart_home_gateway_request_t api_request;
    smart_home_gateway_response_t *api_response = NULL;
    int agent_locked = 0;
    int keep_open = 0;

    request = smart_home_bulk_alloc(BRIDGE_REQUEST_MAX);
    if (!request) {
        send_http(fd, 503, "{\"error\":{\"code\":\"resource_unavailable\"}}");
        goto out;
    }
    ov_mem_region_log("bridge-http-request", request);

    received = recv_request(fd, request, BRIDGE_REQUEST_MAX);
    if (received <= 0) {
        send_http(fd, 400, "{\"error\":{\"code\":\"invalid_request\"}}");
        goto out;
    }
    request[received] = '\0';
    if (!authorized(request, bridge->token)) {
        send_http(fd, 401, "{\"error\":{\"code\":\"unauthorized\"}}");
        goto out;
    }
    if (pthread_mutex_trylock(&bridge->app->agent_mutex) != 0) {
        send_http(fd, 503, "{\"error\":{\"code\":\"agent_busy\"}}");
        goto out;
    }
    agent_locked = 1;
    if (strstr(request, "Upgrade: websocket") &&
        strncmp(request, "GET /v1/events ", 15) == 0) {
        keep_open = handle_websocket(bridge, fd, &request, (size_t)received);
        goto out;
    }
    if (sscanf(request, "%7s %127s", method, path) != 2) {
        send_http(fd, 400, "{\"error\":{\"code\":\"invalid_request\"}}");
        goto out;
    }
    body = strstr(request, "\r\n\r\n");
    api_request.method = method;
    api_request.path = path;
    api_request.body = body ? body + 4 : "";
    api_request.request_id = NULL;
    api_response = smart_home_bulk_alloc(sizeof(*api_response));
    if (!api_response) {
        send_http(fd, 503, "{\"error\":{\"code\":\"resource_unavailable\"}}");
        goto out;
    }
    ov_mem_region_log("bridge-api-response", api_response);
    if (smart_home_gateway_api_handle(bridge->app, &api_request, api_response) != AGENT_OK &&
        api_response->status_code == 0) {
        api_response->status_code = 500;
        strcpy(api_response->body, "{\"error\":{\"code\":\"internal\"}}");
    }
    send_http(fd, api_response->status_code, api_response->body);

out:
    smart_home_bulk_free(api_response);
    smart_home_bulk_free(request);
    if (agent_locked) {
        pthread_mutex_unlock(&bridge->app->agent_mutex);
    }
    if (!keep_open) {
        close(fd);
    }
}
#endif

#ifdef CONFIG_SMART_HOME_APP_BRIDGE_WORKER
static void *bridge_worker(void *argument)
{
    char stack_marker;
    smart_home_app_bridge_t *bridge = argument;

    smart_home_cpu_debug_log("bridge-worker-start");
    ov_mem_region_log("bridge-worker-stack", &stack_marker);

#ifdef CONFIG_SMART_HOME_APP_BRIDGE_WORKER_ACCEPT
    while (!bridge->stopping) {
        int fd = accept(bridge->listen_fd, NULL, NULL);
        if (fd >= 0) {
            smart_home_cpu_debug_log("bridge-request");
            handle_client(bridge, fd);
        }
        else if (errno != EINTR && !bridge->stopping) break;
    }
#else
    while (!bridge->stopping) {
        if (sem_wait(&bridge->worker_wait) < 0 && errno != EINTR) {
            break;
        }
    }
#endif
    return NULL;
}
#endif

int smart_home_app_bridge_start(smart_home_app_bridge_t **bridge_out,
                                smart_home_agent_app_t *app)
{
    smart_home_app_bridge_t *bridge;
    struct sockaddr_in address;
    int ret;
    if (!bridge_out || !app) return AGENT_ERROR_INVALID;
    bridge = calloc(1u, sizeof(*bridge));
    if (!bridge) return AGENT_ERROR_NOMEM;
    ov_mem_region_log("bridge-context", bridge);
    bridge->listen_fd = -1; bridge->websocket_fd = -1; bridge->app = app;
    ret = load_token(bridge->token, sizeof(bridge->token));
    if (ret != AGENT_OK || pthread_mutex_init(&bridge->mutex, NULL) != 0) goto fail;
    bridge->mutex_ready = 1;
    bridge->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (bridge->listen_fd < 0) { ret = AGENT_ERROR_NETWORK; goto fail; }
    memset(&address, 0, sizeof(address)); address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY); address.sin_port = htons(CONFIG_SMART_HOME_APP_BRIDGE_PORT);
    if (bind(bridge->listen_fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        listen(bridge->listen_fd, 2) < 0) { ret = AGENT_ERROR_NETWORK; goto fail; }
    smart_home_device_service_set_event_listener(&app->device_service, bridge_event, bridge);
    if (app->run_service) {
        smart_home_agent_run_service_set_listener(app->run_service,
                                                  bridge_run_event, bridge);
    }
#ifdef CONFIG_SMART_HOME_APP_BRIDGE_WORKER
    {
        pthread_attr_t attr;

#ifndef CONFIG_SMART_HOME_APP_BRIDGE_WORKER_ACCEPT
        if (sem_init(&bridge->worker_wait, 0, 0) != 0) {
            ret = AGENT_ERROR;
            goto fail;
        }
        bridge->worker_wait_ready = 1;
#endif
        bridge_mem_diag("bridge-worker-before-create");
        pthread_attr_init(&attr);
#ifdef __NuttX__
        if (pthread_attr_setstack(&attr, g_bridge_worker_stack,
                                  sizeof(g_bridge_worker_stack)) != 0) {
            pthread_attr_destroy(&attr);
            ret = AGENT_ERROR;
            goto fail;
        }
#endif
        if (pthread_create(&bridge->worker, &attr, bridge_worker, bridge) != 0) {
            pthread_attr_destroy(&attr);
            ret = AGENT_ERROR;
            goto fail;
        }
        pthread_attr_destroy(&attr);
        bridge->worker_started = 1;
        bridge_mem_diag("bridge-worker-after-create");
        smart_home_bulk_diag("bridge-worker-ready");
    }
#endif
    *bridge_out = bridge;
#ifndef CONFIG_SMART_HOME_APP_BRIDGE_WORKER
    fprintf(stderr,
            "[smart_home_bridge] listener initialized; HTTP worker disabled\n");
#elif !defined(CONFIG_SMART_HOME_APP_BRIDGE_WORKER_ACCEPT)
    fprintf(stderr,
            "[smart_home_bridge] worker initialized; accept loop disabled\n");
#endif
    return AGENT_OK;
fail:
    if (bridge->listen_fd >= 0) close(bridge->listen_fd);
#if defined(CONFIG_SMART_HOME_APP_BRIDGE_WORKER) && \
    !defined(CONFIG_SMART_HOME_APP_BRIDGE_WORKER_ACCEPT)
    if (bridge->worker_wait_ready) {
        sem_destroy(&bridge->worker_wait);
    }
#endif
    if (bridge->mutex_ready) pthread_mutex_destroy(&bridge->mutex);
    free(bridge); return ret;
}

void smart_home_app_bridge_stop(smart_home_app_bridge_t **bridge_ptr)
{
    smart_home_app_bridge_t *bridge;
    if (!bridge_ptr || !(bridge = *bridge_ptr)) return;
    bridge->stopping = 1;
    close(bridge->listen_fd);
    if (bridge->worker_started) {
#if defined(CONFIG_SMART_HOME_APP_BRIDGE_WORKER) && \
    !defined(CONFIG_SMART_HOME_APP_BRIDGE_WORKER_ACCEPT)
        sem_post(&bridge->worker_wait);
#endif
        pthread_join(bridge->worker, NULL);
    }
#if defined(CONFIG_SMART_HOME_APP_BRIDGE_WORKER) && \
    !defined(CONFIG_SMART_HOME_APP_BRIDGE_WORKER_ACCEPT)
    if (bridge->worker_wait_ready) {
        sem_destroy(&bridge->worker_wait);
    }
#endif
    smart_home_device_service_set_event_listener(&bridge->app->device_service, NULL, NULL);
    if (bridge->app->run_service) {
        smart_home_agent_run_service_set_listener(bridge->app->run_service,
                                                  NULL, NULL);
    }
    pthread_mutex_lock(&bridge->mutex); if (bridge->websocket_fd >= 0) close(bridge->websocket_fd); pthread_mutex_unlock(&bridge->mutex);
    pthread_mutex_destroy(&bridge->mutex); free(bridge); *bridge_ptr = NULL;
}
