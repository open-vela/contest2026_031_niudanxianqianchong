/* SPDX-License-Identifier: Apache-2.0 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "smart_home_node_gateway.h"
#include "../smart_home_memory.h"

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <semaphore.h>
#include <stdint.h>
#include <syslog.h>

#include "cagent_addons/cjson_compat.h"
#include "cagent_addons/errors.h"
#include "cagent_addons/limits.h"
#include "cagent_addons/node_gateway.h"
#include "cagent_addons/remote_tool.h"
#include "cagent_addons/ws_transport.h"
#include <agent.h>

#define SMART_HOME_NODE_SECRETS_VERSION 1
#define SMART_HOME_NODE_SECRETS_MAX_SIZE 2048u
#define SMART_HOME_NODE_STACK_ALIGNMENT 16u

struct smart_home_node_gateway {
    agent_t *agent;
    pthread_mutex_t *agent_mutex;
    caddons_transport_t *transport;
    caddons_remote_catalog_t *catalog;
    caddons_node_gateway_t *gateway;
    sem_t mutation_sem;
    pthread_mutex_t state_mutex;
    pthread_t mutation_worker;
    caddons_thread_stack_t mutation_stack;
    bool sem_ready;
    bool state_mutex_ready;
    bool worker_started;
    bool stopping;
    uint32_t presentation_revision;
    uint16_t listen_port;
};

static int node_stack_alloc(size_t stack_size, caddons_thread_stack_t *stack,
                            void *user_data)
{
    uintptr_t address;
    void *allocation;

    (void)user_data;
    if (!stack || stack_size > SIZE_MAX - (SMART_HOME_NODE_STACK_ALIGNMENT - 1u)) {
        return CADDONS_ERR_INVALID;
    }

    memset(stack, 0, sizeof(*stack));
    allocation = smart_home_bulk_alloc(
        stack_size + SMART_HOME_NODE_STACK_ALIGNMENT - 1u);
    if (!allocation) {
        return CADDONS_ERR_NOMEM;
    }

    address = (uintptr_t)allocation;
    address = (address + SMART_HOME_NODE_STACK_ALIGNMENT - 1u)
              & ~(uintptr_t)(SMART_HOME_NODE_STACK_ALIGNMENT - 1u);
    stack->allocation = allocation;
    stack->stack = (void *)address;
    stack->stack_size = stack_size;
    ov_mem_region_log("node-psram-stack", stack->stack);
    return CADDONS_OK;
}

static void node_stack_free(caddons_thread_stack_t *stack, void *user_data)
{
    (void)user_data;
    if (!stack) {
        return;
    }

    if (stack->allocation) {
        smart_home_bulk_free(stack->allocation);
    }
    memset(stack, 0, sizeof(*stack));
}

static void secure_clear(void *buffer, size_t size)
{
    volatile unsigned char *cursor = buffer;

    while (size-- > 0u) {
        *cursor++ = 0u;
    }
}

static bool only_whitespace(const char *cursor, const char *end)
{
    while (cursor < end && isspace((unsigned char)*cursor)) {
        cursor++;
    }
    return cursor == end;
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

    length = fread(buffer, 1u, buffer_size, stream);
    if (ferror(stream)) {
        fclose(stream);
        return CADDONS_ERR_INTERNAL;
    }
    if (!feof(stream)) {
        fclose(stream);
        return CADDONS_ERR_LIMIT;
    }
    fclose(stream);

    if (length == 0u || length >= buffer_size) {
        return CADDONS_ERR_LIMIT;
    }

    buffer[length] = '\0';
    *length_out = length;
    return CADDONS_OK;
}

static int load_shared_token(char *token, size_t token_size)
{
    char buffer[SMART_HOME_NODE_SECRETS_MAX_SIZE + 1u];
    cJSON *root = NULL;
    cJSON *gateway;
    cJSON *version;
    cJSON *shared_token;
    const char *end = NULL;
    const char *value;
    size_t length = 0u;
    int rc;

    if (!token || token_size < 2u) {
        return CADDONS_ERR_INVALID;
    }
    token[0] = '\0';
    memset(buffer, 0, sizeof(buffer));

    rc = read_file(CONFIG_SMART_HOME_NODE_GATEWAY_SECRETS_PATH,
                   buffer, sizeof(buffer), &length);
    if (rc != CADDONS_OK) {
        goto out;
    }

    root = cJSON_ParseWithLengthOpts(buffer, length, &end, 0);
    if (!root || !cJSON_IsObject(root)
        || !end || !only_whitespace(end, buffer + length)) {
        rc = CADDONS_ERR_PARSE;
        goto out;
    }

    version = cJSON_GetObjectItemCaseSensitive(root, "version");
    gateway = cJSON_GetObjectItemCaseSensitive(root, "node_gateway");
    shared_token = gateway
        ? cJSON_GetObjectItemCaseSensitive(gateway, "shared_token") : NULL;
    value = cJSON_GetStringValue(shared_token);
    if (!cJSON_IsNumber(version)
        || version->valuedouble != SMART_HOME_NODE_SECRETS_VERSION
        || !cJSON_IsObject(gateway) || !value || !value[0]) {
        rc = CADDONS_ERR_INVALID;
        goto out;
    }
    if (strlen(value) >= token_size) {
        rc = CADDONS_ERR_LIMIT;
        goto out;
    }

    strcpy(token, value);
    rc = CADDONS_OK;

out:
    cJSON_Delete(root);
    secure_clear(buffer, sizeof(buffer));
    if (rc != CADDONS_OK) {
        secure_clear(token, token_size);
    }
    return rc;
}

static void catalog_changed(void *user_data)
{
    smart_home_node_gateway_t *gateway = user_data;

    if (!gateway) {
        return;
    }

    if (gateway->state_mutex_ready) {
        pthread_mutex_lock(&gateway->state_mutex);
        gateway->presentation_revision++;
        pthread_mutex_unlock(&gateway->state_mutex);
    }
    if (gateway->sem_ready) {
        sem_post(&gateway->mutation_sem);
    }
}

static int apply_pending_mutations(smart_home_node_gateway_t *gateway)
{
    caddons_remote_mutation_t mutation;
    int rc;

    for (;;) {
        rc = caddons_remote_catalog_next_mutation(gateway->catalog, &mutation);
        if (rc == CADDONS_ERR_NOT_FOUND) {
            return rc;
        }
        if (rc != CADDONS_OK) {
            return rc;
        }

        pthread_mutex_lock(gateway->agent_mutex);
        rc = caddons_remote_catalog_apply_mutation(gateway->catalog,
                                                    gateway->agent,
                                                    &mutation);
        pthread_mutex_unlock(gateway->agent_mutex);
        if (rc != CADDONS_OK) {
            syslog(LOG_ERR, "smart_home: Node catalog mutation failed: %d\n", rc);
            return rc;
        }
        syslog(LOG_INFO, "smart_home: Node tool %s: %s\n",
               mutation.type == CADDONS_MUTATION_REGISTER
                   ? "registered" : "unregistered",
               mutation.public_name);
    }
}

static bool is_stopping(smart_home_node_gateway_t *gateway)
{
    bool stopping;

    pthread_mutex_lock(&gateway->state_mutex);
    stopping = gateway->stopping;
    pthread_mutex_unlock(&gateway->state_mutex);
    return stopping;
}

static void *mutation_worker(void *argument)
{
    char stack_marker = 0;
    smart_home_node_gateway_t *gateway = argument;

    ov_mem_region_log("node-gateway-worker-stack", &stack_marker);

    for (;;) {
        int rc;

        do {
            rc = sem_wait(&gateway->mutation_sem);
        } while (rc < 0 && errno == EINTR);
        if (rc < 0) {
            syslog(LOG_ERR, "smart_home: Node catalog worker semaphore failed\n");
            return NULL;
        }

        rc = apply_pending_mutations(gateway);
        if (rc != CADDONS_OK && rc != CADDONS_ERR_NOT_FOUND) {
            continue;
        }
        if (is_stopping(gateway) && rc == CADDONS_ERR_NOT_FOUND) {
            return NULL;
        }
    }
}

static int start_worker(smart_home_node_gateway_t *gateway)
{
    pthread_attr_t attributes;
    int rc;

    if (sem_init(&gateway->mutation_sem, 0, 0) != 0) {
        return CADDONS_ERR_INTERNAL;
    }
    gateway->sem_ready = true;
    if (pthread_mutex_init(&gateway->state_mutex, NULL) != 0) {
        return CADDONS_ERR_INTERNAL;
    }
    gateway->state_mutex_ready = true;

    if (pthread_attr_init(&attributes) != 0) {
        return CADDONS_ERR_INTERNAL;
    }
    rc = node_stack_alloc(CONFIG_SMART_HOME_NODE_GATEWAY_WORKER_STACKSIZE,
                          &gateway->mutation_stack, gateway);
    if (rc != 0) {
        pthread_attr_destroy(&attributes);
        return rc;
    }
    rc = pthread_attr_setstack(&attributes, gateway->mutation_stack.stack,
                               gateway->mutation_stack.stack_size);
    if (rc != 0) {
        pthread_attr_destroy(&attributes);
        node_stack_free(&gateway->mutation_stack, gateway);
        return CADDONS_ERR_INVALID;
    }
    rc = pthread_create(&gateway->mutation_worker, &attributes,
                        mutation_worker, gateway);
    pthread_attr_destroy(&attributes);
    if (rc != 0) {
        node_stack_free(&gateway->mutation_stack, gateway);
        return CADDONS_ERR_INTERNAL;
    }
    gateway->worker_started = true;
    return CADDONS_OK;
}

static void destroy_gateway(smart_home_node_gateway_t *gateway)
{
    if (!gateway) {
        return;
    }

    if (gateway->gateway) {
        caddons_node_gateway_stop(gateway->gateway);
    }
    if (gateway->state_mutex_ready) {
        pthread_mutex_lock(&gateway->state_mutex);
        gateway->stopping = true;
        pthread_mutex_unlock(&gateway->state_mutex);
    }
    if (gateway->sem_ready) {
        sem_post(&gateway->mutation_sem);
    }
    if (gateway->worker_started) {
        pthread_join(gateway->mutation_worker, NULL);
    }
    node_stack_free(&gateway->mutation_stack, gateway);
    if (gateway->gateway) {
        caddons_node_gateway_destroy(gateway->gateway);
    }
    if (gateway->transport) {
        caddons_ws_transport_destroy(gateway->transport);
    }
    if (gateway->catalog) {
        caddons_remote_catalog_destroy(gateway->catalog);
    }
    if (gateway->state_mutex_ready) {
        pthread_mutex_destroy(&gateway->state_mutex);
    }
    if (gateway->sem_ready) {
        sem_destroy(&gateway->mutation_sem);
    }
    secure_clear(gateway, sizeof(*gateway));
    free(gateway);
}

int smart_home_node_gateway_start(smart_home_node_gateway_t **gateway_out,
                                  agent_t *agent,
                                  pthread_mutex_t *agent_mutex)
{
    return smart_home_node_gateway_start_with_port(
        gateway_out, agent, agent_mutex, CONFIG_SMART_HOME_NODE_GATEWAY_PORT);
}

int smart_home_node_gateway_start_with_port(
    smart_home_node_gateway_t **gateway_out,
    agent_t *agent,
    pthread_mutex_t *agent_mutex,
    uint16_t listen_port)
{
    caddons_ws_transport_config_t transport_config;
    caddons_node_gateway_config_t gateway_config;
    smart_home_node_gateway_t *gateway;
    char shared_token[CAGENT_ADDONS_AUTH_TOKEN_MAX_LENGTH + 1u];
    int rc;

    if (!gateway_out || *gateway_out || !agent || !agent_mutex ||
        listen_port == 0u) {
        return AGENT_ERROR_INVALID;
    }
    memset(shared_token, 0, sizeof(shared_token));

    gateway = calloc(1u, sizeof(*gateway));
    if (!gateway) {
        return AGENT_ERROR_NOMEM;
    }
    ov_mem_region_log("node-gateway-context", gateway);
    gateway->agent = agent;
    gateway->agent_mutex = agent_mutex;

    rc = load_shared_token(shared_token, sizeof(shared_token));
    if (rc != CADDONS_OK) {
        if (rc == CADDONS_ERR_NOT_FOUND) {
            syslog(LOG_WARNING,
                   "smart_home: Node gateway credentials file is missing: %s\n",
                   CONFIG_SMART_HOME_NODE_GATEWAY_SECRETS_PATH);
        } else {
            syslog(LOG_WARNING,
                   "smart_home: Node gateway credentials are invalid: %d\n", rc);
        }
        goto fail;
    }

    gateway->catalog = caddons_remote_catalog_create();
    if (!gateway->catalog) {
        rc = CADDONS_ERR_NOMEM;
        goto fail;
    }

    memset(&transport_config, 0, sizeof(transport_config));
    transport_config.max_peers = CAGENT_ADDONS_MAX_NODES;
    transport_config.connect_timeout_ms = CAGENT_ADDONS_WS_CONNECT_TIMEOUT_MS;
    transport_config.io_timeout_ms = CAGENT_ADDONS_WS_IO_TIMEOUT_MS;
    transport_config.stack_provider.alloc_fn = node_stack_alloc;
    transport_config.stack_provider.free_fn = node_stack_free;
    transport_config.stack_provider.user_data = gateway;
    gateway->transport = caddons_ws_transport_create(&transport_config);
    if (!gateway->transport) {
        rc = CADDONS_ERR_NOMEM;
        goto fail;
    }

    memset(&gateway_config, 0, sizeof(gateway_config));
    gateway_config.transport = gateway->transport;
    gateway_config.catalog = gateway->catalog;
    gateway_config.bind_host = CONFIG_SMART_HOME_NODE_GATEWAY_BIND_HOST;
    gateway_config.listen_port = listen_port;
    gateway_config.ws_path = CAGENT_ADDONS_NODE_DEFAULT_PATH;
    gateway_config.auth_token = shared_token;
    gateway_config.max_nodes = CAGENT_ADDONS_MAX_NODES;
    gateway_config.max_pending = CAGENT_ADDONS_MAX_PENDING;
    gateway_config.invoke_timeout_ms = CAGENT_ADDONS_INVOKE_TIMEOUT_MS;
    gateway_config.catalog_changed_fn = catalog_changed;
    gateway_config.catalog_changed_user_data = gateway;
    gateway_config.stack_provider = transport_config.stack_provider;
    gateway->gateway = caddons_node_gateway_create(&gateway_config);
    secure_clear(shared_token, sizeof(shared_token));
    if (!gateway->gateway) {
        rc = CADDONS_ERR_INVALID;
        goto fail;
    }

    rc = start_worker(gateway);
    if (rc != CADDONS_OK) {
        goto fail;
    }
    rc = caddons_node_gateway_start(gateway->gateway);
    if (rc != CADDONS_OK) {
        printf("[smart_home_node] gateway start failed rc=%d\n", rc);
        goto fail;
    }

    gateway->listen_port = listen_port;
    *gateway_out = gateway;
    printf("[smart_home_node] gateway listening on %s:%u\n",
           CONFIG_SMART_HOME_NODE_GATEWAY_BIND_HOST,
           (unsigned int)listen_port);
    syslog(LOG_INFO, "smart_home: Node gateway listening on %s:%u\n",
           CONFIG_SMART_HOME_NODE_GATEWAY_BIND_HOST,
           (unsigned int)listen_port);
    return AGENT_OK;

fail:
    secure_clear(shared_token, sizeof(shared_token));
    destroy_gateway(gateway);
    return caddons_error_to_agent(rc);
}

void smart_home_node_gateway_stop(smart_home_node_gateway_t **gateway_ptr)
{
    if (!gateway_ptr || !*gateway_ptr) {
        return;
    }

    destroy_gateway(*gateway_ptr);
    *gateway_ptr = NULL;
}

uint16_t smart_home_node_gateway_port(const smart_home_node_gateway_t *gateway)
{
    return gateway ? gateway->listen_port : 0;
}

size_t smart_home_node_gateway_list(const smart_home_node_gateway_t *gateway,
                                    caddons_node_info_t *nodes,
                                    size_t capacity,
                                    uint32_t *revision_out)
{
    smart_home_node_gateway_t *mutable_gateway =
        (smart_home_node_gateway_t *)gateway;
    size_t count;

    if (revision_out) {
        *revision_out = 0;
    }
    if (!gateway || !gateway->gateway) {
        return 0;
    }

    count = caddons_node_gateway_list(gateway->gateway, nodes, capacity);
    if (revision_out && gateway->state_mutex_ready) {
        pthread_mutex_lock(&mutable_gateway->state_mutex);
        *revision_out = gateway->presentation_revision;
        pthread_mutex_unlock(&mutable_gateway->state_mutex);
    }
    return count;
}
