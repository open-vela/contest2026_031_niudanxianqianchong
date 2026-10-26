/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* Session-specific flat filenames; generic stores never own bulk Session semantics. */
#include <agent_session_jsonl_files.h>

#include <stdint.h>
#include <string.h>

#define PREFIX "session-"
#define SUFFIX ".jsonl"

static bool overlaps(const void* a, size_t a_bytes, const void* b, size_t b_bytes)
{
    uintptr_t left = (uintptr_t)a;
    uintptr_t right = (uintptr_t)b;
    if (!a_bytes || !b_bytes) return false;
    if (a_bytes > UINTPTR_MAX - left || b_bytes > UINTPTR_MAX - right) return true;
    return left < right + b_bytes && right < left + a_bytes;
}

static agent_error_t session_name(agent_session_jsonl_files_t* files,
                                   agent_string_view_t id, agent_string_view_t* name)
{
    static const char hex[] = "0123456789abcdef";
    size_t i;
    size_t pos = sizeof(PREFIX) - 1u;

    if (!id.data || !id.size || overlaps(id.data, id.size, files->name, files->name_capacity))
        return AGENT_ERROR_INVALID;
    if (id.size > (files->name_capacity - (sizeof(PREFIX) - 1u) - sizeof(SUFFIX)) / 2u)
        return AGENT_ERROR_CAPACITY;
    memcpy(files->name, PREFIX, pos);
    for (i = 0u; i < id.size; ++i) {
        unsigned char byte = (unsigned char)id.data[i];
        files->name[pos++] = hex[byte >> 4u];
        files->name[pos++] = hex[byte & 15u];
    }
    memcpy(files->name + pos, SUFFIX, sizeof(SUFFIX));
    *name = agent_string_view(files->name, pos + sizeof(SUFFIX) - 1u);
    return AGENT_OK;
}

static bool is_session(agent_string_view_t name)
{
    size_t prefix = sizeof(PREFIX) - 1u;
    size_t suffix = sizeof(SUFFIX) - 1u;
    size_t i;
    if (name.size < prefix + suffix + 2u || (name.size - prefix - suffix) % 2u ||
        memcmp(name.data, PREFIX, prefix) ||
        memcmp(name.data + name.size - suffix, SUFFIX, suffix)) return false;
    for (i = prefix; i < name.size - suffix; ++i) {
        if (!((name.data[i] >= '0' && name.data[i] <= '9') ||
              (name.data[i] >= 'a' && name.data[i] <= 'f'))) return false;
    }
    return true;
}

static agent_error_t file_size(void* context, agent_string_view_t id, uint64_t* bytes)
{
    agent_session_jsonl_files_t* files = context;
    agent_string_view_t name;
    agent_error_t status = session_name(files, id, &name);
    return status == AGENT_OK ? agent_file_size(&files->store, name, bytes) : status;
}

static agent_error_t file_read(void* context, agent_string_view_t id, uint64_t offset,
                               char* output, size_t bytes)
{
    agent_session_jsonl_files_t* files = context;
    agent_string_view_t name;
    agent_error_t status;
    if (overlaps(output, bytes, files->name, files->name_capacity)) return AGENT_ERROR_INVALID;
    status = session_name(files, id, &name);
    return status == AGENT_OK ? agent_file_read_exact(&files->store, name, offset, output, bytes) : status;
}

static agent_error_t file_append(void* context, agent_string_view_t id,
                                 const char* data, size_t bytes)
{
    agent_session_jsonl_files_t* files = context;
    agent_string_view_t name;
    agent_error_t status;
    if (overlaps(data, bytes, files->name, files->name_capacity)) return AGENT_ERROR_INVALID;
    status = session_name(files, id, &name);
    return status == AGENT_OK ? agent_file_append(&files->store, name, data, bytes) : status;
}

