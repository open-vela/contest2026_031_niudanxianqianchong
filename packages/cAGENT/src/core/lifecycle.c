/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* Core workspace layout and minimal lifecycle. */

#include "core/core_internal.h"
#include "model/model_internal.h"
#include "runtime/runtime_internal.h"

#include <string.h>

typedef char agent_workspace_must_fit_core
    [(sizeof(agent_t) + AGENT_TOOL_REGISTRY_BYTES + AGENT_SKILL_REGISTRY_BYTES +
      AGENT_CONTEXT_REGISTRY_BYTES +
      (AGENT_MAX_CONTEXTS ? AGENT_ALIGNOF(agent_context_registry_t) - 1u : 0u) +
      (AGENT_MAX_SKILLS ? AGENT_ALIGNOF(agent_skill_registry_t) - 1u : 0u) +
      (AGENT_MAX_TOOLS ? AGENT_ALIGNOF(agent_tool_registry_t) - 1u : 0u) +
      AGENT_SCRATCH_BYTES <= sizeof(agent_workspace_t)) ? 1 : -1];

static agent_error_t agent_limits_validate(const agent_limits_t* limits)
{
    if (limits == NULL || limits->max_steps == 0u)
    {
        return AGENT_ERROR_INVALID;
    }
    return AGENT_OK;
}

static agent_core_capacity_t agent_core_capacity_from_profile(void)
{
    agent_core_capacity_t capacity;

    capacity.max_tools = AGENT_MAX_TOOLS;
    capacity.max_contexts = AGENT_MAX_CONTEXTS;
    capacity.max_skills = AGENT_MAX_SKILLS;
    capacity.scratch_bytes = AGENT_SCRATCH_BYTES;
    capacity.max_projected_messages = AGENT_MAX_PROJECTED_MESSAGES;
    capacity.max_input_bytes = AGENT_MAX_INPUT_BYTES;
    capacity.max_context_bytes = AGENT_MAX_CONTEXT_BYTES;
    capacity.max_schema_bytes = AGENT_MAX_SCHEMA_BYTES;
    capacity.max_arguments_bytes = AGENT_MAX_ARGUMENTS_BYTES;
    capacity.max_tool_output_bytes = AGENT_MAX_TOOL_OUTPUT_BYTES;
    capacity.max_model_output_bytes = AGENT_MAX_MODEL_OUTPUT_BYTES;
    capacity.max_model_tool_calls = AGENT_MAX_MODEL_TOOL_CALLS;
    capacity.max_name_bytes = AGENT_MAX_NAME_BYTES;
    capacity.max_description_bytes = AGENT_MAX_DESCRIPTION_BYTES;
    capacity.max_identifier_bytes = AGENT_MAX_IDENTIFIER_BYTES;
    capacity.max_json_depth = AGENT_MAX_JSON_DEPTH;
    return capacity;
}

agent_config_t agent_config_default(void)
{
    agent_config_t config;

    memset(&config, 0, sizeof(config));
    config.limits = (agent_limits_t)AGENT_LIMITS_DEFAULT;
    return config;
}

agent_error_t agent_core_validate_config(const agent_config_t* config)
{
    agent_error_t status;

    if (config == NULL || (config->system_prompt.size != 0u && config->system_prompt.data == NULL))
    {
        return AGENT_ERROR_INVALID;
    }
    status = agent_limits_validate(&config->limits);
    if (status != AGENT_OK)
    {
        return status;
    }
    return agent_runtime_validate(&config->runtime);
}

