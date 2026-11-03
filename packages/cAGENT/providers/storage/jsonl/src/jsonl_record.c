/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* Stable version-1 JSONL turn encoding and bounded decoding. */

#include "jsonl_internal.h"

#include <string.h>

static agent_error_t syntax(agent_json_writer_t* writer, const char* value)
{
    return agent_json_writer_literal(writer, agent_string_view(value, strlen(value)));
}

agent_error_t agent_jsonl_encode_message(agent_json_writer_t* writer,
                                          const agent_message_view_t* message)
{
    size_t i;
    agent_error_t status;

    if (!writer || !message || message->role < AGENT_MESSAGE_ROLE_SYSTEM ||
        message->role > AGENT_MESSAGE_ROLE_TOOL ||
        (message->tool_call_count && !message->tool_calls)) return AGENT_ERROR_INVALID;
    status = syntax(writer, "{\"r\":");
    if (status == AGENT_OK) status = agent_json_writer_u32(writer, (uint32_t)message->role);
    if (status == AGENT_OK) status = syntax(writer, ",\"c\":");
    if (status == AGENT_OK) status = agent_json_writer_string(writer, message->content);
    if (status == AGENT_OK) status = syntax(writer, ",\"tid\":");
    if (status == AGENT_OK) status = agent_json_writer_string(writer, message->tool_call_id);
    if (status == AGENT_OK) status = syntax(writer, ",\"tc\":[");
    for (i = 0u; status == AGENT_OK && i < message->tool_call_count; ++i) {
        const agent_tool_call_view_t* call = &message->tool_calls[i];

        if (i) status = syntax(writer, ",");
        if (status == AGENT_OK) status = syntax(writer, "{\"id\":");
        if (status == AGENT_OK) status = agent_json_writer_string(writer, call->id);
        if (status == AGENT_OK) status = syntax(writer, ",\"name\":");
        if (status == AGENT_OK) status = agent_json_writer_string(writer, call->name);
        if (status == AGENT_OK) status = syntax(writer, ",\"a\":");
        if (status == AGENT_OK)
            status = agent_json_validate_object(call->arguments_json, 16u);
        if (status == AGENT_OK)
            status = agent_json_writer_string(writer, call->arguments_json);
        if (status == AGENT_OK) status = syntax(writer, "}");
    }
    if (status == AGENT_OK) status = syntax(writer, "]}");
    return status;
}

static agent_error_t field(const agent_json_document_t* doc, size_t object,
                           const char* key, size_t* value)
{
    agent_error_t status = agent_json_object_get(doc, object,
                                                 agent_string_view(key, strlen(key)), value);
    return status == AGENT_ERROR_NOT_FOUND ? AGENT_ERROR_PARSE : status;
}

static agent_error_t digit(const agent_json_document_t* doc, size_t object,
                           const char* key, uint32_t* value)
{
    agent_string_view_t raw;
    size_t token;
    agent_error_t status = field(doc, object, key, &token);

    if (status != AGENT_OK) return status;
    if (doc->tokens[token].type != JSMN_PRIMITIVE) return AGENT_ERROR_PARSE;
    status = agent_json_raw_value(doc, token, &raw);
    if (status != AGENT_OK) return status;
    if (raw.size != 1u || raw.data[0] < '0' || raw.data[0] > '9')
        return AGENT_ERROR_PARSE;
    *value = (uint32_t)(raw.data[0] - '0');
    return AGENT_OK;
}

static agent_error_t string_field(agent_session_jsonl_t* jsonl,
                                  const agent_json_document_t* doc, size_t object,
                                  const char* key, size_t* used,
                                  agent_string_view_t* view)
{
    size_t token;
    agent_error_t status = field(doc, object, key, &token);

    if (status != AGENT_OK) return status;
    status = agent_json_decode_string(doc, token,
                                      jsonl->config.decoded + *used,
                                      jsonl->config.decoded_capacity - *used, view);
    if (status == AGENT_ERROR_INVALID) return AGENT_ERROR_PARSE;
    if (status == AGENT_OK) *used += view->size;
    return status;
}