static agent_error_t file_truncate(void* context, agent_string_view_t id, uint64_t bytes)
{
    agent_session_jsonl_files_t* files = context;
    agent_string_view_t name;
    agent_error_t status = session_name(files, id, &name);
    return status == AGENT_OK ? agent_file_truncate(&files->store, name, bytes) : status;
}

static agent_error_t file_sync(void* context, agent_string_view_t id)
{
    agent_session_jsonl_files_t* files = context;
    agent_string_view_t name;
    agent_error_t status = session_name(files, id, &name);
    return status == AGENT_OK ? agent_file_sync(&files->store, name) : status;
}

static agent_error_t file_remove(void* context, agent_string_view_t id)
{
    agent_session_jsonl_files_t* files = context;
    agent_string_view_t name;
    agent_error_t status = session_name(files, id, &name);
    return status == AGENT_OK ? agent_file_remove(&files->store, name) : status;
}

static agent_error_t count_entry(void* context, agent_string_view_t name, bool* stop)
{
    size_t* count = context;
    (void)stop;
    if (!is_session(name)) return AGENT_OK;
    if (*count == SIZE_MAX) return AGENT_ERROR_CAPACITY;
    ++*count;
    return AGENT_OK;
}

static agent_error_t file_count(void* context, size_t* count)
{
    agent_session_jsonl_files_t* files = context;
    size_t total = 0u;
    agent_error_t status;
    if (!count) return AGENT_ERROR_INVALID;
    status = agent_file_visit(&files->store, count_entry, &total);
    if (status == AGENT_OK) *count = total;
    return status;
}

typedef struct {
    agent_session_jsonl_files_t* files;
    size_t length;
} selection_t;

static agent_error_t select_entry(void* context, agent_string_view_t name, bool* stop)
{
    selection_t* selection = context;
    if (!is_session(name)) return AGENT_OK;
    if (name.size >= selection->files->name_capacity) return AGENT_ERROR_CAPACITY;
    memcpy(selection->files->name, name.data, name.size);
    selection->files->name[name.size] = '\0';
    selection->length = name.size;
    *stop = true;
    return AGENT_OK;
}

static agent_error_t file_clear_all(void* context)
{
    agent_session_jsonl_files_t* files = context;
    for (;;) {
        selection_t selection = {files, 0u};
        agent_error_t status = agent_file_visit(&files->store, select_entry, &selection);
        if (status != AGENT_OK || !selection.length) return status;
        /* Close the enumeration before deletion; never reenter a backend from its callback. */
        status = agent_file_remove(&files->store, agent_string_view(files->name, selection.length));
        if (status != AGENT_OK) return status;
    }
}

agent_error_t agent_session_jsonl_files_init(agent_session_jsonl_files_t* files,
                                            const agent_file_store_t* store,
                                            char* name_buffer, size_t name_capacity,
                                            agent_session_jsonl_config_t* config)
{
    static const agent_session_jsonl_file_ops_t ops = {
        file_size, file_read, file_append, file_truncate, file_sync,
        file_remove, file_clear_all, file_count
    };
    const agent_file_store_ops_t* source;
    if (!files || !store || !store->ops || !config || !name_buffer ||
        overlaps(files, sizeof(*files), store, sizeof(*store)) ||
        overlaps(files, sizeof(*files), config, sizeof(*config)) ||
        overlaps(name_buffer, name_capacity, files, sizeof(*files)) ||
        overlaps(name_buffer, name_capacity, store, sizeof(*store)) ||
        overlaps(name_buffer, name_capacity, config, sizeof(*config))) return AGENT_ERROR_INVALID;
    if (name_capacity < sizeof(PREFIX) - 1u + 2u + sizeof(SUFFIX)) return AGENT_ERROR_CAPACITY;
    source = store->ops;
    if (!source->size || !source->read || !source->visit || !source->append ||
        !source->truncate || !source->sync || !source->remove) return AGENT_ERROR_NOT_SUPPORTED;
    files->store = *store;
    files->name = name_buffer;
    files->name_capacity = name_capacity;
    config->files = ops;
    config->file_context = files;
    return AGENT_OK;
}
