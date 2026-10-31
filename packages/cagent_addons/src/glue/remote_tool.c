/* SPDX-License-Identifier: Apache-2.0 */
/** Remote tool catalog and cAGENT registry glue. */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "cagent_addons/remote_tool.h"

#include <agent.h>
#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cagent_addons/cjson_compat.h"

#define RESULT_PREFIX_RESERVE 64u

typedef struct {
    struct caddons_remote_catalog *owner;
    bool occupied;
    caddons_remote_descriptor_t descriptor;
    caddons_remote_descriptor_t *replacement;
    caddons_remote_state_t state;
    bool online;
    bool enabled;
    uint32_t inflight;
} remote_entry_t;

struct caddons_remote_catalog {
    pthread_mutex_t mutex;
    bool active_execution;
    char result_json[CAGENT_ADDONS_MAX_RESULT_SIZE + 1];
    /* agent_tool_context_t 的参数是非 NUL 结尾视图，后端执行函数需要 C 字符串；
     * 执行已被 active_execution 串行化，因此复用一个缓冲即可。 */
    char args_json[CAGENT_ADDONS_MAX_SCHEMA_SIZE + 1];
    remote_entry_t entries[CAGENT_ADDONS_MAX_REMOTE_TOOLS];
};

static uint64_t monotonic_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static bool terminated(const char *value, size_t capacity)
{
    return value && memchr(value, '\0', capacity) != NULL;
}

static cJSON *parse_json_exact(const char *json)
{
    const char *end = NULL;
    size_t length;
    cJSON *value;
    if (!json) return NULL;
    length = strlen(json);
    value = cJSON_ParseWithLengthOpts(json, length, &end, 0);
    while (value && end < json + length && isspace((unsigned char)*end)) end++;
    if (!value || end != json + length) {
        cJSON_Delete(value);
        return NULL;
    }
    return value;
}

static bool valid_public_name(const char *name)
{
    const unsigned char *cursor = (const unsigned char *)name;
    if (!cursor || !*cursor) return false;
    for (; *cursor; cursor++) {
        if (!((*cursor >= 'a' && *cursor <= 'z')
              || (*cursor >= 'A' && *cursor <= 'Z')
              || (*cursor >= '0' && *cursor <= '9')
              || *cursor == '_' || *cursor == '-')) return false;
    }
    return true;
}

static int validate_descriptor(const caddons_remote_descriptor_t *tool)
{
    cJSON *schema;
    size_t name_len;
    if (!tool || !tool->execute
        || !terminated(tool->route_id, sizeof(tool->route_id))
        || !terminated(tool->source_name, sizeof(tool->source_name))
        || !terminated(tool->public_name, sizeof(tool->public_name))
        || !terminated(tool->description, sizeof(tool->description))
        || !terminated(tool->input_schema_json, sizeof(tool->input_schema_json)))
        return CADDONS_ERR_INVALID;
    name_len = strlen(tool->public_name);
    if (!tool->route_id[0] || !tool->source_name[0] || name_len == 0
        || name_len > CAGENT_ADDONS_MAX_PUBLIC_NAME_SIZE
        || !valid_public_name(tool->public_name)
        || !tool->description[0] || !tool->input_schema_json[0]
        || tool->timeout_ms == 0) return CADDONS_ERR_INVALID;
    if (tool->origin != CADDONS_TOOL_NODE && tool->origin != CADDONS_TOOL_MCP)
        return CADDONS_ERR_INVALID;
    schema = parse_json_exact(tool->input_schema_json);
    if (!schema || !cJSON_IsObject(schema)) {
        cJSON_Delete(schema);
        return CADDONS_ERR_PARSE;
    }
    cJSON_Delete(schema);
    return CADDONS_OK;
}

static size_t origin_count(const caddons_remote_catalog_t *catalog,
                           caddons_tool_origin_t origin)
{
    size_t i;
    size_t count = 0;
    for (i = 0; i < CAGENT_ADDONS_MAX_REMOTE_TOOLS; i++) {
        if (catalog->entries[i].occupied
            && catalog->entries[i].state != CADDONS_REMOTE_REMOVED
            && catalog->entries[i].descriptor.origin == origin) count++;
    }
    return count;
}

