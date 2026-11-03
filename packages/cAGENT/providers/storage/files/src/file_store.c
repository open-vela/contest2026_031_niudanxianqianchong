/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* Capability dispatch and bounded byte/text loading without heap allocation. */
#include <agent_file_store.h>

#include <stdint.h>
#include <string.h>

static bool valid_name(agent_string_view_t name)
{
    size_t i;

    if (!name.data || !name.size ||
        (name.size == 1u && name.data[0] == '.') ||
        (name.size == 2u && memcmp(name.data, "..", 2u) == 0) ||
        (name.size >= 8u && memcmp(name.data, ".cagent-", 8u) == 0))
        return false;
    for (i = 0u; i < name.size; ++i) {
        if (!name.data[i] || name.data[i] == '/' || name.data[i] == '\\') return false;
    }
    return true;
}

static bool valid_store(const agent_file_store_t* store)
{
    return store && store->ops;
}

agent_error_t agent_file_size(const agent_file_store_t* store,
                              agent_string_view_t name, uint64_t* bytes)
{
    if (!bytes) return AGENT_ERROR_INVALID;
    *bytes = 0u;
    if (!valid_store(store) || !valid_name(name)) return AGENT_ERROR_INVALID;
    if (!store->ops->size) return AGENT_ERROR_NOT_SUPPORTED;
    return store->ops->size(store->context, name, bytes);
}

agent_error_t agent_file_read(const agent_file_store_t* store, agent_string_view_t name,
                              uint64_t offset, void* output, size_t capacity,
                              size_t* bytes_read)
{
    size_t count = 0u;
    agent_error_t status;

    if (!bytes_read) return AGENT_ERROR_INVALID;
    *bytes_read = 0u;
    if (!valid_store(store) || !valid_name(name) || (capacity && !output) ||
        offset > UINT64_MAX - capacity)
        return AGENT_ERROR_INVALID;
    if (!store->ops->read) return AGENT_ERROR_NOT_SUPPORTED;
    status = store->ops->read(store->context, name, offset, output, capacity, &count);
    if (status != AGENT_OK) return status;
    if (count > capacity) return AGENT_ERROR_IO;
    *bytes_read = count;
    return AGENT_OK;
}

agent_error_t agent_file_read_exact(const agent_file_store_t* store,
                                    agent_string_view_t name, uint64_t offset,
                                    void* output, size_t bytes)
{
    size_t done = 0u;

    if (!valid_store(store) || !valid_name(name) || (bytes && !output) ||
        offset > UINT64_MAX - bytes)
        return AGENT_ERROR_INVALID;
    if (!store->ops->read) return AGENT_ERROR_NOT_SUPPORTED;
    while (done < bytes) {
        size_t count;
        agent_error_t status = agent_file_read(store, name, offset + done,
                                (unsigned char*)output + done, bytes - done, &count);
        if (status != AGENT_OK) return status;
        if (!count) return AGENT_ERROR_IO;
        done += count;
    }
    return AGENT_OK;
}

agent_error_t agent_file_read_all(const agent_file_store_t* store,
                                  agent_string_view_t name, void* output,
                                  size_t capacity, size_t max_bytes, size_t* bytes_read)
{
    uint64_t length;
    uint64_t after;
    unsigned char extra;
    size_t count;
    agent_error_t status;

    if (!bytes_read) return AGENT_ERROR_INVALID;
    *bytes_read = 0u;
    if ((capacity && !output) || !max_bytes) return AGENT_ERROR_INVALID;
    status = agent_file_size(store, name, &length);
    if (status != AGENT_OK) return status;
    if (length > capacity || length > max_bytes) return AGENT_ERROR_CAPACITY;
    status = agent_file_read_exact(store, name, 0u, output, (size_t)length);
    if (status != AGENT_OK) return status;
    status = agent_file_read(store, name, length, &extra, 1u, &count);
    if (status != AGENT_OK) return status;
    if (count) return AGENT_ERROR_IO;
    status = agent_file_size(store, name, &after);
    if (status != AGENT_OK) return status;
    if (after != length) return AGENT_ERROR_IO;
    *bytes_read = (size_t)length;
    return AGENT_OK;
}

