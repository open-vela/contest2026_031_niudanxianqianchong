/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* Complete-turn JSONL transaction and reverse, bounded history scan. */

#include "jsonl_internal.h"

#include <stdint.h>
#include <string.h>

typedef struct {
    char byte;
    jsmntok_t token;
} token_alignment_t;

typedef struct {
    const void* data;
    size_t bytes;
} region_t;

static bool regions_overlap(const region_t* left, const region_t* right)
{
    uintptr_t a = (uintptr_t)left->data;
    uintptr_t b = (uintptr_t)right->data;

    if (!left->bytes || !right->bytes) return false;
    if (left->bytes > UINTPTR_MAX - a || right->bytes > UINTPTR_MAX - b)
        return true;
    return a < b + right->bytes && b < a + left->bytes;
}

static agent_string_view_t current_id(const agent_session_jsonl_t* jsonl)
{
    return agent_string_view(jsonl->config.session_id, jsonl->session_id_size);
}

static agent_error_t file_size(agent_session_jsonl_t* jsonl,
                               agent_string_view_t id, uint64_t* bytes)
{
    agent_error_t status = jsonl->config.files.size(jsonl->config.file_context, id, bytes);

    if (status == AGENT_ERROR_NOT_FOUND) {
        *bytes = 0u;
        return AGENT_OK;
    }
    return status;
}

/* Return the offset just after the last newline in [0, before), or zero. */
static agent_error_t last_line_boundary(agent_session_jsonl_t* jsonl,
                                        agent_string_view_t id, uint64_t before,
                                        uint64_t* boundary)
{
    char chunk[128];
    uint64_t end = before;

    *boundary = 0u;
    while (end) {
        size_t count = end < sizeof(chunk) ? (size_t)end : sizeof(chunk);
        uint64_t start = end - count;
        size_t i;
        agent_error_t status = jsonl->config.files.read(jsonl->config.file_context,
                                                         id, start, chunk, count);
        if (status != AGENT_OK) return status;
        for (i = count; i > 0u; --i) {
            if (chunk[i - 1u] == '\n') {
                *boundary = start + i;
                return AGENT_OK;
            }
        }
        end = start;
    }
    return AGENT_OK;
}

static agent_error_t committed_end(agent_session_jsonl_t* jsonl,
                                   agent_string_view_t id, bool repair,
                                   uint64_t* end)
{
    char last;
    agent_error_t status = file_size(jsonl, id, end);

    if (status != AGENT_OK || *end == 0u) return status;
    status = jsonl->config.files.read(jsonl->config.file_context, id,
                                      *end - 1u, &last, 1u);
    if (status != AGENT_OK || last == '\n') return status;
    status = last_line_boundary(jsonl, id, *end, end);
    if (status != AGENT_OK || !repair) return status;
    status = jsonl->config.files.truncate(jsonl->config.file_context, id, *end);
    if (status != AGENT_OK) return status;
    return jsonl->config.files.sync(jsonl->config.file_context, id);
}

static agent_json_writer_t resume_writer(agent_session_jsonl_t* jsonl)
{
    agent_json_writer_t writer;

    writer.data = jsonl->config.write_line;
    writer.capacity = jsonl->config.write_line_capacity;
    writer.length = jsonl->write_length;
    writer.status = AGENT_OK;
    return writer;
}

static agent_error_t jsonl_begin(void* context, agent_string_view_t id,
                                 void** transaction)
{
    agent_session_jsonl_t* jsonl = context;
    agent_json_writer_t writer;
    uint64_t end;
    agent_error_t status;

    if (!jsonl || !transaction || !id.data || id.size == 0u) return AGENT_ERROR_INVALID;
    *transaction = NULL;
    if (jsonl->active) return AGENT_ERROR_BUSY;
    if (id.size > jsonl->config.session_id_capacity) return AGENT_ERROR_CAPACITY;
    memcpy(jsonl->config.session_id, id.data, id.size);
    jsonl->session_id_size = id.size;
    status = committed_end(jsonl, current_id(jsonl), true, &end);
    if (status != AGENT_OK) return status;
    jsonl->message_count = 0u;
    jsonl->call_count = 0u;
    jsonl->last_role = 0;
    jsonl->last_tool_call_count = 0u;
    agent_json_writer_init(&writer, jsonl->config.write_line,
                           jsonl->config.write_line_capacity);
    status = agent_json_writer_literal(&writer,
                                       agent_string_view("{\"v\":1,\"sid\":",
                                                         sizeof("{\"v\":1,\"sid\":") - 1u));
    if (status == AGENT_OK) status = agent_json_writer_string(&writer, id);
    if (status == AGENT_OK)
        status = agent_json_writer_literal(&writer, agent_string_view(",\"m\":[",
                                                                  sizeof(",\"m\":[") - 1u));
    if (status != AGENT_OK) return status;
    jsonl->write_length = writer.length;
    jsonl->active = true;
    *transaction = jsonl;
    return AGENT_OK;
}

