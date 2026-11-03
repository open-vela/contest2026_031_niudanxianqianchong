/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* Private bounded JSON codec; inputs, tokens, and outputs are caller-owned. */
#pragma once

#include <agent/types.h>

#define JSMN_HEADER
#include "jsmn.h"
#undef JSMN_HEADER

typedef struct {
    agent_string_view_t input;
    jsmntok_t* tokens;
    size_t token_count;
} agent_json_document_t;

typedef enum {
    AGENT_JSON_PATH_KEY,
    AGENT_JSON_PATH_INDEX
} agent_json_path_kind_t;

typedef struct {
    agent_json_path_kind_t kind;
    agent_string_view_t key;
    size_t index;
} agent_json_path_segment_t;

typedef struct {
    char* data;
    size_t capacity;
    size_t length;
    agent_error_t status;
} agent_json_writer_t;

/* max_depth counts nested objects/arrays and is capped at 32 for stack safety. */
agent_error_t agent_json_parse(agent_string_view_t input, jsmntok_t* tokens,
                               size_t token_capacity, size_t max_depth,
                               agent_json_document_t* document);
/* Strictly validate one complete object without allocating tokens. */
agent_error_t agent_json_validate_object(agent_string_view_t input, size_t max_depth);
/* Tokenless strict object admission; reject decoded duplicate/NUL keys at every depth. */
agent_error_t agent_json_validate_unique_object(agent_string_view_t input, size_t max_depth);
agent_error_t agent_json_object_get(const agent_json_document_t* document,
                                    size_t object, agent_string_view_t key,
                                    size_t* value);
agent_error_t agent_json_array_get(const agent_json_document_t* document,
                                   size_t array, size_t element, size_t* value);
agent_error_t agent_json_path_get(const agent_json_document_t* document,
                                  size_t root, const agent_json_path_segment_t* path,
                                  size_t count, size_t* value);
agent_error_t agent_json_decode_string(const agent_json_document_t* document,
                                       size_t token, char* output, size_t capacity,
                                       agent_string_view_t* decoded);
agent_error_t agent_json_raw_value(const agent_json_document_t* document,
                                   size_t token, agent_string_view_t* raw);

void agent_json_writer_init(agent_json_writer_t* writer, char* buffer, size_t capacity);
/* Literal accepts only trusted syntax, not application/model strings. */
agent_error_t agent_json_writer_literal(agent_json_writer_t* writer,
                                        agent_string_view_t literal);
agent_error_t agent_json_writer_string(agent_json_writer_t* writer,
                                       agent_string_view_t value);
agent_error_t agent_json_writer_u32(agent_json_writer_t* writer, uint32_t value);
agent_error_t agent_json_writer_bool(agent_json_writer_t* writer, bool value);
agent_error_t agent_json_writer_raw_value(agent_json_writer_t* writer,
                                          const agent_json_document_t* document,
                                          size_t token);
agent_error_t agent_json_writer_finish(agent_json_writer_t* writer,
                                       agent_string_view_t* output);

/* Shared UTF-8 check used by the strict reader and bounded writer. */
agent_error_t agent_json_utf8_width(const unsigned char* input, size_t available,
                                    size_t* width);