agent_error_t agent_core_layout(agent_workspace_t* workspace, const agent_config_t* config,
                                agent_t** agent)
{
    agent_arena_t arena;
    agent_core_capacity_t capacity;
    void* agent_memory;
    void* scratch_memory;
    agent_error_t status;

    if (workspace == NULL || config == NULL || agent == NULL)
    {
        return AGENT_ERROR_INVALID;
    }
    *agent = NULL;
    capacity = agent_core_capacity_from_profile();
    if (capacity.scratch_bytes == 0u || capacity.max_projected_messages == 0u)
    {
        return AGENT_ERROR_INVALID;
    }
#if AGENT_MAX_TOOLS > 0
    if (capacity.max_json_depth == 0u || capacity.max_json_depth > 32u)
    {
        return AGENT_ERROR_INVALID;
    }
#endif

    memset(workspace->bytes, 0, sizeof(workspace->bytes));
    status = agent_arena_init(&arena, workspace->bytes, sizeof(workspace->bytes));
    if (status != AGENT_OK)
    {
        return status;
    }
    status = agent_arena_take(&arena, sizeof(agent_t), AGENT_ALIGNOF(agent_t), &agent_memory);
    if (status != AGENT_OK)
    {
        return status;
    }
    *agent = agent_memory;
    status = agent_tool_registry_init(&(*agent)->tools, &arena);
    if (status == AGENT_OK)
        status = agent_skill_registry_init(&(*agent)->skills, &arena);
    if (status == AGENT_OK)
        status = agent_context_registry_init(&(*agent)->contexts, &arena);
    if (status != AGENT_OK)
    {
        memset(workspace->bytes, 0, sizeof(workspace->bytes));
        *agent = NULL;
        return status;
    }
    status = agent_arena_take(&arena, capacity.scratch_bytes, 1u, &scratch_memory);
    if (status != AGENT_OK)
    {
        memset(workspace->bytes, 0, sizeof(workspace->bytes));
        *agent = NULL;
        return status;
    }

    (*agent)->config = *config;
    (*agent)->state = AGENT_CORE_CONFIGURING;
    (*agent)->workspace = workspace;
    (*agent)->workspace_size = sizeof(*workspace);
    status = agent_arena_init(&(*agent)->scratch, scratch_memory, capacity.scratch_bytes);
    if (status != AGENT_OK)
    {
        memset(workspace->bytes, 0, sizeof(workspace->bytes));
        *agent = NULL;
    }
    return status;
}

agent_error_t agent_init(agent_t** agent, agent_workspace_t* workspace,
                         const agent_config_t* config)
{
    agent_error_t status;

    if (agent == NULL)
    {
        return AGENT_ERROR_INVALID;
    }
    *agent = NULL;
    status = agent_core_validate_config(config);
    if (status != AGENT_OK)
    {
        return status;
    }
    return agent_core_layout(workspace, config, agent);
}

agent_t* agent_create(const agent_config_t* config)
{
    agent_workspace_t* workspace;
    agent_t* agent;

    if (agent_core_validate_config(config) != AGENT_OK || config->runtime.allocator.alloc == NULL)
    {
        return NULL;
    }
    workspace = agent_runtime_alloc(&config->runtime, sizeof(*workspace));
    if (workspace == NULL)
    {
        return NULL;
    }
    if (agent_init(&agent, workspace, config) != AGENT_OK)
    {
        agent_runtime_free(&config->runtime, workspace);
        return NULL;
    }
    agent->owns_workspace = true;
    return agent;
}

agent_error_t agent_start(agent_t* agent)
{
    agent_error_t status = agent_core_require_idle(agent);
    if (status != AGENT_OK) return status;
    if (agent->state != AGENT_CORE_CONFIGURING)
    {
        return AGENT_ERROR_STATE;
    }
    agent->state = AGENT_CORE_READY;
    return AGENT_OK;
}

void agent_destroy(agent_t* agent)
{
    agent_runtime_t runtime;
    agent_workspace_t* workspace;
    bool owns_workspace;

    if (agent == NULL || agent->state == AGENT_CORE_ACTIVE || agent->in_callback)
    {
        return;
    }

    runtime = agent->config.runtime;
    workspace = agent->workspace;
    owns_workspace = agent->owns_workspace;
    if (agent->model != NULL && agent->owns_model)
    {
        agent_model_destroy(agent->model);
    }
    memset(workspace->bytes, 0, sizeof(workspace->bytes));
    if (owns_workspace)
    {
        agent_runtime_free(&runtime, workspace);
    }
}

agent_error_t agent_core_require_idle(const agent_t* agent)
{
    if (agent == NULL)
    {
        return AGENT_ERROR_INVALID;
    }
    if (agent->state == AGENT_CORE_ACTIVE || agent->in_callback)
    {
        return AGENT_ERROR_BUSY;
    }
    return AGENT_OK;
}

agent_error_t agent_set_limits(agent_t* agent, const agent_limits_t* limits)
{
    agent_error_t status = agent_core_require_idle(agent);

    if (status != AGENT_OK)
    {
        return status;
    }
    status = agent_limits_validate(limits);
    if (status != AGENT_OK)
    {
        return status;
    }
    agent->config.limits = *limits;
    return AGENT_OK;
}