static remote_entry_t *find_public(caddons_remote_catalog_t *catalog,
                                   const char *public_name)
{
    size_t i;
    for (i = 0; i < CAGENT_ADDONS_MAX_REMOTE_TOOLS; i++) {
        if (catalog->entries[i].occupied
            && strcmp(catalog->entries[i].descriptor.public_name, public_name) == 0)
            return &catalog->entries[i];
    }
    return NULL;
}

static remote_entry_t *find_mutation(caddons_remote_catalog_t *catalog,
                                     const caddons_remote_mutation_t *mutation)
{
    remote_entry_t *entry = find_public(catalog, mutation->public_name);
    if (!entry || entry->descriptor.connection_gen != mutation->connection_gen)
        return NULL;
    return entry;
}

static uint32_t tool_flags(caddons_tool_risk_t risk)
{
    uint32_t flags = 0;
    if (risk == CADDONS_TOOL_RISK_READ_ONLY) flags |= AGENT_TOOL_READ_ONLY;
    if (risk == CADDONS_TOOL_RISK_SIDE_EFFECT) flags |= AGENT_TOOL_SIDE_EFFECT;
    if (risk == CADDONS_TOOL_RISK_DANGEROUS)
        flags |= AGENT_TOOL_SIDE_EFFECT | AGENT_TOOL_REQUIRES_CONFIRM;
    return flags;
}

static const char *origin_name(caddons_tool_origin_t origin)
{
    return origin == CADDONS_TOOL_NODE ? "node" : "mcp";
}

static int format_error(caddons_remote_catalog_t *catalog,
                        remote_entry_t *entry, int error)
{
    int length = snprintf(catalog->result_json, sizeof(catalog->result_json),
                          "{\"ok\":false,\"source\":\"%s\","
                          "\"error\":{\"code\":\"%s\",\"retryable\":%s,"
                          "\"message\":\"%s\"}}",
                          origin_name(entry->descriptor.origin),
                          caddons_error_code(error),
                          error == CADDONS_ERR_NETWORK || error == CADDONS_ERR_TIMEOUT
                              || error == CADDONS_ERR_OFFLINE ? "true" : "false",
                          caddons_error_code(error));
    return length < 0 || (size_t)length >= sizeof(catalog->result_json)
               ? CADDONS_ERR_LIMIT : CADDONS_OK;
}

static int format_success(caddons_remote_catalog_t *catalog,
                          remote_entry_t *entry)
{
    char prefix[RESULT_PREFIX_RESERVE];
    char *content = catalog->result_json + RESULT_PREFIX_RESERVE;
    cJSON *parsed;
    size_t content_len;
    size_t prefix_len;
    int length;
    parsed = parse_json_exact(content);
    if (!parsed) return format_error(catalog, entry, CADDONS_ERR_PARSE);
    cJSON_Delete(parsed);
    content_len = strlen(content);
    length = snprintf(prefix, sizeof(prefix),
                      "{\"ok\":true,\"source\":\"%s\",\"content\":",
                      origin_name(entry->descriptor.origin));
    if (length < 0 || (size_t)length >= sizeof(prefix)) return CADDONS_ERR_LIMIT;
    prefix_len = (size_t)length;
    if (prefix_len + content_len + 2 > sizeof(catalog->result_json))
        return format_error(catalog, entry, CADDONS_ERR_LIMIT);
    memmove(catalog->result_json + prefix_len, content, content_len);
    memcpy(catalog->result_json, prefix, prefix_len);
    catalog->result_json[prefix_len + content_len] = '}';
    catalog->result_json[prefix_len + content_len + 1] = '\0';
    return CADDONS_OK;
}