agent_error_t agent_file_read_text(const agent_file_store_t* store,
                                   agent_string_view_t name, char* output,
                                   size_t capacity, size_t max_bytes,
                                   agent_string_view_t* text)
{
    size_t bytes;
    agent_error_t status;

    if (!text) return AGENT_ERROR_INVALID;
    *text = agent_string_view(NULL, 0u);
    if (!output || !capacity) return AGENT_ERROR_INVALID;
    status = agent_file_read_all(store, name, output, capacity - 1u, max_bytes, &bytes);
    if (status == AGENT_OK && memchr(output, '\0', bytes)) status = AGENT_ERROR_PARSE;
    if (status != AGENT_OK) {
        output[0] = '\0';
        return status;
    }
    output[bytes] = '\0';
    *text = agent_string_view(output, bytes);
    return AGENT_OK;
}

typedef struct {
    agent_file_visit_fn visitor;
    void* context;
    agent_error_t status;
    bool stopped;
} visit_state_t;

static agent_error_t visit_checked(void* context, agent_string_view_t name, bool* stop)
{
    visit_state_t* state = context;

    if (!stop) return state->status = AGENT_ERROR_INVALID;
    *stop = true;
    if (state->status != AGENT_OK) return state->status;
    if (state->stopped) return AGENT_OK;
    if (!valid_name(name)) return state->status = AGENT_ERROR_IO;
    *stop = false;
    state->status = state->visitor(state->context, name, stop);
    state->stopped = *stop || state->status != AGENT_OK;
    if (state->stopped) *stop = true;
    return state->status;
}

agent_error_t agent_file_visit(const agent_file_store_t* store,
                               agent_file_visit_fn visitor, void* context)
{
    visit_state_t state;
    agent_error_t status;

    if (!valid_store(store) || !visitor) return AGENT_ERROR_INVALID;
    if (!store->ops->visit) return AGENT_ERROR_NOT_SUPPORTED;
    state.visitor = visitor;
    state.context = context;
    state.status = AGENT_OK;
    state.stopped = false;
    status = store->ops->visit(store->context, visit_checked, &state);
    return state.status != AGENT_OK ? state.status : status;
}

agent_error_t agent_file_append(const agent_file_store_t* store,
                                agent_string_view_t name, const void* data, size_t bytes)
{
    if (!valid_store(store) || !valid_name(name) || (bytes && !data))
        return AGENT_ERROR_INVALID;
    if (!store->ops->append) return AGENT_ERROR_NOT_SUPPORTED;
    return store->ops->append(store->context, name, data, bytes);
}

agent_error_t agent_file_truncate(const agent_file_store_t* store,
                                  agent_string_view_t name, uint64_t bytes)
{
    if (!valid_store(store) || !valid_name(name)) return AGENT_ERROR_INVALID;
    if (!store->ops->truncate) return AGENT_ERROR_NOT_SUPPORTED;
    return store->ops->truncate(store->context, name, bytes);
}

agent_error_t agent_file_sync(const agent_file_store_t* store, agent_string_view_t name)
{
    if (!valid_store(store) || !valid_name(name)) return AGENT_ERROR_INVALID;
    if (!store->ops->sync) return AGENT_ERROR_NOT_SUPPORTED;
    return store->ops->sync(store->context, name);
}

agent_error_t agent_file_remove(const agent_file_store_t* store, agent_string_view_t name)
{
    if (!valid_store(store) || !valid_name(name)) return AGENT_ERROR_INVALID;
    if (!store->ops->remove) return AGENT_ERROR_NOT_SUPPORTED;
    return store->ops->remove(store->context, name);
}

agent_error_t agent_file_replace(const agent_file_store_t* store,
                                 agent_string_view_t name, const void* data,
                                 size_t bytes, bool* published)
{
    if (!published) return AGENT_ERROR_INVALID;
    *published = false;
    if (!valid_store(store) || !valid_name(name) || (bytes && !data))
        return AGENT_ERROR_INVALID;
    if (!store->ops->replace) return AGENT_ERROR_NOT_SUPPORTED;
    return store->ops->replace(store->context, name, data, bytes, published);
}
