/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* Private context registry and bounded projection. */
#pragma once

#include "core/arena_internal.h"
#include <agent/context.h>
#include <agent/model.h>
#include "session/session_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    agent_context_provider_t provider;
    bool memory;
    agent_memory_key_t key;
} agent_context_entry_t;

typedef struct agent_context_registry {
    size_t count;
    agent_context_entry_t entries[AGENT_MAX_CONTEXTS ? AGENT_MAX_CONTEXTS : 1u];
} agent_context_registry_t;

#define AGENT_CONTEXT_REGISTRY_BYTES (AGENT_MAX_CONTEXTS ? sizeof(agent_context_registry_t) : 0u)
#define AGENT_CONTEXT_PARTS (1u + AGENT_MAX_SKILLS + AGENT_MAX_CONTEXTS)

typedef struct {
    agent_string_view_t name;
    agent_error_t status;
} agent_context_omission_t;

typedef struct {
    size_t count;
    agent_context_omission_t entries[AGENT_CONTEXT_PARTS];
} agent_context_report_t;

typedef struct {
    agent_string_view_t text;
    agent_context_placement_t placement;
    bool required;
    agent_string_view_t name;
} agent_context_part_t;

typedef struct {
    agent_t* owner;
    agent_request_t request;
    agent_limits_t limits;
    const agent_cancel_token_t* cancel;
    uint64_t deadline_ms;
    size_t part_count;
    size_t text_cost;
    size_t end;
    agent_context_part_t parts[AGENT_CONTEXT_PARTS];
    agent_context_report_t report;
} agent_context_turn_t;

/* Driver-owned descriptor, outside scratch; release is LIFO and invalidates all request views. */
typedef struct {
    agent_t* owner;
    size_t mark;
    size_t end;
    agent_model_request_t request;
    agent_context_report_t report;
} agent_context_projection_t;

typedef struct {
    agent_context_turn_t* turn;
    char* data;
    size_t size;
    size_t capacity;
    agent_error_t status;
} agent_context_writer_t;

agent_error_t agent_context_registry_init(agent_context_registry_t** registry, agent_arena_t* arena);
agent_error_t agent_context_poll(const agent_context_turn_t* turn);
agent_error_t agent_context_write(void* context, agent_string_view_t text);
agent_error_t agent_context_collect(agent_context_turn_t* turn, const agent_context_entry_t* entry,
                                     char* buffer, size_t capacity, agent_string_view_t* text);
/* ACTIVE-driver only; snapshots survive until the owning turn scratch is reset. */
agent_error_t agent_context_prepare(agent_t* agent, const agent_request_t* request,
                                    const agent_cancel_token_t* cancel, uint64_t deadline_ms,
                                    agent_context_turn_t** turn);
/* reserve_bytes excludes tail space from projection; the driver owns response retention across release. */
agent_error_t agent_context_project(agent_context_turn_t* turn, const agent_session_turn_t* session,
                                    uint32_t remaining_tool_calls, size_t reserve_bytes,
                                    agent_context_projection_t* projection);
agent_error_t agent_context_release(agent_context_projection_t* projection);

#ifdef __cplusplus
}
#endif