static agent_error_t remote_execute(void *user_data,
                                          const agent_tool_context_t *context,
                                          const agent_text_sink_t *output)
{
    remote_entry_t *entry = (remote_entry_t *)user_data;
    caddons_remote_execute_fn execute;
    void *backend_context;
    uint64_t generation;
    uint64_t deadline;
    uint32_t timeout_ms;
    char *backend_result;
    size_t backend_result_size;
    size_t args_size;
    agent_error_t sink_rc;
    int rc;
    caddons_remote_catalog_t *catalog;

    if (!entry || !output || !output->write) return AGENT_ERROR_INVALID;
    catalog = entry->owner;

    args_size = context ? context->call.arguments_json.size : 0;
    if (args_size >= sizeof(catalog->args_json)) return AGENT_ERROR_LIMIT;

    pthread_mutex_lock(&catalog->mutex);
    if (!entry->occupied || entry->state != CADDONS_REMOTE_REGISTERED
        || !entry->online || !entry->enabled) {
        format_error(catalog, entry, CADDONS_ERR_OFFLINE);
    }
    else if (catalog->active_execution || entry->inflight != 0) {
        pthread_mutex_unlock(&catalog->mutex);
        return AGENT_ERROR_BUSY;
    }
    else {
        if (args_size > 0) memcpy(catalog->args_json, context->call.arguments_json.data, args_size);
        else { catalog->args_json[0] = '{'; catalog->args_json[1] = '}'; args_size = 2; }
        catalog->args_json[args_size] = '\0';

        catalog->active_execution = true;
        entry->inflight++;
        execute = entry->descriptor.execute;
        backend_context = entry->descriptor.backend_context;
        generation = entry->descriptor.connection_gen;
        timeout_ms = entry->descriptor.timeout_ms;
        backend_result = catalog->result_json + RESULT_PREFIX_RESERVE;
        backend_result_size = sizeof(catalog->result_json) - RESULT_PREFIX_RESERVE;
        backend_result[0] = '\0';
        backend_result[backend_result_size - 1] = '\0';
        pthread_mutex_unlock(&catalog->mutex);

        deadline = monotonic_ms() + timeout_ms;
        if (context && context->deadline_ms != 0 && context->deadline_ms < deadline)
            deadline = context->deadline_ms;
        rc = execute(backend_context, entry->descriptor.route_id, generation,
                     catalog->args_json, deadline, backend_result, backend_result_size);

        pthread_mutex_lock(&catalog->mutex);
        backend_result[backend_result_size - 1] = '\0';
        if (rc == CADDONS_OK) rc = format_success(catalog, entry);
        else format_error(catalog, entry, rc);
        entry->inflight--;
        catalog->active_execution = false;
    }

    sink_rc = output->write(output->context,
                            agent_string_view(catalog->result_json,
                                              strlen(catalog->result_json)));
    pthread_mutex_unlock(&catalog->mutex);
    return sink_rc == AGENT_OK ? AGENT_OK : sink_rc;
}

caddons_remote_catalog_t *caddons_remote_catalog_create(void)
{
    caddons_remote_catalog_t *catalog = calloc(1, sizeof(*catalog));
    if (!catalog) return NULL;
    if (pthread_mutex_init(&catalog->mutex, NULL) != 0) {
        free(catalog);
        return NULL;
    }
    return catalog;
}

void caddons_remote_catalog_destroy(caddons_remote_catalog_t *catalog)
{
    size_t i;
    if (!catalog) return;
    for (i = 0; i < CAGENT_ADDONS_MAX_REMOTE_TOOLS; i++)
        free(catalog->entries[i].replacement);
    pthread_mutex_destroy(&catalog->mutex);
    free(catalog);
}