static agent_error_t decode_message(agent_session_jsonl_t* jsonl,
                                    const agent_json_document_t* doc, size_t token,
                                    size_t* used, size_t* call_count,
                                    agent_message_view_t* message)
{
    uint32_t role;
    size_t calls;
    size_t index;
    agent_error_t status;

    if (doc->tokens[token].type != JSMN_OBJECT) return AGENT_ERROR_PARSE;
    memset(message, 0, sizeof(*message));
    status = digit(doc, token, "r", &role);
    if (status != AGENT_OK || role < AGENT_MESSAGE_ROLE_SYSTEM ||
        role > AGENT_MESSAGE_ROLE_TOOL) return AGENT_ERROR_PARSE;
    message->role = (agent_message_role_t)role;
    status = string_field(jsonl, doc, token, "c", used, &message->content);
    if (status != AGENT_OK) return status;
    status = string_field(jsonl, doc, token, "tid", used, &message->tool_call_id);
    if (status != AGENT_OK) return status;
    status = field(doc, token, "tc", &calls);
    if (status != AGENT_OK) return status;
    if (doc->tokens[calls].type != JSMN_ARRAY) return AGENT_ERROR_PARSE;
    for (index = 0u;; ++index) {
        agent_tool_call_view_t* call;
        size_t item;

        status = agent_json_array_get(doc, calls, index, &item);
        if (status == AGENT_ERROR_NOT_FOUND) break;
        if (status != AGENT_OK || doc->tokens[item].type != JSMN_OBJECT)
            return AGENT_ERROR_PARSE;
        if (*call_count >= jsonl->config.read_call_capacity) return AGENT_ERROR_CAPACITY;
        call = &jsonl->config.read_calls[(*call_count)++];
        if (message->tool_call_count == 0u) message->tool_calls = call;
        status = string_field(jsonl, doc, item, "id", used, &call->id);
        if (status != AGENT_OK) return status;
        status = string_field(jsonl, doc, item, "name", used, &call->name);
        if (status != AGENT_OK) return status;
        status = string_field(jsonl, doc, item, "a", used, &call->arguments_json);
        if (status != AGENT_OK) return status;
        status = agent_json_validate_object(call->arguments_json, 16u);
        if (status != AGENT_OK) return status;
        ++message->tool_call_count;
    }
    return AGENT_OK;
}

agent_error_t agent_jsonl_decode_line(agent_session_jsonl_t* jsonl,
                                      agent_string_view_t line,
                                      agent_string_view_t expected_id,
                                      bool* complete,
                                      agent_session_group_view_t* group)
{
    agent_json_document_t doc;
    agent_string_view_t id;
    size_t messages;
    size_t used = 0u;
    size_t call_count = 0u;
    size_t count = 0u;
    uint32_t version;
    uint32_t outcome;
    agent_error_t status;

    if (!jsonl || !complete || !group) return AGENT_ERROR_INVALID;
    *complete = false;
    memset(group, 0, sizeof(*group));
    status = agent_json_parse(line, jsonl->config.token_buffer,
                              jsonl->config.token_buffer_bytes / sizeof(jsmntok_t),
                              16u, &doc);
    if (status != AGENT_OK) return status;
    if (doc.tokens[0].type != JSMN_OBJECT) return AGENT_ERROR_PARSE;
    status = digit(&doc, 0u, "v", &version);
    if (status != AGENT_OK || version != 1u) return AGENT_ERROR_PARSE;
    status = string_field(jsonl, &doc, 0u, "sid", &used, &id);
    if (status != AGENT_OK) return status;
    if (id.size != expected_id.size || memcmp(id.data, expected_id.data, id.size))
        return AGENT_ERROR_PARSE;
    used = 0u;
    status = digit(&doc, 0u, "o", &outcome);
    if (status != AGENT_OK || (outcome != AGENT_SESSION_TURN_COMPLETE &&
                               outcome != AGENT_SESSION_TURN_ABORTED))
        return AGENT_ERROR_PARSE;
    status = field(&doc, 0u, "m", &messages);
    if (status != AGENT_OK) return status;
    if (doc.tokens[messages].type != JSMN_ARRAY) return AGENT_ERROR_PARSE;
    if (outcome == AGENT_SESSION_TURN_ABORTED) return AGENT_OK;
    for (;;) {
        size_t item;

        status = agent_json_array_get(&doc, messages, count, &item);
        if (status == AGENT_ERROR_NOT_FOUND) break;
        if (status != AGENT_OK) return status;
        if (count >= jsonl->config.read_message_capacity) return AGENT_ERROR_CAPACITY;
        status = decode_message(jsonl, &doc, item, &used, &call_count,
                                &jsonl->config.read_messages[count]);
        if (status != AGENT_OK) return status;
        ++count;
    }
    group->messages = jsonl->config.read_messages;
    group->message_count = count;
    *complete = true;
    return AGENT_OK;
}
