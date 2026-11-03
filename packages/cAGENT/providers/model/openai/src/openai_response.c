/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* Complete response validation precedes any Model sink delivery. */

#include "openai_internal.h"

#include <string.h>

static agent_error_t required_field(const agent_json_document_t* document,
                                    size_t object, agent_string_view_t key, size_t* value)
{
    return agent_json_object_get(document, object, key, value) == AGENT_OK ?
           AGENT_OK : AGENT_ERROR_MODEL_PARSE;
}

static agent_error_t decoded_string(agent_openai_provider_t* provider,
                                    const agent_json_document_t* document,
                                    size_t token, size_t* used,
                                    agent_string_view_t* value)
{
    agent_error_t status;

    if (*used > provider->config.decoded_capacity) {
        return AGENT_ERROR_CAPACITY;
    }
    status = agent_json_decode_string(document, token,
                                      provider->config.decoded_buffer + *used,
                                      provider->config.decoded_capacity - *used, value);
    if (status != AGENT_OK) {
        return status == AGENT_ERROR_CAPACITY ? status : AGENT_ERROR_MODEL_PARSE;
    }
    *used += value->size;
    return AGENT_OK;
}

static bool equals(agent_string_view_t value, agent_string_view_t literal)
{
    return value.size == literal.size &&
           memcmp(value.data, literal.data, value.size) == 0;
}

static bool is_null(const agent_json_document_t* document, size_t token)
{
    jsmntok_t* item = &document->tokens[token];

    return item->type == JSMN_PRIMITIVE && item->end - item->start == 4 &&
           memcmp(document->input.data + item->start, "null", 4u) == 0;
}

static agent_error_t inspect_call(agent_openai_provider_t* provider,
                                  const agent_json_document_t* document,
                                  size_t token, const agent_model_sink_t* sink,
                                  bool deliver)
{
    agent_tool_call_view_t call;
    agent_string_view_t type;
    size_t field;
    size_t function;
    size_t used = 0u;
    agent_error_t status;

    if (document->tokens[token].type != JSMN_OBJECT) {
        return AGENT_ERROR_MODEL_PARSE;
    }
    status = required_field(document, token, OPENAI_LITERAL("id"), &field);
    if (status == AGENT_OK) {
        status = decoded_string(provider, document, field, &used, &call.id);
    }
    if (status != AGENT_OK) return status;
    if (call.id.size == 0u) return AGENT_ERROR_MODEL_PARSE;
    if (call.id.size > AGENT_MAX_IDENTIFIER_BYTES) return AGENT_ERROR_LIMIT;
    status = required_field(document, token, OPENAI_LITERAL("type"), &field);
    if (status == AGENT_OK) {
        status = decoded_string(provider, document, field, &used, &type);
    }
    if (status != AGENT_OK) return status;
    if (!equals(type, OPENAI_LITERAL("function"))) {
        return AGENT_ERROR_MODEL_PARSE;
    }
    status = required_field(document, token, OPENAI_LITERAL("function"), &function);
    if (status != AGENT_OK || document->tokens[function].type != JSMN_OBJECT) {
        return AGENT_ERROR_MODEL_PARSE;
    }
    status = required_field(document, function, OPENAI_LITERAL("name"), &field);
    if (status == AGENT_OK) {
        status = decoded_string(provider, document, field, &used, &call.name);
    }
    if (status != AGENT_OK) return status;
    if (call.name.size > AGENT_MAX_NAME_BYTES) return AGENT_ERROR_LIMIT;
    if (!agent_openai_valid_tool_name(call.name)) return AGENT_ERROR_MODEL_PARSE;
    status = required_field(document, function, OPENAI_LITERAL("arguments"), &field);
    if (status == AGENT_OK) {
        status = decoded_string(provider, document, field, &used, &call.arguments_json);
    }
    if (status != AGENT_OK) return status;
    if (call.arguments_json.size > AGENT_MAX_ARGUMENTS_BYTES) return AGENT_ERROR_LIMIT;
    status = agent_json_validate_object(call.arguments_json,
                                        provider->config.max_json_depth);
    if (status != AGENT_OK) {
        return status == AGENT_ERROR_LIMIT ? status : AGENT_ERROR_TOOL_ARGUMENT;
    }
    return deliver ? sink->tool_call(sink->context, &call) : AGENT_OK;
}