int caddons_remote_catalog_discover(caddons_remote_catalog_t *catalog,
                                    const caddons_remote_descriptor_t *tool)
{
    remote_entry_t *entry;
    size_t i;
    size_t limit;
    int rc = validate_descriptor(tool);
    if (!catalog || rc != CADDONS_OK) return rc;
    pthread_mutex_lock(&catalog->mutex);
    entry = find_public(catalog, tool->public_name);
    if (entry) {
        if (entry->state == CADDONS_REMOTE_REMOVED) {
            memset(entry, 0, sizeof(*entry));
            entry->owner = catalog;
            entry->occupied = true;
            entry->descriptor = *tool;
            entry->state = CADDONS_REMOTE_REGISTER_QUEUED;
            entry->online = true;
            entry->enabled = true;
            pthread_mutex_unlock(&catalog->mutex);
            return CADDONS_OK;
        }
        if (strcmp(entry->descriptor.route_id, tool->route_id) == 0
            && entry->descriptor.connection_gen == tool->connection_gen) {
            pthread_mutex_unlock(&catalog->mutex);
            return CADDONS_OK;
        }
        if ((entry->state == CADDONS_REMOTE_DISABLE_QUEUED
             || entry->state == CADDONS_REMOTE_DRAINING)
            && entry->descriptor.origin == tool->origin) {
            caddons_remote_descriptor_t *replacement = malloc(sizeof(*replacement));
            if (!replacement) {
                pthread_mutex_unlock(&catalog->mutex);
                return CADDONS_ERR_NOMEM;
            }
            *replacement = *tool;
            free(entry->replacement);
            entry->replacement = replacement;
            pthread_mutex_unlock(&catalog->mutex);
            return CADDONS_OK;
        }
        pthread_mutex_unlock(&catalog->mutex);
        return CADDONS_ERR_INVALID;
    }
    limit = tool->origin == CADDONS_TOOL_NODE ? CAGENT_ADDONS_MAX_NODE_TOOLS
                                              : CAGENT_ADDONS_MAX_MCP_TOOLS;
    if (origin_count(catalog, tool->origin) >= limit) {
        pthread_mutex_unlock(&catalog->mutex);
        return CADDONS_ERR_LIMIT;
    }
    entry = NULL;
    for (i = 0; i < CAGENT_ADDONS_MAX_REMOTE_TOOLS; i++) {
        if (!catalog->entries[i].occupied
            || catalog->entries[i].state == CADDONS_REMOTE_REMOVED) {
            entry = &catalog->entries[i];
            break;
        }
    }
    if (!entry) {
        pthread_mutex_unlock(&catalog->mutex);
        return CADDONS_ERR_LIMIT;
    }
    memset(entry, 0, sizeof(*entry));
    entry->owner = catalog;
    entry->occupied = true;
    entry->descriptor = *tool;
    entry->state = CADDONS_REMOTE_REGISTER_QUEUED;
    entry->online = true;
    entry->enabled = true;
    pthread_mutex_unlock(&catalog->mutex);
    return CADDONS_OK;
}

int caddons_remote_catalog_remove_source(caddons_remote_catalog_t *catalog,
                                         caddons_tool_origin_t origin,
                                         const char *route_prefix,
                                         uint64_t connection_gen)
{
    size_t i;
    size_t prefix_len;
    bool found = false;
    if (!catalog || !route_prefix || !route_prefix[0]) return CADDONS_ERR_INVALID;
    prefix_len = strlen(route_prefix);
    pthread_mutex_lock(&catalog->mutex);
    for (i = 0; i < CAGENT_ADDONS_MAX_REMOTE_TOOLS; i++) {
        remote_entry_t *entry = &catalog->entries[i];
        if (!entry->occupied || entry->descriptor.origin != origin
            || entry->descriptor.connection_gen != connection_gen
            || strncmp(entry->descriptor.route_id, route_prefix, prefix_len) != 0)
            continue;
        found = true;
        entry->online = false;
        entry->enabled = false;
        if (entry->state == CADDONS_REMOTE_REGISTERED)
            entry->state = CADDONS_REMOTE_DISABLE_QUEUED;
        else if (entry->state == CADDONS_REMOTE_REGISTER_QUEUED) {
            entry->state = CADDONS_REMOTE_REMOVED;
            entry->occupied = false;
        }
    }
    pthread_mutex_unlock(&catalog->mutex);
    return found ? CADDONS_OK : CADDONS_ERR_NOT_FOUND;
}

