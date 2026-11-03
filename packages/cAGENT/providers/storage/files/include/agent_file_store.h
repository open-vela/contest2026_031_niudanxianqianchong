/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* Optional synchronous byte-file access within one application-owned root. */
#pragma once

#include <agent/error.h>
#include <agent/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Names are borrowed during the callback; stop ends enumeration successfully. */
typedef agent_error_t (*agent_file_visit_fn)(void* context,
                                            agent_string_view_t name, bool* stop);

/* Missing callbacks denote unsupported capabilities; callbacks must not reenter a store. */
typedef struct {
    agent_error_t (*size)(void* context, agent_string_view_t name, uint64_t* bytes);
    /* Successful short reads are allowed; zero bytes denotes EOF. */
    agent_error_t (*read)(void* context, agent_string_view_t name, uint64_t offset,
                         void* output, size_t capacity, size_t* bytes_read);
    agent_error_t (*visit)(void* context, agent_file_visit_fn visitor, void* user_data);
    /* Failure may leave an appended prefix; the caller owns record recovery. */
    agent_error_t (*append)(void* context, agent_string_view_t name,
                           const void* data, size_t bytes);
    agent_error_t (*truncate)(void* context, agent_string_view_t name, uint64_t bytes);
    agent_error_t (*sync)(void* context, agent_string_view_t name);
    /* Failure may follow deletion when later synchronization fails. */
    agent_error_t (*remove)(void* context, agent_string_view_t name);
    /* published remains true if replacement succeeded but later synchronization failed. */
    agent_error_t (*replace)(void* context, agent_string_view_t name,
                            const void* data, size_t bytes, bool* published);
} agent_file_store_ops_t;

/* Immutable ops/context are borrowed; caller serializes access and keeps buffers disjoint. */
typedef struct {
    const agent_file_store_ops_t* ops;
    void* context;
} agent_file_store_t;

/* Names are single components: no NUL, slash, backslash, dot entries or .cagent- prefix. */
agent_error_t agent_file_size(const agent_file_store_t* store,
                              agent_string_view_t name, uint64_t* bytes);
/* Output is unspecified on failure; bytes_read is zero. */
agent_error_t agent_file_read(const agent_file_store_t* store, agent_string_view_t name,
                              uint64_t offset, void* output, size_t capacity,
                              size_t* bytes_read);
/* Unexpected EOF is IO; no allocation and no silent truncation. */
agent_error_t agent_file_read_exact(const agent_file_store_t* store,
                                    agent_string_view_t name, uint64_t offset,
                                    void* output, size_t bytes);
/* Bounded whole-file read, not a snapshot; caller excludes concurrent writes/replaces. */
agent_error_t agent_file_read_all(const agent_file_store_t* store,
                                  agent_string_view_t name, void* output,
                                  size_t capacity, size_t max_bytes, size_t* bytes_read);
/* NUL-terminate byte text, reject embedded NUL; UTF-8/format validation belongs to the consumer. */
agent_error_t agent_file_read_text(const agent_file_store_t* store,
                                   agent_string_view_t name, char* output,
                                   size_t capacity, size_t max_bytes,
                                   agent_string_view_t* text);
/* Unordered regular-file names; visitor errors stop the operation unchanged. */
agent_error_t agent_file_visit(const agent_file_store_t* store,
                               agent_file_visit_fn visitor, void* context);
agent_error_t agent_file_append(const agent_file_store_t* store,
                                agent_string_view_t name, const void* data, size_t bytes);
agent_error_t agent_file_truncate(const agent_file_store_t* store,
                                  agent_string_view_t name, uint64_t bytes);
/* The backend documents whether directory metadata is also synchronized. */
agent_error_t agent_file_sync(const agent_file_store_t* store, agent_string_view_t name);
/* An error does not guarantee the file still exists; no deletion rollback. */
agent_error_t agent_file_remove(const agent_file_store_t* store, agent_string_view_t name);
/* Do not retry blindly after failure with published=true. */
agent_error_t agent_file_replace(const agent_file_store_t* store,
                                 agent_string_view_t name, const void* data,
                                 size_t bytes, bool* published);

#ifdef __cplusplus
}
#endif
