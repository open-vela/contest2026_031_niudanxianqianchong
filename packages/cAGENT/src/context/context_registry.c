/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* One bounded namespace for dynamic contributions and selected Memory documents. */
#include "core/core_internal.h"
#include "core/text_internal.h"
#include <string.h>

agent_error_t agent_context_registry_init(agent_context_registry_t** registry, agent_arena_t* arena)
{
    if (!registry || !arena) return AGENT_ERROR_INVALID;
    *registry = NULL;
#if AGENT_MAX_CONTEXTS > 0
    {
        void* memory;
        agent_error_t status = agent_arena_take(arena, sizeof(agent_context_registry_t),
                                                AGENT_ALIGNOF(agent_context_registry_t), &memory);
        if (status != AGENT_OK) return status;
        memset(memory, 0, sizeof(agent_context_registry_t));
        *registry = memory;
    }
#endif
    return AGENT_OK;
}

static bool same_name(agent_string_view_t a, agent_string_view_t b)
{
    return a.size == b.size && memcmp(a.data, b.data, a.size) == 0;
}

static agent_error_t insert(agent_t* agent, agent_context_entry_t entry)
{
    size_t i;
    agent_context_registry_t* registry = agent->contexts;
    agent_context_provider_t* p = &entry.provider;
    size_t maximum = AGENT_MAX_CONTEXT_BYTES;
    if (!registry) return AGENT_ERROR_NOT_SUPPORTED;
    if (!agent_text_name_valid(p->name, AGENT_MAX_NAME_BYTES) ||
        agent_bytes_overlap(p->name.data, p->name.size, agent->workspace, agent->workspace_size) ||
        (p->placement != AGENT_CONTEXT_INSTRUCTIONS && p->placement != AGENT_CONTEXT_REFERENCE))
        return AGENT_ERROR_INVALID;
    if (p->placement == AGENT_CONTEXT_REFERENCE && maximum > AGENT_MAX_INPUT_BYTES)
        maximum = AGENT_MAX_INPUT_BYTES;
    if (!p->max_bytes) p->max_bytes = maximum;
    if (!p->max_bytes || p->max_bytes > maximum) return AGENT_ERROR_LIMIT;
    for (i = 0u; i < registry->count; ++i)
        if (same_name(p->name, registry->entries[i].provider.name)) return AGENT_ERROR_EXISTS;
    if (registry->count == AGENT_MAX_CONTEXTS) return AGENT_ERROR_CAPACITY;
    i = registry->count;
    while (i && registry->entries[i - 1u].provider.priority < p->priority) {
        registry->entries[i] = registry->entries[i - 1u];
        --i;
    }
    registry->entries[i] = entry;
    ++registry->count;
    return AGENT_OK;
}

agent_error_t agent_register_context(agent_t* agent, const agent_context_provider_t* provider)
{
    agent_context_entry_t entry = {0};
    agent_error_t status = agent_core_require_idle(agent);
    if (status != AGENT_OK) return status;
    if (!provider || !provider->build) return AGENT_ERROR_INVALID;
    entry.provider = *provider;
    return insert(agent, entry);
}

agent_error_t agent_register_memory_context(agent_t* agent, const agent_memory_context_t* source)
{
    agent_context_entry_t entry = {0};
    agent_error_t status = agent_core_require_idle(agent);
    if (status != AGENT_OK) return status;
    if (!source || !source->max_bytes || source->key.kind < AGENT_MEMORY_SOUL ||
        source->key.kind > AGENT_MEMORY_NOTE) return AGENT_ERROR_INVALID;
    if (source->key.kind == AGENT_MEMORY_NOTE) {
        if (!source->key.id.size || source->key.id.size > AGENT_MAX_IDENTIFIER_BYTES ||
            agent_bytes_overlap(source->key.id.data, source->key.id.size,
                                agent->workspace, agent->workspace_size)) return AGENT_ERROR_INVALID;
        status = agent_text_validate(source->key.id);
        if (status != AGENT_OK) return status;
    } else if (source->key.id.size) return AGENT_ERROR_INVALID;
    entry.memory = true;
    entry.key = source->key;
    entry.provider.name = source->name;
    entry.provider.priority = source->priority;
    entry.provider.required = source->required;
    entry.provider.max_bytes = source->max_bytes;
    entry.provider.placement = source->key.kind == AGENT_MEMORY_SOUL ?
                               AGENT_CONTEXT_INSTRUCTIONS : AGENT_CONTEXT_REFERENCE;
    return insert(agent, entry);
}

agent_error_t agent_unregister_context(agent_t* agent, agent_string_view_t name)
{
    size_t i;
    agent_error_t status = agent_core_require_idle(agent);
    if (status != AGENT_OK) return status;
    if (!agent_text_name_valid(name, AGENT_MAX_NAME_BYTES)) return AGENT_ERROR_INVALID;
    if (!agent->contexts) return AGENT_ERROR_NOT_SUPPORTED;
    for (i = 0u; i < agent->contexts->count; ++i) {
        if (!same_name(name, agent->contexts->entries[i].provider.name)) continue;
        memmove(&agent->contexts->entries[i], &agent->contexts->entries[i + 1u],
                (agent->contexts->count - i - 1u) * sizeof(agent_context_entry_t));
        memset(&agent->contexts->entries[--agent->contexts->count], 0, sizeof(agent_context_entry_t));
        return AGENT_OK;
    }
    return AGENT_ERROR_NOT_FOUND;
}