int caddons_remote_catalog_next_mutation(caddons_remote_catalog_t *catalog,
                                         caddons_remote_mutation_t *mutation)
{
    size_t i;
    if (!catalog || !mutation) return CADDONS_ERR_INVALID;
    pthread_mutex_lock(&catalog->mutex);
    for (i = 0; i < CAGENT_ADDONS_MAX_REMOTE_TOOLS; i++) {
        remote_entry_t *entry = &catalog->entries[i];
        if (entry->occupied && entry->state == CADDONS_REMOTE_DISABLE_QUEUED
            && entry->inflight == 0) {
            mutation->type = CADDONS_MUTATION_UNREGISTER;
            strcpy(mutation->public_name, entry->descriptor.public_name);
            mutation->connection_gen = entry->descriptor.connection_gen;
            entry->state = CADDONS_REMOTE_DRAINING;
            pthread_mutex_unlock(&catalog->mutex);
            return CADDONS_OK;
        }
    }
    for (i = 0; i < CAGENT_ADDONS_MAX_REMOTE_TOOLS; i++) {
        remote_entry_t *entry = &catalog->entries[i];
        if (entry->occupied && entry->state == CADDONS_REMOTE_REGISTER_QUEUED) {
            mutation->type = CADDONS_MUTATION_REGISTER;
            strcpy(mutation->public_name, entry->descriptor.public_name);
            mutation->connection_gen = entry->descriptor.connection_gen;
            pthread_mutex_unlock(&catalog->mutex);
            return CADDONS_OK;
        }
    }
    pthread_mutex_unlock(&catalog->mutex);
    return CADDONS_ERR_NOT_FOUND;
}

int caddons_remote_catalog_apply_mutation(caddons_remote_catalog_t *catalog,
                                          agent_t *agent,
                                          const caddons_remote_mutation_t *mutation)
{
    remote_entry_t *entry;
    agent_tool_t tool;
    int rc;
    if (!catalog || !agent || !mutation) return CADDONS_ERR_INVALID;
    pthread_mutex_lock(&catalog->mutex);
    entry = find_mutation(catalog, mutation);
    if (!entry) {
        pthread_mutex_unlock(&catalog->mutex);
        return CADDONS_ERR_NOT_FOUND;
    }
    if (mutation->type == CADDONS_MUTATION_REGISTER) {
        if (entry->state != CADDONS_REMOTE_REGISTER_QUEUED) {
            pthread_mutex_unlock(&catalog->mutex);
            return CADDONS_ERR_INVALID;
        }
        memset(&tool, 0, sizeof(tool));
        tool.name = agent_string_view(entry->descriptor.public_name,
                                      strlen(entry->descriptor.public_name));
        tool.group = agent_string_view(origin_name(entry->descriptor.origin),
                                       strlen(origin_name(entry->descriptor.origin)));
        tool.description = agent_string_view(entry->descriptor.description,
                                             strlen(entry->descriptor.description));
        tool.input_schema_json =
            agent_string_view(entry->descriptor.input_schema_json,
                              strlen(entry->descriptor.input_schema_json));
        tool.execute = remote_execute;
        tool.user_data = entry;
        tool.flags = tool_flags(entry->descriptor.risk);
        /* V2 的 agent_tool_t 不再携带 timeout；deadline 由执行上下文与
         * descriptor.timeout_ms 在 remote_execute 内合成。 */
        rc = agent_register_tool(agent, &tool);
        if (rc == AGENT_OK) entry->state = CADDONS_REMOTE_REGISTERED;
        pthread_mutex_unlock(&catalog->mutex);
        return rc == AGENT_OK ? CADDONS_OK : rc == AGENT_ERROR_LIMIT
            ? CADDONS_ERR_LIMIT : CADDONS_ERR_INVALID;
    }
    if (mutation->type != CADDONS_MUTATION_UNREGISTER
        || entry->state != CADDONS_REMOTE_DRAINING || entry->inflight != 0) {
        pthread_mutex_unlock(&catalog->mutex);
        return CADDONS_ERR_INVALID;
    }
    rc = agent_unregister_tool(agent,
                               agent_string_view(entry->descriptor.public_name,
                                                 strlen(entry->descriptor.public_name)));
    if (rc == AGENT_OK || rc == AGENT_ERROR_NOT_FOUND) {
        if (entry->replacement) {
            caddons_remote_descriptor_t replacement = *entry->replacement;
            free(entry->replacement);
            memset(entry, 0, sizeof(*entry));
            entry->owner = catalog;
            entry->occupied = true;
            entry->descriptor = replacement;
            entry->state = CADDONS_REMOTE_REGISTER_QUEUED;
            entry->online = true;
            entry->enabled = true;
        } else {
            entry->state = CADDONS_REMOTE_REMOVED;
            entry->occupied = false;
        }
    } else {
        entry->state = CADDONS_REMOTE_DISABLE_QUEUED;
    }
    pthread_mutex_unlock(&catalog->mutex);
    return rc == AGENT_OK || rc == AGENT_ERROR_NOT_FOUND ? CADDONS_OK
                                                         : CADDONS_ERR_INTERNAL;
}