static agent_error_t inspect_message(agent_openai_provider_t* provider,
                                     const agent_json_document_t* document,
                                     size_t message, const agent_model_sink_t* sink,
                                     bool deliver, size_t* call_count,
                                     bool* has_output)
{
    agent_string_view_t role;
    agent_string_view_t content;
    size_t field;
    size_t calls;
    size_t i;
    size_t used = 0u;
    agent_error_t status;

    *call_count = 0u;
    *has_output = false;
    if (document->tokens[message].type != JSMN_OBJECT) {
        return AGENT_ERROR_MODEL_PARSE;
    }
    status = required_field(document, message, OPENAI_LITERAL("role"), &field);
    if (status == AGENT_OK) {
        status = decoded_string(provider, document, field, &used, &role);
    }
    if (status != AGENT_OK) return status;
    if (!equals(role, OPENAI_LITERAL("assistant"))) {
        return AGENT_ERROR_MODEL_PARSE;
    }
    status = agent_json_object_get(document, message, OPENAI_LITERAL("content"), &field);
    if (status != AGENT_OK && status != AGENT_ERROR_NOT_FOUND) {
        return AGENT_ERROR_MODEL_PARSE;
    }
    if (status == AGENT_OK && !is_null(document, field)) {
        used = 0u;
        status = decoded_string(provider, document, field, &used, &content);
        if (status != AGENT_OK) return status;
        *has_output = true;
    }
    if (!*has_output || content.size == 0u) {
        status = agent_json_object_get(document, message, OPENAI_LITERAL("refusal"), &field);
        if (status != AGENT_OK && status != AGENT_ERROR_NOT_FOUND) {
            return AGENT_ERROR_MODEL_PARSE;
        }
        if (status == AGENT_OK && !is_null(document, field)) {
            used = 0u;
            status = decoded_string(provider, document, field, &used, &content);
            if (status != AGENT_OK) return status;
            *has_output = true;
        }
    }
    if (*has_output) {
        if (content.size > AGENT_MAX_MODEL_OUTPUT_BYTES) return AGENT_ERROR_LIMIT;
        if (deliver && content.size != 0u) {
            status = sink->text(sink->context, content);
            if (status != AGENT_OK) return status;
        }
    }
    status = agent_json_object_get(document, message, OPENAI_LITERAL("tool_calls"), &calls);
    if (status == AGENT_ERROR_NOT_FOUND) return AGENT_OK;
    if (status == AGENT_OK && is_null(document, calls)) return AGENT_OK;
    if (status != AGENT_OK || document->tokens[calls].type != JSMN_ARRAY) {
        return AGENT_ERROR_MODEL_PARSE;
    }
    for (i = 0u;; ++i) {
        status = agent_json_array_get(document, calls, i, &field);
        if (status == AGENT_ERROR_NOT_FOUND) break;
        if (status != AGENT_OK) return AGENT_ERROR_MODEL_PARSE;
        if (i >= AGENT_MAX_MODEL_TOOL_CALLS) return AGENT_ERROR_LIMIT;
        status = inspect_call(provider, document, field, sink, deliver);
        if (status != AGENT_OK) return status;
    }
    *call_count = i;
    return AGENT_OK;
}

agent_error_t agent_openai_read_response(agent_openai_provider_t* provider,
                                         size_t size, const agent_model_sink_t* sink)
{
    agent_json_document_t document;
    agent_string_view_t finish;
    size_t choices;
    size_t choice;
    size_t message;
    size_t field;
    size_t used = 0u;
    size_t call_count;
    bool has_output;
    bool tool_finish;
    agent_error_t status;

    if (size == 0u) return AGENT_ERROR_MODEL_PARSE;
    status = agent_json_parse(agent_string_view(provider->config.response_buffer, size),
                              agent_openai_token_buffer(provider),
                              provider->config.response_token_capacity,
                              provider->config.max_json_depth, &document);
    if (status != AGENT_OK) {
        return status == AGENT_ERROR_CAPACITY ? status : AGENT_ERROR_MODEL_PARSE;
    }
    if (document.tokens[0].type != JSMN_OBJECT) return AGENT_ERROR_MODEL_PARSE;
    status = required_field(&document, 0u, OPENAI_LITERAL("choices"), &choices);
    if (status != AGENT_OK || document.tokens[choices].type != JSMN_ARRAY ||
        agent_json_array_get(&document, choices, 0u, &choice) != AGENT_OK ||
        document.tokens[choice].type != JSMN_OBJECT) {
        return AGENT_ERROR_MODEL_PARSE;
    }
    status = required_field(&document, choice, OPENAI_LITERAL("finish_reason"), &field);
    if (status == AGENT_OK) {
        status = decoded_string(provider, &document, field, &used, &finish);
    }
    if (status != AGENT_OK) return status;
    if (equals(finish, OPENAI_LITERAL("length"))) return AGENT_ERROR_LIMIT;
    if (equals(finish, OPENAI_LITERAL("content_filter"))) return AGENT_ERROR_MODEL_FAILED;
    if (!equals(finish, OPENAI_LITERAL("stop")) &&
        !equals(finish, OPENAI_LITERAL("tool_calls"))) {
        return AGENT_ERROR_MODEL_FAILED;
    }
    tool_finish = equals(finish, OPENAI_LITERAL("tool_calls"));
    status = required_field(&document, choice, OPENAI_LITERAL("message"), &message);
    if (status != AGENT_OK) return status;
    status = inspect_message(provider, &document, message, sink, false,
                             &call_count, &has_output);
    if (status != AGENT_OK) return status;
    if ((call_count == 0u && !has_output) ||
        (call_count != 0u) != tool_finish) {
        return AGENT_ERROR_MODEL_PARSE;
    }
    return inspect_message(provider, &document, message, sink, true,
                           &call_count, &has_output);
}
