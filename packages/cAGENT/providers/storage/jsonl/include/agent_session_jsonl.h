/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* Bounded JSONL Session Storage over application-supplied file operations. */
#pragma once

#include <agent/session.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Each session ID addresses one file; read() must fill exactly size bytes. */
typedef struct {
    agent_error_t (*size)(void* context, agent_string_view_t id, uint64_t* bytes);
    agent_error_t (*read)(void* context, agent_string_view_t id, uint64_t offset,
                          char* output, size_t size);
    /* append() may leave a partial tail on error; truncate() repairs it. */
    agent_error_t (*append)(void* context, agent_string_view_t id,
                            const char* data, size_t size);
    agent_error_t (*truncate)(void* context, agent_string_view_t id, uint64_t size);
    agent_error_t (*sync)(void* context, agent_string_view_t id);
    /* Failure may follow deletion or partial clearing; rollback is not required. */
    agent_error_t (*remove)(void* context, agent_string_view_t id);
    agent_error_t (*clear_all)(void* context);
    agent_error_t (*count)(void* context, size_t* count);
} agent_session_jsonl_file_ops_t;

/* Every buffer is borrowed, non-overlapping, and remains alive while bound. */
typedef struct {
    agent_session_jsonl_file_ops_t files;
    void* file_context;
    char* write_line;
    size_t write_line_capacity;
    char* read_line;
    size_t read_line_capacity;
    char* decoded;
    size_t decoded_capacity;
    void* token_buffer;       /* Aligned for int; private codec tokens live here. */
    size_t token_buffer_bytes;
    agent_message_view_t* read_messages;
    size_t read_message_capacity;
    agent_tool_call_view_t* read_calls;
    size_t read_call_capacity;
    char* session_id;
    size_t session_id_capacity;
} agent_session_jsonl_config_t;

typedef struct {
    agent_session_jsonl_config_t config;
    size_t write_length;
    size_t message_count;
    size_t call_count;
    size_t session_id_size;
    agent_message_role_t last_role;
    size_t last_tool_call_count;
    bool active;
} agent_session_jsonl_t;

/* No file is opened at init; file operations are synchronous and not thread-safe. */
agent_error_t agent_session_jsonl_init(agent_session_jsonl_t* jsonl,
                                       const agent_session_jsonl_config_t* config);

/* Copy the Session ops table; the JSONL state and buffers remain caller-owned. */
agent_error_t agent_session_jsonl_bind(agent_session_jsonl_t* jsonl,
                                       agent_session_storage_t* storage);

#ifdef __cplusplus
}
#endif
