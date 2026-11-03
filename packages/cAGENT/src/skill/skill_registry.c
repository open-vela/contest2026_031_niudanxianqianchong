/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* Stable-priority borrowed skills with whole-contribution admission. */
#include "core/core_internal.h"
#include "core/text_internal.h"
#include <string.h>

agent_error_t agent_skill_registry_init(agent_skill_registry_t** registry, agent_arena_t* arena)
{
    if (!registry || !arena) return AGENT_ERROR_INVALID;
    *registry = NULL;
#if AGENT_MAX_SKILLS > 0
    {
        void* memory;
        agent_error_t status = agent_arena_take(arena, sizeof(agent_skill_registry_t),
                                                AGENT_ALIGNOF(agent_skill_registry_t), &memory);
        if (status != AGENT_OK) return status;
        memset(memory, 0, sizeof(agent_skill_registry_t));
        *registry = memory;
    }
#endif
    return AGENT_OK;
}

static bool same_name(agent_string_view_t a, agent_string_view_t b)
{
    return a.size == b.size && memcmp(a.data, b.data, a.size) == 0;
}

static agent_error_t admit(const agent_t* agent, agent_string_view_t text, size_t maximum)
{
    if (text.size > maximum) return AGENT_ERROR_LIMIT;
    if (agent_bytes_overlap(text.data, text.size, agent->workspace, agent->workspace_size))
        return AGENT_ERROR_INVALID;
    return agent_text_validate(text);
}

agent_error_t agent_register_skill(agent_t* agent, const agent_skill_t* skill)
{
    size_t i, index;
    agent_error_t status = agent_core_require_idle(agent);
    if (status != AGENT_OK) return status;
    if (!agent->skills) return AGENT_ERROR_NOT_SUPPORTED;
    if (!skill || !agent_text_name_valid(skill->name, AGENT_MAX_NAME_BYTES))
        return AGENT_ERROR_INVALID;
    status = admit(agent, skill->name, AGENT_MAX_NAME_BYTES);
    if (status == AGENT_OK) status = admit(agent, skill->description, AGENT_MAX_DESCRIPTION_BYTES);
    if (status == AGENT_OK) status = admit(agent, skill->content, AGENT_MAX_CONTEXT_BYTES);
    if (status != AGENT_OK) return status;
    for (i = 0u; i < agent->skills->count; ++i)
        if (same_name(skill->name, agent->skills->entries[i].name)) return AGENT_ERROR_EXISTS;
    if (agent->skills->count == AGENT_MAX_SKILLS) return AGENT_ERROR_CAPACITY;
    {
        agent_skill_t copy = *skill;
        index = agent->skills->count;
        while (index && agent->skills->entries[index - 1u].priority < copy.priority) {
            agent->skills->entries[index] = agent->skills->entries[index - 1u];
            --index;
        }
        agent->skills->entries[index] = copy;
        ++agent->skills->count;
    }
    return AGENT_OK;
}

agent_error_t agent_unregister_skill(agent_t* agent, agent_string_view_t name)
{
    size_t i;
    agent_error_t status = agent_core_require_idle(agent);
    if (status != AGENT_OK) return status;
    if (!agent_text_name_valid(name, AGENT_MAX_NAME_BYTES)) return AGENT_ERROR_INVALID;
    if (!agent->skills) return AGENT_ERROR_NOT_SUPPORTED;
    for (i = 0u; i < agent->skills->count; ++i) {
        if (!same_name(name, agent->skills->entries[i].name)) continue;
        memmove(&agent->skills->entries[i], &agent->skills->entries[i + 1u],
                (agent->skills->count - i - 1u) * sizeof(agent_skill_t));
        memset(&agent->skills->entries[--agent->skills->count], 0, sizeof(agent_skill_t));
        return AGENT_OK;
    }
    return AGENT_ERROR_NOT_FOUND;
}

agent_error_t agent_skill_project(const agent_skill_registry_t* registry,
                                  size_t max_bytes, const agent_text_sink_t* output)
{
    size_t i, required_bytes = 0u, required_count = 0u, used = 0u;
    if (!output || !output->write) return AGENT_ERROR_INVALID;
    if (!registry) return AGENT_OK;
    for (i = 0u; i < registry->count; ++i) {
        const agent_skill_t* skill = &registry->entries[i];
        if (!skill->required || !skill->content.size) continue;
        if (skill->content.size > max_bytes - required_bytes)
            return AGENT_ERROR_CONTEXT_OVERFLOW;
        required_bytes += skill->content.size;
        if (required_count++) {
            if (max_bytes - required_bytes < 2u) return AGENT_ERROR_CONTEXT_OVERFLOW;
            required_bytes += 2u;
        }
    }
    for (i = 0u; i < registry->count; ++i) {
        const agent_skill_t* skill = &registry->entries[i];
        size_t separator = used ? 2u : 0u, reserve, available = max_bytes - used;
        agent_error_t status;
        if (!skill->content.size) continue;
        if (skill->required) {
            required_bytes -= skill->content.size;
            if (--required_count) required_bytes -= 2u;
        }
        reserve = required_bytes;
        if (required_count) {
            if (reserve > available || available - reserve < 2u) {
                if (skill->required) return AGENT_ERROR_CONTEXT_OVERFLOW;
                continue;
            }
            reserve += 2u;
        }
        if (reserve > available || separator > available - reserve ||
            skill->content.size > available - reserve - separator) {
            if (skill->required) return AGENT_ERROR_CONTEXT_OVERFLOW;
            continue;
        }
        if (separator) {
            status = output->write(output->context, agent_string_view("\n\n", 2u));
            if (status != AGENT_OK) return status;
        }
        status = output->write(output->context, skill->content);
        if (status != AGENT_OK) return status;
        used += separator + skill->content.size;
    }
    return AGENT_OK;
}
