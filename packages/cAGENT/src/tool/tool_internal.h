/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* Private tool registry, schema validation, and guarded dispatch. */
#pragma once

#include "core/arena_internal.h"
#include <agent/config.h>
#include <agent/model.h>
#include <agent/tool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct agent_tool_registry {
    size_t count;
    agent_tool_t entries[AGENT_MAX_TOOLS ? AGENT_MAX_TOOLS : 1u];
} agent_tool_registry_t;

#define AGENT_TOOL_REGISTRY_BYTES (AGENT_MAX_TOOLS ? sizeof(agent_tool_registry_t) : 0u)

/* Invocation facts; output is caller-owned and may be partial on failure. */
typedef struct {
    agent_error_t status;
    agent_error_t handler_status;
    agent_error_t output_status;
    bool handler_called;
    agent_string_view_t output;
} agent_tool_execution_t;

/* Internal driver observation at actual handler boundaries; never application hooks. */
typedef struct {
    void (*begin)(void* data, const agent_tool_context_t* context);
    void (*end)(void* data, const agent_tool_context_t* context, const agent_tool_execution_t* execution);
    void* data;
} agent_tool_observer_t;

agent_error_t agent_tool_registry_init(agent_tool_registry_t** registry, agent_arena_t* arena);
const agent_tool_t* agent_tool_registry_find(const agent_tool_registry_t* registry,
                                             agent_string_view_t name);
agent_error_t agent_tool_registry_project(const agent_tool_registry_t* registry,
                                          agent_tool_view_t* views, size_t capacity, size_t* count);
agent_error_t agent_tool_object_validate(agent_string_view_t object, size_t maximum);
agent_error_t agent_tool_text_validate(agent_string_view_t text);
bool agent_tool_valid_name(agent_string_view_t name);
bool agent_tool_overlaps(const void* a, size_t a_size, const void* b, size_t b_size);
bool agent_tool_registry_buffer_safe(const agent_tool_registry_t* registry, const void* data,
                                     size_t size);
/* Driver-only, ACTIVE state; Run owns aggregate budget, events, and Session pairing. */
agent_error_t agent_tool_invoke(agent_t* agent, const agent_tool_context_t* context,
                                uint32_t remaining_calls, char* output, size_t capacity,
                                agent_tool_execution_t* execution);
agent_error_t agent_tool_invoke_observed(agent_t* agent, const agent_tool_context_t* context,
    uint32_t remaining_calls, char* output, size_t capacity, agent_tool_execution_t* execution,
    const agent_tool_observer_t* observer);

#ifdef __cplusplus
}
#endif
