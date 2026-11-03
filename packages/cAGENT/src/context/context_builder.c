/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* Sticky bounded collection; validate complete UTF-8 after all chunks arrive. */
#include "core/core_internal.h"
#include "core/text_internal.h"
#include "runtime/runtime_internal.h"
#include <string.h>

agent_error_t agent_context_poll(const agent_context_turn_t* turn)
{
    if (agent_cancel_token_is_set(turn->cancel)) return AGENT_ERROR_CANCELLED;
    if (turn->deadline_ms && agent_runtime_now_ms(&turn->owner->config.runtime) >= turn->deadline_ms)
        return AGENT_ERROR_TIMEOUT;
    return AGENT_OK;
}

agent_error_t agent_context_write(void* context, agent_string_view_t text)
{
    agent_context_writer_t* writer = context;
    if (writer->status != AGENT_OK) return writer->status;
    writer->status = agent_context_poll(writer->turn);
    if (writer->status != AGENT_OK) return writer->status;
    if ((text.size && !text.data) ||
        agent_bytes_overlap(text.data, text.size, writer->data, writer->capacity))
        writer->status = AGENT_ERROR_INVALID;
    else if (text.size > writer->capacity - writer->size)
        writer->status = AGENT_ERROR_CONTEXT_OVERFLOW;
    else if (text.size && memchr(text.data, '\0', text.size))
        writer->status = AGENT_ERROR_INVALID;
    else if (text.size) {
        memcpy(writer->data + writer->size, text.data, text.size);
        writer->size += text.size;
    }
    return writer->status;
}

agent_error_t agent_context_collect(agent_context_turn_t* turn, const agent_context_entry_t* entry,
                                     char* buffer, size_t capacity, agent_string_view_t* text)
{
    agent_context_writer_t writer = {turn, buffer, 0u, capacity, AGENT_OK};
    agent_text_sink_t sink = {agent_context_write, &writer};
    agent_context_request_t request = {&turn->request, turn->cancel, turn->deadline_ms};
    agent_error_t status = agent_context_poll(turn), after;
    *text = agent_string_view(NULL, 0u);
    if (status != AGENT_OK) return status;
    turn->owner->in_callback = true;
    status = entry->provider.build(entry->provider.user_data, &request, &sink);
    turn->owner->in_callback = false;
    after = agent_context_poll(turn);
    if (after != AGENT_OK) return after;
    if (writer.status != AGENT_OK) return writer.status;
    if (status != AGENT_OK) return status;
    *text = agent_string_view(buffer, writer.size);
    status = agent_text_validate(*text);
    if (status != AGENT_OK) *text = agent_string_view(NULL, 0u);
    return status;
}
