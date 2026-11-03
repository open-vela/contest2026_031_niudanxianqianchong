/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* Private Core state; this layout is never a public ABI. */
#pragma once

#include "context/context_internal.h"
#include "run/run_internal.h"
#include "session/session_internal.h"
#include "skill/skill_internal.h"
#include "tool/tool_internal.h"
#include "core/arena_internal.h"
#include <agent.h>
#include <agent/model.h>
#include <agent/memory.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { AGENT_CORE_CONFIGURING = 0, AGENT_CORE_READY, AGENT_CORE_ACTIVE } agent_core_state_t;

typedef struct agent_core_capacity {
    size_t max_tools;
    size_t max_contexts;
    size_t max_skills;
    size_t scratch_bytes;
    size_t max_projected_messages;
    size_t max_input_bytes;
    size_t max_context_bytes;
    size_t max_schema_bytes;
    size_t max_arguments_bytes;
    size_t max_tool_output_bytes;
    size_t max_model_output_bytes;
    size_t max_model_tool_calls;
    size_t max_name_bytes;
    size_t max_description_bytes;
    size_t max_identifier_bytes;
    size_t max_json_depth;
} agent_core_capacity_t;

struct agent {
    agent_config_t config;
    agent_core_state_t state;
    void* workspace;
    size_t workspace_size;
    bool owns_workspace;
    agent_model_t* model;
    bool owns_model;
    agent_tool_registry_t* tools;
    agent_context_registry_t* contexts;
    agent_skill_registry_t* skills;
    agent_session_storage_t session_storage;
    bool has_session_storage;
    agent_memory_t memory;
    bool has_memory;
    agent_cancel_token_t* active_cancel; /* Guard with runtime.cancel_sync for cross-task access. */
    agent_arena_t scratch;
    agent_policy_callback_t policy;
    void* policy_context;
    agent_event_callback_t observer;
    void* observer_context;
    agent_stats_t stats;
    bool in_callback;
};

agent_error_t agent_core_validate_config(const agent_config_t* config);
agent_error_t agent_core_layout(agent_workspace_t* workspace, const agent_config_t* config,
                                agent_t** agent);
agent_error_t agent_core_require_idle(const agent_t* agent);
void agent_core_emit(agent_t* agent, const agent_event_t* event);

#ifdef __cplusplus
}
#endif