static agent_error_t jsonl_append(void* context, void* transaction,
                                  const agent_message_view_t* message)
{
    agent_session_jsonl_t* jsonl = context;
    agent_json_writer_t writer;
    agent_error_t status;

    if (!jsonl || transaction != jsonl || !jsonl->active || !message)
        return AGENT_ERROR_INVALID;
    if (jsonl->message_count >= jsonl->config.read_message_capacity ||
        message->tool_call_count > jsonl->config.read_call_capacity - jsonl->call_count)
        return AGENT_ERROR_CAPACITY;
    writer = resume_writer(jsonl);
    writer.capacity -= sizeof("],\"o\":1}\n") - 1u;
    if (jsonl->message_count)
        status = agent_json_writer_literal(&writer, agent_string_view(",", 1u));
    else
        status = AGENT_OK;
    if (status == AGENT_OK) status = agent_jsonl_encode_message(&writer, message);
    if (status != AGENT_OK) {
        jsonl->config.write_line[jsonl->write_length] = '\0';
        return status;
    }
    jsonl->write_length = writer.length;
    ++jsonl->message_count;
    jsonl->call_count += message->tool_call_count;
    jsonl->last_role = message->role;
    jsonl->last_tool_call_count = message->tool_call_count;
    return AGENT_OK;
}

static agent_error_t jsonl_finish(void* context, void* transaction,
                                  agent_session_turn_outcome_t outcome)
{
    agent_session_jsonl_t* jsonl = context;
    agent_json_writer_t writer;
    agent_string_view_t line;
    uint64_t end;
    agent_error_t completion_status = AGENT_OK;
    agent_error_t status;

    if (!jsonl || transaction != jsonl || !jsonl->active) return AGENT_ERROR_INVALID;
    jsonl->active = false;
    if (outcome != AGENT_SESSION_TURN_COMPLETE && outcome != AGENT_SESSION_TURN_ABORTED)
        return AGENT_ERROR_INVALID;
    if (outcome == AGENT_SESSION_TURN_COMPLETE &&
        (jsonl->message_count < 2u ||
         jsonl->last_role != AGENT_MESSAGE_ROLE_ASSISTANT ||
         jsonl->last_tool_call_count != 0u)) {
        outcome = AGENT_SESSION_TURN_ABORTED;
        completion_status = AGENT_ERROR_STATE;
    }
    writer = resume_writer(jsonl);
    status = agent_json_writer_literal(&writer,
               agent_string_view(outcome == AGENT_SESSION_TURN_COMPLETE ?
                                 "],\"o\":1}\n" : "],\"o\":2}\n",
                                 sizeof("],\"o\":1}\n") - 1u));
    if (status == AGENT_OK) status = agent_json_writer_finish(&writer, &line);
    if (status != AGENT_OK) return status;
    status = committed_end(jsonl, current_id(jsonl), true, &end);
    if (status != AGENT_OK) return status;
    status = jsonl->config.files.append(jsonl->config.file_context,
                                        current_id(jsonl), line.data, line.size);
    if (status != AGENT_OK) {
        agent_error_t rollback = jsonl->config.files.truncate(
            jsonl->config.file_context, current_id(jsonl), end);
        return rollback == AGENT_OK ? status : rollback;
    }
    /* sync failure is ambiguous: the complete line may already be durable. */
    status = jsonl->config.files.sync(jsonl->config.file_context, current_id(jsonl));
    return status == AGENT_OK ? completion_status : status;
}

static agent_error_t jsonl_recent(void* context, agent_string_view_t id,
                                  size_t max_candidates, agent_session_visit_fn visit,
                                  void* visit_context)
{
    agent_session_jsonl_t* jsonl = context;
    uint64_t end;
    size_t seen = 0u;
    agent_error_t status;

    if (!jsonl || !id.data || id.size == 0u || !visit) return AGENT_ERROR_INVALID;
    if (regions_overlap(&(region_t){id.data, id.size},
                        &(region_t){jsonl->config.read_line,
                                    jsonl->config.read_line_capacity}) ||
        regions_overlap(&(region_t){id.data, id.size},
                        &(region_t){jsonl->config.decoded,
                                    jsonl->config.decoded_capacity}))
        return AGENT_ERROR_INVALID;
    status = committed_end(jsonl, id, false, &end);
    if (status != AGENT_OK) return status;
    while (end && seen < max_candidates) {
        agent_session_group_view_t group;
        uint64_t start;
        uint64_t length;
        bool complete;

        status = last_line_boundary(jsonl, id, end - 1u, &start);
        if (status != AGENT_OK) return status;
        length = end - start - 1u;
        if (length == 0u) return AGENT_ERROR_PARSE;
        if (length >= jsonl->config.read_line_capacity) return AGENT_ERROR_CAPACITY;
        status = jsonl->config.files.read(jsonl->config.file_context, id, start,
                                          jsonl->config.read_line, (size_t)length);
        if (status != AGENT_OK) return status;
        jsonl->config.read_line[length] = '\0';
        status = agent_jsonl_decode_line(jsonl,
                                         agent_string_view(jsonl->config.read_line,
                                                           (size_t)length),
                                         id, &complete, &group);
        if (status != AGENT_OK) return status;
        if (complete) {
            ++seen;
            status = visit(visit_context, &group);
            if (status != AGENT_OK) return status;
        }
        end = start;
    }
    return AGENT_OK;
}

