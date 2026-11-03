/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <agent.h>
#include <string.h>

typedef struct {
    agent_event_type_t type;
    uint32_t model_calls;
    agent_error_t status;
    char tool_name[AGENT_MAX_NAME_BYTES + 1u];
    char tool_call_id[AGENT_MAX_IDENTIFIER_BYTES + 1u];
} smart_home_ui_event_t;

static inline void smart_home_ui_copy_view(char *out, size_t capacity, agent_string_view_t text)
{
    size_t size = text.size < capacity - 1u ? text.size : capacity - 1u;
    if (!text.data) size = 0u;
    if (size) memcpy(out, text.data, size);
    out[size] = '\0';
}

static inline void smart_home_ui_event_copy(smart_home_ui_event_t *out, const agent_event_t *event)
{
    memset(out, 0, sizeof(*out));
    out->type = event->type;
    out->model_calls = event->summary.model_calls;
    out->status = event->status;
    smart_home_ui_copy_view(out->tool_name, sizeof(out->tool_name), event->tool_call.name);
    smart_home_ui_copy_view(out->tool_call_id, sizeof(out->tool_call_id), event->tool_call.id);
}
