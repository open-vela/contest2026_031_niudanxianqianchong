/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* Canonical Model request to bounded Chat Completions JSON. */

#include "openai_internal.h"

static agent_error_t input_status(agent_error_t status)
{
    return status == AGENT_ERROR_PARSE ? AGENT_ERROR_INVALID : status;
}

static agent_error_t append(agent_json_writer_t* writer, agent_string_view_t syntax)
{
    return agent_json_writer_literal(writer, syntax);
}

bool agent_openai_valid_tool_name(agent_string_view_t name)
{
    size_t i;

    if (name.data == NULL || name.size == 0u || name.size > 64u ||
        name.size > AGENT_MAX_NAME_BYTES) {
        return false;
    }
    for (i = 0u; i < name.size; ++i) {
        char c = name.data[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

static agent_error_t write_string(agent_json_writer_t* writer, agent_string_view_t value)
{
    return input_status(agent_json_writer_string(writer, value));
}

static agent_error_t write_history_calls(agent_openai_provider_t* provider,
                                         agent_json_writer_t* writer,
                                         const agent_message_view_t* message)
{
    size_t i;
    agent_error_t status;

    if (message->tool_call_count > AGENT_MAX_MODEL_TOOL_CALLS ||
        (message->tool_call_count != 0u && message->tool_calls == NULL)) {
        return AGENT_ERROR_LIMIT;
    }
    status = append(writer, OPENAI_LITERAL(",\"tool_calls\":["));
    for (i = 0u; status == AGENT_OK && i < message->tool_call_count; ++i) {
        const agent_tool_call_view_t* call = &message->tool_calls[i];
        if (call->id.data == NULL || call->id.size == 0u ||
            (call->arguments_json.size != 0u && call->arguments_json.data == NULL)) {
            return AGENT_ERROR_INVALID;
        }
        if (call->id.size > AGENT_MAX_IDENTIFIER_BYTES ||
            call->arguments_json.size > AGENT_MAX_ARGUMENTS_BYTES ||
            call->name.size > AGENT_MAX_NAME_BYTES) {
            return AGENT_ERROR_LIMIT;
        }
        if (!agent_openai_valid_tool_name(call->name)) return AGENT_ERROR_INVALID;
        status = agent_json_validate_object(call->arguments_json,
                                             provider->config.max_json_depth);
        if (status != AGENT_OK) {
            return input_status(status);
        }
        if (i != 0u) {
            status = append(writer, OPENAI_LITERAL(","));
        }
        if (status == AGENT_OK) status = append(writer, OPENAI_LITERAL("{\"id\":"));
        if (status == AGENT_OK) status = write_string(writer, call->id);
        if (status == AGENT_OK) status = append(writer, OPENAI_LITERAL(",\"type\":\"function\",\"function\":{\"name\":"));
        if (status == AGENT_OK) status = write_string(writer, call->name);
        if (status == AGENT_OK) status = append(writer, OPENAI_LITERAL(",\"arguments\":"));
        if (status == AGENT_OK) status = write_string(writer, call->arguments_json);
        if (status == AGENT_OK) status = append(writer, OPENAI_LITERAL("}}"));
    }
    if (status == AGENT_OK) status = append(writer, OPENAI_LITERAL("]"));
    return status;
}

static agent_error_t write_message(agent_openai_provider_t* provider,
                                   agent_json_writer_t* writer,
                                   const agent_message_view_t* message)
{
    agent_error_t status;
    size_t content_limit;

    if ((message->content.size != 0u && message->content.data == NULL) ||
        (message->role != AGENT_MESSAGE_ROLE_ASSISTANT &&
         message->tool_call_count != 0u)) {
        return AGENT_ERROR_INVALID;
    }
    switch (message->role) {
    case AGENT_MESSAGE_ROLE_SYSTEM:
        content_limit = AGENT_MAX_CONTEXT_BYTES;
        status = append(writer, OPENAI_LITERAL("{\"role\":\"system\",\"content\":"));
        break;
    case AGENT_MESSAGE_ROLE_USER:
        content_limit = AGENT_MAX_INPUT_BYTES;
        status = append(writer, OPENAI_LITERAL("{\"role\":\"user\",\"content\":"));
        break;
    case AGENT_MESSAGE_ROLE_ASSISTANT:
        content_limit = AGENT_MAX_MODEL_OUTPUT_BYTES;
        status = append(writer, OPENAI_LITERAL("{\"role\":\"assistant\",\"content\":"));
        break;
    case AGENT_MESSAGE_ROLE_TOOL:
        content_limit = AGENT_MAX_TOOL_OUTPUT_BYTES;
        if (message->tool_call_id.data == NULL ||
            message->tool_call_id.size == 0u) {
            return AGENT_ERROR_INVALID;
        }
        if (message->tool_call_id.size > AGENT_MAX_IDENTIFIER_BYTES) {
            return AGENT_ERROR_LIMIT;
        }
        status = append(writer, OPENAI_LITERAL("{\"role\":\"tool\",\"content\":"));
        break;
    default:
        return AGENT_ERROR_INVALID;
    }
    if (message->content.size > content_limit) {
        return AGENT_ERROR_LIMIT;
    }
    if (status == AGENT_OK && message->role == AGENT_MESSAGE_ROLE_ASSISTANT &&
        message->content.size == 0u && message->tool_call_count != 0u) {
        status = append(writer, OPENAI_LITERAL("null"));
    } else if (status == AGENT_OK) {
        status = write_string(writer, message->content);
    }
    if (status == AGENT_OK && message->role == AGENT_MESSAGE_ROLE_TOOL) {
        status = append(writer, OPENAI_LITERAL(",\"tool_call_id\":"));
        if (status == AGENT_OK) status = write_string(writer, message->tool_call_id);
    }
    if (status == AGENT_OK && message->tool_call_count != 0u) {
        status = write_history_calls(provider, writer, message);
    }
    if (status == AGENT_OK) status = append(writer, OPENAI_LITERAL("}"));
    return status;
}

static agent_error_t write_tool(agent_openai_provider_t* provider,
                                agent_json_writer_t* writer,
                                const agent_tool_view_t* tool)
{
    agent_error_t status;

    if ((tool->description.size != 0u && tool->description.data == NULL) ||
        (tool->input_schema_json.size != 0u &&
         tool->input_schema_json.data == NULL)) {
        return AGENT_ERROR_INVALID;
    }
    if (tool->description.size > AGENT_MAX_DESCRIPTION_BYTES ||
        tool->input_schema_json.size > AGENT_MAX_SCHEMA_BYTES ||
        tool->name.size > AGENT_MAX_NAME_BYTES) {
        return AGENT_ERROR_LIMIT;
    }
    if (!agent_openai_valid_tool_name(tool->name)) return AGENT_ERROR_INVALID;
    status = agent_json_validate_object(tool->input_schema_json,
                                         provider->config.max_json_depth);
    if (status != AGENT_OK) {
        return input_status(status);
    }
    status = append(writer, OPENAI_LITERAL("{\"type\":\"function\",\"function\":{\"name\":"));
    if (status == AGENT_OK) status = write_string(writer, tool->name);
    if (status == AGENT_OK && tool->description.size != 0u) {
        status = append(writer, OPENAI_LITERAL(",\"description\":"));
        if (status == AGENT_OK) status = write_string(writer, tool->description);
    }
    if (status == AGENT_OK) status = append(writer, OPENAI_LITERAL(",\"parameters\":"));
    if (status == AGENT_OK) status = append(writer, tool->input_schema_json);
    if (status == AGENT_OK) status = append(writer, OPENAI_LITERAL("}}"));
    return status;
}

agent_error_t agent_openai_write_request(agent_openai_provider_t* provider,
                                         const agent_model_request_t* request,
                                         agent_string_view_t* body)
{
    agent_json_writer_t writer;
    agent_error_t status;
    size_t i;
    bool has_message = false;

    if (body == NULL || (request->message_count != 0u && request->messages == NULL) ||
        (request->tool_count != 0u && request->tools == NULL) ||
        (request->system_prompt.size != 0u && request->system_prompt.data == NULL)) {
        return AGENT_ERROR_INVALID;
    }
    if (request->system_prompt.size > AGENT_MAX_CONTEXT_BYTES ||
        request->message_count > AGENT_MAX_PROJECTED_MESSAGES ||
        request->tool_count > AGENT_MAX_TOOLS) {
        return AGENT_ERROR_LIMIT;
    }
    agent_json_writer_init(&writer, provider->config.request_buffer,
                           provider->config.request_capacity);
    status = append(&writer, OPENAI_LITERAL("{\"model\":"));
    if (status == AGENT_OK) status = write_string(&writer, provider->config.model);
    if (status == AGENT_OK) status = append(&writer, OPENAI_LITERAL(",\"messages\":["));
    if (status == AGENT_OK && request->system_prompt.size != 0u) {
        status = append(&writer, OPENAI_LITERAL("{\"role\":\"system\",\"content\":"));
        if (status == AGENT_OK) status = write_string(&writer, request->system_prompt);
        if (status == AGENT_OK) status = append(&writer, OPENAI_LITERAL("}"));
        has_message = true;
    }
    for (i = 0u; status == AGENT_OK && i < request->message_count; ++i) {
        if (has_message) status = append(&writer, OPENAI_LITERAL(","));
        if (status == AGENT_OK) {
            status = write_message(provider, &writer, &request->messages[i]);
        }
        has_message = true;
    }
    if (status == AGENT_OK && !has_message) return AGENT_ERROR_INVALID;
    if (status == AGENT_OK) status = append(&writer, OPENAI_LITERAL("]"));
    if (status == AGENT_OK && request->tool_count != 0u) {
        status = append(&writer, OPENAI_LITERAL(",\"tools\":["));
        for (i = 0u; status == AGENT_OK && i < request->tool_count; ++i) {
            if (i != 0u) status = append(&writer, OPENAI_LITERAL(","));
            if (status == AGENT_OK) {
                status = write_tool(provider, &writer, &request->tools[i]);
            }
        }
        if (status == AGENT_OK) status = append(&writer, OPENAI_LITERAL("]"));
    }
    if (status == AGENT_OK && request->max_output_tokens != 0u) {
        status = append(&writer, provider->config.use_legacy_max_tokens
            ? OPENAI_LITERAL(",\"max_tokens\":")
            : OPENAI_LITERAL(",\"max_completion_tokens\":"));
        if (status == AGENT_OK) {
            status = agent_json_writer_u32(&writer, request->max_output_tokens);
        }
    }
    if (status == AGENT_OK) {
        status = append(&writer, OPENAI_LITERAL(",\"n\":1,\"stream\":false}"));
    }
    if (status != AGENT_OK) return status;
    return agent_json_writer_finish(&writer, body);
}