static agent_error_t jsonl_clear(void* context, agent_string_view_t id)
{
    agent_session_jsonl_t* jsonl = context;
    agent_error_t status;

    if (!jsonl || !id.data || id.size == 0u) return AGENT_ERROR_INVALID;
    if (jsonl->active) return AGENT_ERROR_BUSY;
    status = jsonl->config.files.truncate(jsonl->config.file_context, id, 0u);
    if (status != AGENT_OK) return status;
    return jsonl->config.files.sync(jsonl->config.file_context, id);
}

static agent_error_t jsonl_clear_all(void* context)
{
    agent_session_jsonl_t* jsonl = context;

    if (!jsonl) return AGENT_ERROR_INVALID;
    if (jsonl->active) return AGENT_ERROR_BUSY;
    return jsonl->config.files.clear_all(jsonl->config.file_context);
}

static agent_error_t jsonl_remove(void* context, agent_string_view_t id)
{
    agent_session_jsonl_t* jsonl = context;

    if (!jsonl || !id.data || id.size == 0u) return AGENT_ERROR_INVALID;
    if (jsonl->active) return AGENT_ERROR_BUSY;
    return jsonl->config.files.remove(jsonl->config.file_context, id);
}

static agent_error_t jsonl_count(void* context, size_t* count)
{
    agent_session_jsonl_t* jsonl = context;

    if (!jsonl || !count) return AGENT_ERROR_INVALID;
    return jsonl->config.files.count(jsonl->config.file_context, count);
}

agent_error_t agent_session_jsonl_init(agent_session_jsonl_t* jsonl,
                                       const agent_session_jsonl_config_t* config)
{
    region_t regions[7];
    size_t i;
    size_t j;

    if (!jsonl || !config || !config->files.size || !config->files.read ||
        !config->files.append || !config->files.truncate || !config->files.sync ||
        !config->files.remove || !config->files.clear_all || !config->files.count ||
        !config->write_line || config->write_line_capacity < 32u ||
        !config->read_line || config->read_line_capacity < 32u ||
        !config->decoded || !config->decoded_capacity ||
        !config->token_buffer ||
        config->token_buffer_bytes < 16u * sizeof(jsmntok_t) ||
        (uintptr_t)config->token_buffer % offsetof(token_alignment_t, token) != 0u ||
        !config->read_messages || !config->read_message_capacity ||
        (config->read_call_capacity && !config->read_calls) ||
        !config->session_id || !config->session_id_capacity ||
        config->read_message_capacity > SIZE_MAX / sizeof(agent_message_view_t) ||
        config->read_call_capacity > SIZE_MAX / sizeof(agent_tool_call_view_t))
        return AGENT_ERROR_INVALID;
    regions[0] = (region_t){config->write_line, config->write_line_capacity};
    regions[1] = (region_t){config->read_line, config->read_line_capacity};
    regions[2] = (region_t){config->decoded, config->decoded_capacity};
    regions[3] = (region_t){config->token_buffer, config->token_buffer_bytes};
    regions[4] = (region_t){config->read_messages,
                             config->read_message_capacity * sizeof(agent_message_view_t)};
    regions[5] = (region_t){config->read_calls,
                             config->read_call_capacity * sizeof(agent_tool_call_view_t)};
    regions[6] = (region_t){config->session_id, config->session_id_capacity};
    for (i = 0u; i < 7u; ++i) {
        for (j = i + 1u; j < 7u; ++j) {
            if (regions_overlap(&regions[i], &regions[j])) return AGENT_ERROR_INVALID;
        }
    }
    memset(jsonl, 0, sizeof(*jsonl));
    jsonl->config = *config;
    return AGENT_OK;
}

agent_error_t agent_session_jsonl_bind(agent_session_jsonl_t* jsonl,
                                       agent_session_storage_t* storage)
{
    static const agent_session_storage_ops_t ops = {
        jsonl_begin, jsonl_append, jsonl_finish, jsonl_recent,
        jsonl_clear, jsonl_clear_all, jsonl_remove, jsonl_count
    };

    if (!jsonl || !storage || !jsonl->config.write_line) return AGENT_ERROR_INVALID;
    storage->ops = ops;
    storage->context = jsonl;
    return AGENT_OK;
}