size_t caddons_remote_catalog_list(const caddons_remote_catalog_t *catalog,
                                   caddons_remote_view_t *tools,
                                   size_t capacity)
{
    size_t i;
    size_t count = 0;
    caddons_remote_catalog_t *mutable_catalog = (caddons_remote_catalog_t *)catalog;
    if (!catalog) return 0;
    pthread_mutex_lock(&mutable_catalog->mutex);
    for (i = 0; i < CAGENT_ADDONS_MAX_REMOTE_TOOLS; i++) {
        const remote_entry_t *entry = &catalog->entries[i];
        if (!entry->occupied) continue;
        if (tools && count < capacity) {
            tools[count].descriptor = entry->descriptor;
            tools[count].state = entry->state;
            tools[count].online = entry->online;
            tools[count].enabled = entry->enabled;
            tools[count].inflight = entry->inflight;
        }
        count++;
    }
    pthread_mutex_unlock(&mutable_catalog->mutex);
    return count;
}

int caddons_remote_catalog_set_enabled(caddons_remote_catalog_t *catalog,
                                       agent_t *agent,
                                       const char *public_name,
                                       bool enabled)
{
    remote_entry_t *entry;
    int rc;
    if (!catalog || !agent || !public_name) return CADDONS_ERR_INVALID;
    pthread_mutex_lock(&catalog->mutex);
    entry = find_public(catalog, public_name);
    if (!entry || entry->state != CADDONS_REMOTE_REGISTERED) {
        pthread_mutex_unlock(&catalog->mutex);
        return CADDONS_ERR_NOT_FOUND;
    }
    rc = agent_tool_set_enabled(agent, agent_string_view(public_name, strlen(public_name)),
                                enabled);
    if (rc == AGENT_OK) entry->enabled = enabled;
    pthread_mutex_unlock(&catalog->mutex);
    return rc == AGENT_OK ? CADDONS_OK : CADDONS_ERR_INTERNAL;
}

int caddons_error_to_agent(int error)
{
    switch (error) {
    case CADDONS_OK: return AGENT_OK;
    case CADDONS_ERR_INVALID: return AGENT_ERROR_INVALID;
    case CADDONS_ERR_PARSE: return AGENT_ERROR_PARSE;
    case CADDONS_ERR_LIMIT: return AGENT_ERROR_LIMIT;
    /* V2 没有独立的网络错误码，链路失败归入 IO。 */
    case CADDONS_ERR_NETWORK:
    case CADDONS_ERR_OFFLINE: return AGENT_ERROR_IO;
    case CADDONS_ERR_TIMEOUT: return AGENT_ERROR_TIMEOUT;
    case CADDONS_ERR_DENIED: return AGENT_ERROR_POLICY_DENIED;
    case CADDONS_ERR_BUSY: return AGENT_ERROR_BUSY;
    case CADDONS_ERR_NOT_FOUND: return AGENT_ERROR_NOT_FOUND;
    case CADDONS_ERR_NOMEM: return AGENT_ERROR_NOMEM;
    default: return AGENT_ERROR;
    }
}
